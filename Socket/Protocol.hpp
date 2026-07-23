#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <span>
#include <stdexcept>
#include "Memory.hpp"
#include "Tools.hpp"

namespace Socket
{
    enum class ReadStatus : uint8_t
    {
        New = 0,   // Data read successfully and is newer than last observed.
        Old = 1,   // Data read successfully but is stale.
        Empty = 2, // No data available.
        Closed = 3 // Channel is closed.
    };

    class Protocol
    {
    public:
        static constexpr uint64_t s_ringWrapMarker = UINT64_MAX;
        static constexpr uint64_t s_magic = 0x4846542148465421ULL;
        static constexpr int32_t HeaderLength = Tools::Memory::CacheLine;

        struct alignas(Tools::Memory::CacheLine) Header64
        {
            std::atomic<uint64_t> Sequence; 
            std::atomic<uint64_t> Magic; 
            int32_t Length;
            int32_t ObjectType;
        };

        static inline int32_t GetAlignedEntryLength(int32_t valueLength)
        {
            return Tools::Memory::GetAlignedLength(HeaderLength + valueLength, Tools::Memory::CacheLine);
        }

        template <typename T> requires Tools::PlainOldData<T>
        static inline T* GetValuePointer(uint8_t* slotPtr)
        {
            return reinterpret_cast<T*>(slotPtr + HeaderLength);
        }

        static inline bool IsThisNewerThan(uint64_t current, uint64_t other)
        {
            return current > other;
        }

        static inline bool IsWriteInProgress(uint64_t sequence)
        {
            return (sequence & 1ULL) != 0ULL;
        }

        // -------- Lock primitives (slot path) --------

        static inline void AcquireLock(Header64* hdr)
        {
            uint64_t seq = hdr->Sequence.load(std::memory_order_acquire);
            hdr->Sequence.store(seq + 1ULL, std::memory_order_release);
        }

        static inline void ReleaseLock(Header64* hdr)
        {
            uint64_t seq = hdr->Sequence.load(std::memory_order_acquire);
            hdr->Sequence.store(seq + 1ULL, std::memory_order_release);
        }

        static inline uint64_t ReadSequence(Header64* hdr)
        {
            return hdr->Sequence.load(std::memory_order_acquire);
        }

        // -------- Slot Write --------

        template <typename T> requires Tools::PlainOldData<T>
        static void Write(const T& obj, Header64* dstHdr, int32_t dstLen)
        {
            const uint8_t* srcObj = reinterpret_cast<const uint8_t*>(&obj);
            Write(srcObj, static_cast<int32_t>(sizeof(T)), dstHdr, dstLen);
        }

        static void Write(const uint8_t* srcObj, int32_t srcObjLen, Header64* dstHdr, int32_t dstLen)
        {
            // 1. Validate Available Space
            if (dstLen < HeaderLength)
                throw std::out_of_range("dstLen");

            uint8_t* dstObj = reinterpret_cast<uint8_t*>(dstHdr) + HeaderLength;
            int32_t dstObjLen = dstLen - HeaderLength;

            uint64_t seq = dstHdr->Sequence.load(std::memory_order_acquire);

            // 2. Acquire: even -> odd
            dstHdr->Sequence.store(seq + 1ULL, std::memory_order_release);

            // 3. Header metadata + payload copy
            dstHdr->Magic.store(s_magic, std::memory_order_release);
            dstHdr->Length = srcObjLen;
            Copy(srcObj, srcObjLen, dstObj, dstObjLen);

            // 4. Release: odd -> even (publish)
            dstHdr->Sequence.store(seq + 2ULL, std::memory_order_release);
        }

        template <typename T> requires Tools::PlainOldData<T>
        static void RecoveryWrite(const T& obj, Header64* dstHdr, int32_t dstLen)
        {
            RecoveryWrite(reinterpret_cast<const uint8_t*>(&obj), static_cast<int32_t>(sizeof(T)), dstHdr, dstLen);
        }

        // Like Write() but re-bases the seqlock instead of assuming an even start: a writer that died
        // mid-write can leave it odd. For recovering a slot whose original writer is confirmed dead.
        static void RecoveryWrite(const uint8_t* srcObj, int32_t srcObjLen, Header64* dstHdr, int32_t dstLen)
        {
            if (dstLen < HeaderLength)
                throw std::out_of_range("dstLen");

            uint8_t* dstObj = reinterpret_cast<uint8_t*>(dstHdr) + HeaderLength;
            int32_t dstObjLen = dstLen - HeaderLength;

            // base = next even >= seq+1: seq+1 when seq is odd (crash mid-write), seq when even.
            uint64_t seq = dstHdr->Sequence.load(std::memory_order_acquire);
            uint64_t base = (seq + 1ULL) & ~1ULL;

            dstHdr->Sequence.store(base + 1ULL, std::memory_order_release); // odd: write in progress
            dstHdr->Magic.store(s_magic, std::memory_order_release);
            dstHdr->Length = srcObjLen;
            Copy(srcObj, srcObjLen, dstObj, dstObjLen);
            dstHdr->Sequence.store(base + 2ULL, std::memory_order_release); // even: published, > old
        }

        // -------- Slot Read --------

        static ReadStatus TryRead(Header64* srcHdr, std::span<uint8_t> dstObj, std::span<const uint8_t>& rdstObj, uint64_t& lastEvenSeq)
        {
            int32_t srcObjLen = 0;
            ReadStatus status = TryRead(srcHdr, dstObj.data(), static_cast<int32_t>(dstObj.size()), srcObjLen, lastEvenSeq);
            rdstObj = std::span<const uint8_t>(dstObj.data(), static_cast<size_t>(srcObjLen));
            return status;
        }

        template <typename T> requires Tools::PlainOldData<T>
        static ReadStatus TryRead(Header64* srcHdr, T& obj, uint64_t& lastEvenSeq)
        {
            int32_t sizeOfT = static_cast<int32_t>(sizeof(T));
            int32_t srcObjLen = 0;
            ReadStatus status = TryRead(srcHdr, reinterpret_cast<uint8_t*>(&obj), sizeOfT, srcObjLen, lastEvenSeq);

            if ((status == ReadStatus::New || status == ReadStatus::Old) && srcObjLen < sizeOfT)
                throw std::runtime_error("Size mismatch.");

            return status;
        }

        static ReadStatus TryRead(Header64* srcHdr, uint8_t* dstObj, int32_t dstObjLen, int32_t& srcObjLen, uint64_t& lastEvenSeq)
        {
            uint8_t* srcObj = reinterpret_cast<uint8_t*>(srcHdr) + HeaderLength;

            auto readSeq = [&]() -> uint64_t
            {
                uint64_t seq = srcHdr->Sequence.load(std::memory_order_acquire);
                uint64_t magic = srcHdr->Magic.load(std::memory_order_acquire);

                if (magic != s_magic) [[unlikely]]
                    return 0ULL;
                
                return seq;
            };

            while (true)
            {
                // 1. Read Initial Sequence
                uint64_t seq1 = readSeq();

                // 2. Check if empty
                if (seq1 == 0ULL)
                {
                    srcObjLen = 0;
                    return ReadStatus::Empty;
                }

                // 3. Spin if Write is in progress (Odd sequence)
                if (IsWriteInProgress(seq1))
                {
                    _mm_pause();
                    continue;
                }

                srcObjLen = srcHdr->Length;

                // 4. Pre-Copy Validation
                uint64_t seq2 = srcHdr->Sequence.load(std::memory_order_acquire);
                if (seq1 != seq2)
                    continue;

                // 5. Copy Payload Optimistically
                Copy(srcObj, srcObjLen, dstObj, dstObjLen);
                std::atomic_thread_fence(std::memory_order_acquire);
                
                // 6. Post-Copy Validation
                seq2 = srcHdr->Sequence.load(std::memory_order_acquire);
                if (seq1 == seq2)
                {
                    // 7. Commit and return status
                    if (IsThisNewerThan(seq2, lastEvenSeq))
                    {
                        lastEvenSeq = seq2;
                        return ReadStatus::New;
                    }
                    return ReadStatus::Old;
                }
            }
        }

        // -------- Ring Write --------

        static void WriteToRing(std::span<const uint8_t> srcObj, uint8_t*& dst, uint8_t* start, uint8_t* end, uint64_t& writerSeqEven)
        {
            WriteToRing(srcObj.data(), static_cast<int32_t>(srcObj.size()), dst, start, end, writerSeqEven);
        }

        template <typename T> requires Tools::PlainOldData<T>
        static void WriteToRing(const T& obj, uint8_t*& dst, uint8_t* start, uint8_t* end, uint64_t& writerSeqEven)
        {
            WriteToRing(reinterpret_cast<const uint8_t*>(&obj), static_cast<int32_t>(sizeof(T)), dst, start, end, writerSeqEven);
        }

        static void WriteToRing(const uint8_t* srcObj, int32_t srcObjLen, uint8_t*& dst, uint8_t* start, uint8_t* end, uint64_t& writerSeqEven)
        {
            // 1. Check ring alignment
            if (((uintptr_t)start & 63) != 0 || ((uintptr_t)end & 63) != 0)
                throw std::runtime_error("Ring alignment error.");

            auto writeSeq = [](Header64* hdr, uint64_t seq) -> void
            {
                hdr->Magic.store(s_magic, std::memory_order_release);
                hdr->Sequence.store(seq, std::memory_order_release);
            };

            // 2. Calculate Required Space
            int32_t ringSize = static_cast<int32_t>(end - start);
            int32_t alignedEntryLength = GetAlignedEntryLength(srcObjLen);

            if (alignedEntryLength >= ringSize)
                throw std::out_of_range("ringSize");

            int32_t remaining = static_cast<int32_t>(end - dst);
            int32_t require = alignedEntryLength + HeaderLength;

            // 3. Handle Ring Buffer Wrap-Around (if space is short)
            if (remaining < require)
            {
                // 3a. Pre-zero sequence at new start to mark it as empty
                Header64* nextStartHdr = reinterpret_cast<Header64*>(start + alignedEntryLength);
                writeSeq(nextStartHdr, 0ULL);

                // 3b. Write actual data payload at the start of the ring
                WriteToRing(srcObj, srcObjLen, reinterpret_cast<Header64*>(start), ringSize, writerSeqEven);

                // 3c. Stamp the wrap marker at the previous tail
                Header64* currentHdr = reinterpret_cast<Header64*>(dst);
                writeSeq(currentHdr, s_ringWrapMarker);

                dst = start + alignedEntryLength;
                return;
            }

            // 4. Handle Linear Write
            uint8_t* nextPos = dst + alignedEntryLength;
            
            // 4a. Pre-zero next sequence block
            Header64* nextHdr = reinterpret_cast<Header64*>(nextPos);
            writeSeq(nextHdr, 0ULL);

            // 4b. Write actual payload
            WriteToRing(srcObj, srcObjLen, reinterpret_cast<Header64*>(dst), remaining, writerSeqEven);

            // 4c. Advance pointer
            dst = nextPos;
            if (dst == end)
                dst = start;
        }

    private:
        static void WriteToRing(const uint8_t* srcObj, int32_t srcObjLen, Header64* dstHdr, int32_t dstLen, uint64_t& writerSeq)
        {
            // 1. Validate Available Space
            if (dstLen < HeaderLength)
                throw std::out_of_range("dstLen");

            uint8_t* dstObj = reinterpret_cast<uint8_t*>(dstHdr) + HeaderLength;
            int32_t dstObjLen = dstLen - HeaderLength;

            // 2. Acquire: even -> odd. Source of truth is the writer's monotonic cursor.
            dstHdr->Sequence.store(writerSeq + 1ULL, std::memory_order_release);

            // 3. Header metadata + payload copy
            dstHdr->Magic.store(s_magic, std::memory_order_release);
            dstHdr->Length = srcObjLen;
            Copy(srcObj, srcObjLen, dstObj, dstObjLen);

            // 4. Release: odd -> even (publish). Advance the caller's cursor.
            writerSeq += 2ULL;
            dstHdr->Sequence.store(writerSeq, std::memory_order_release);
        }

    public:
        // -------- Ring Read --------

        static ReadStatus TryReadFromRing(uint8_t*& src, uint8_t* start, uint8_t* end, std::span<uint8_t> dstObj, std::span<const uint8_t>& rdstObj, uint64_t& lastReadEvenSeq)
        {
            int32_t srcObjLen = 0;
            ReadStatus status = TryReadFromRing(src, start, end, dstObj.data(), static_cast<int32_t>(dstObj.size()), srcObjLen, lastReadEvenSeq);
            rdstObj = std::span<const uint8_t>(dstObj.data(), static_cast<size_t>(srcObjLen));
            return status;
        }

        static ReadStatus TryReadFromRing(uint8_t*& src, uint8_t* start, uint8_t* end, uint8_t* dstObj, int32_t dstObjLen, int32_t& srcObjLen, uint64_t& lastReadEvenSeq)
        {
            if (((uintptr_t)start & 63) != 0 || ((uintptr_t)end & 63) != 0)
                throw std::runtime_error("Ring alignment error.");

            Header64* srcHdr = reinterpret_cast<Header64*>(src);

            auto readSeq = [&]() -> uint64_t
            {
                uint64_t seq = srcHdr->Sequence.load(std::memory_order_acquire);
                uint64_t magic = srcHdr->Magic.load(std::memory_order_acquire);
                if (magic != s_magic || seq == s_ringWrapMarker) [[unlikely]]
                {
                    srcHdr = reinterpret_cast<Header64*>(start);
                    seq = srcHdr->Sequence.load(std::memory_order_acquire);
                }
                return seq;
            };

            while (true)
            {
                // 1. Initial Read of Sequence
                uint64_t seq0 = readSeq();

                // 2. Spin while Writer is Active
                while (IsWriteInProgress(seq0))
                {
                    _mm_pause();
                    seq0 = readSeq();
                }

                // 3. Bail early if Stale or Empty
                if (!IsThisNewerThan(seq0, lastReadEvenSeq))
                {
                    srcObjLen = 0;
                    return ReadStatus::Empty;
                }

                int32_t len = srcHdr->Length;

                // 4. Pre-Copy Validation
                uint64_t seq1 = readSeq();
                if (seq0 != seq1)
                    continue;

                // 5. Copy Data Optimistically
                Copy(reinterpret_cast<uint8_t*>(srcHdr) + HeaderLength, len, dstObj, dstObjLen);
                std::atomic_thread_fence(std::memory_order_acquire);

                // 6. Post-Copy Validation
                uint64_t seq2 = readSeq();
                if (seq0 != seq2)
                    continue;

                // 7. Commit Read and Advance Pointers
                lastReadEvenSeq = seq0;
                srcObjLen = len;

                src = reinterpret_cast<uint8_t*>(srcHdr);
                src += GetAlignedEntryLength(srcObjLen);

                if (src == end)
                    src = start;

                return ReadStatus::New;
            }
        }

        // -------- Status probes --------

        static ReadStatus GetReadStatus(Header64* srcHdr, uint64_t lastEvenSeq)
        {
            uint64_t seq = srcHdr->Sequence.load(std::memory_order_acquire);

            if (seq == 0ULL)
                return ReadStatus::Empty;

            return IsThisNewerThan(seq, lastEvenSeq) ? ReadStatus::New : ReadStatus::Old;
        }

        static ReadStatus GetReadStatusFromRing(Header64* srcHdr, uint8_t* start, uint8_t* end, uint64_t lastEvenSeq)
        {
            if (((uintptr_t)start & 63) != 0 || ((uintptr_t)end & 63) != 0)
                throw std::runtime_error("Ring alignment error.");

            uint64_t seq = srcHdr->Sequence.load(std::memory_order_acquire);

            if (seq == s_ringWrapMarker)
                srcHdr = reinterpret_cast<Header64*>(start);

            return GetReadStatus(srcHdr, lastEvenSeq) == ReadStatus::New ? ReadStatus::New : ReadStatus::Empty;
        }

    private:
        static inline void Copy(const uint8_t* srcObj, int32_t srcObjLen, uint8_t* dstObj, int32_t dstObjLen)
        {
            if (srcObjLen > dstObjLen || srcObjLen < 0) [[unlikely]]
                throw std::out_of_range("Protocol.Copy Failed");
            std::memcpy(dstObj, srcObj, static_cast<size_t>(srcObjLen));
        }
    };
}