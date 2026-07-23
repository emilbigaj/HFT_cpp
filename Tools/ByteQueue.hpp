//BEGIN_FILE HFT/Tools/ByteQueue.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <span>
#include <cstring>
#include <immintrin.h>
#include "Tools.hpp"
#include "Memory.hpp"

namespace Tools
{
	/// <summary>
	/// Ultra-fast single-threaded byte ring queue backed by unmanaged memory.
	///
	/// DESIGN PHILOSOPHY:
	/// 1. Zero-Allocation: Fixed unmanaged memory allocation upfront to avoid GC/heap pressure.
	/// 2. Contiguous Payloads: Guarantees that every payload allows you to get a contiguous Span back
	///    by leaving "Gaps" at the end of the buffer if a payload won't fit, jumping the writer to 0.
	/// 3. Power-of-Two: Capacity is 2^N to allow fast bitwise masking instead of slow modulo division.
	/// 4. SPSC Thread-Safe: Uses relaxed and acquire-release atomics for wait-free concurrent operations.
	/// 5. HugePages & NUMA Pinning: Backed by an anonymous Tools::Memory mapping (huge pages when large), warmed up-front to prevent page faults.
	/// 6. False-Sharing Free: The producer's published cursor and the consumer's published cursor each own a
	///    dedicated cache line, so the two threads never write the same line. Entries are padded to whole cache
	///    lines so a producer write and a concurrent consumer read never share a data line.
	/// </summary>
	class ByteQueue final
	{
	private:
		static constexpr int32_t CacheLine = 64;
		static constexpr int32_t HeaderLength = 4;
		static constexpr uint32_t s_wrapMarker = 0xFFFFFFFF;

		// -------- Backing store (owns the mapping; read-only in steady state) --------
		// Placed first so it stays off the cache-line-isolated cursor lines below.
		Tools::Memory _memory;

		// -------- Configuration (read-only after construction; safe to share a line) --------
		uint8_t* _base;
		size_t _capacity;
		size_t _mask;
		bool _disposed;

		// -------- Producer cache line: published write cursor + producer-private cached cursors --------
		alignas(CacheLine) std::atomic<size_t> _writeSequence;
		size_t _writeSequencePending;
		size_t _readSequenceCached;
		size_t _commitLength;   // aligned size reserved by the last Enqueue, consumed by Commit

		// -------- Consumer cache line: published read cursor, isolated on its own line --------
		alignas(CacheLine) std::atomic<size_t> _readSequence;

	public:
		// Always anonymous (MAP_PRIVATE | MAP_ANONYMOUS); Tools::Memory selects huge pages
		// automatically when the capacity is large enough.
		explicit ByteQueue(int32_t capacityBytes)
			: _memory(Tools::Memory::CreateAnonymous(ValidateCapacity(capacityBytes))),
			  _base(_memory.Ptr),
			  _capacity(static_cast<size_t>(capacityBytes)),
			  _mask(static_cast<size_t>(capacityBytes) - 1),
			  _disposed(false)
		{
			// Lock the cache-line layout so it cannot silently regress. offsetof on a type
			// that contains std::string (via Tools::Memory) is well-defined here but trips
			// -Winvalid-offsetof, so the layout probe is locally silenced.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
			static_assert(offsetof(ByteQueue, _writeSequence) % CacheLine == 0, "write cursor must be cache-line aligned");
			static_assert(offsetof(ByteQueue, _readSequence) % CacheLine == 0, "read cursor must be cache-line aligned");
			static_assert(offsetof(ByteQueue, _readSequence) - offsetof(ByteQueue, _writeSequence) >= CacheLine, "cursors must live on different cache lines");
			static_assert(offsetof(ByteQueue, _readSequenceCached) / CacheLine == offsetof(ByteQueue, _writeSequence) / CacheLine, "producer-private cursors must share the producer line, not the read cursor line");
			static_assert(sizeof(ByteQueue) - offsetof(ByteQueue, _readSequence) <= CacheLine, "read cursor line must not be shared by trailing members");
#pragma GCC diagnostic pop

			_writeSequence.store(0, std::memory_order_relaxed);
			_readSequence.store(0, std::memory_order_relaxed);
			_writeSequencePending = 0;
			_readSequenceCached = 0;
			_commitLength = 0;
		}

		~ByteQueue()
		{
			Dispose();
		}

		void Dispose()
		{
			if (_disposed)
				return;

			_disposed = true;
			_memory.Dispose();
			_base = nullptr;
		}

		ByteQueue(const ByteQueue&) = delete;
		ByteQueue& operator=(const ByteQueue&) = delete;

	private:
		// Validates the ring capacity (power of two, >= 4096) and returns it.
		static int32_t ValidateCapacity(int32_t capacityBytes)
		{
			if (capacityBytes < 4096 || (capacityBytes & (capacityBytes - 1)) != 0)
				throw std::invalid_argument("ByteQueue capacity must be a power of two and >= 4096.");
			return capacityBytes;
		}

		// Total bytes an entry occupies: header + payload, rounded up to a whole cache line.
		// Returned as size_t for the ring cursor arithmetic (internal helper, not public API).
		[[nodiscard]] ALWAYS_INLINE size_t EntryLength(int32_t length) const
		{
			return static_cast<size_t>(Tools::Memory::GetAlignedLength(HeaderLength + length, CacheLine));
		}

		// True if `need` bytes are free for a writer at `writeSequence`. Checks the cached read
		// cursor first and only reloads the published one when that looks short, to avoid
		// hammering the consumer's cache line.
		[[nodiscard]] ALWAYS_INLINE bool HasRoom(size_t need, size_t writeSequence)
		{
			if (_capacity - (writeSequence - _readSequenceCached) >= need)
				return true;
			_readSequenceCached = _readSequence.load(std::memory_order_acquire);
			return _capacity - (writeSequence - _readSequenceCached) >= need;
		}

		[[noreturn]] NEVER_INLINE static void ThrowFull()
		{
			throw std::runtime_error("ByteQueue is full! Consumer thread is lagging.");
		}

		// Writes the length header at writeOffset, caches the aligned size for Commit, and
		// returns the payload span. _writeSequencePending is managed by the caller: the fast
		// path leaves it unchanged; SlowTryEnqueue updates it after a wrap.
		[[nodiscard]] ALWAYS_INLINE std::span<uint8_t> Reserve(int32_t length, size_t need, size_t writeOffset)
		{
			uint8_t* entryPtr = _base + writeOffset;
			*reinterpret_cast<uint32_t*>(entryPtr) = static_cast<uint32_t>(length);
			_commitLength = need;
			return std::span<uint8_t>(entryPtr + HeaderLength, static_cast<size_t>(length));
		}

	public:
		// -------- Producer (Writer) --------

		// Non-throwing reserve in the classic C# Try-pattern: returns true and fills `dst` with
		// the writable region (exactly `length` bytes) on success; returns false and leaves
		// `dst` untouched when the queue is full. A producer that must not lose data (e.g. an
		// exchange callback on a thread we don't own) can spin/back off on false instead of
		// throwing across that boundary. Fill `dst`, then Commit().
		// (C#: bool TryEnqueue(int length, out Span<byte> dst).)
		[[nodiscard]] ALWAYS_INLINE bool TryEnqueue(int32_t length, std::span<uint8_t>& dst)
		{
			if (_disposed)
				throw std::runtime_error("ObjectDisposedException");

			size_t need = EntryLength(length);
			size_t writeSequence = _writeSequencePending;
			size_t free = _capacity - (writeSequence - _readSequenceCached);

			size_t writeOffset = writeSequence & _mask;
			size_t tail = _capacity - writeOffset;

			if (tail >= need && free >= need)
			{
				dst = Reserve(length, need, writeOffset);
				return true;
			}
			return SlowTryEnqueue(length, need, dst);
		}

	private:
		// Cold path of TryEnqueue: the fast path's contiguous-room check failed, so handle the
		// free-space recheck (with a read-cursor reload) and the wrap-to-start. Returns false
		// (leaving `dst` untouched) when there is genuinely no room. `need` is the aligned size.
		NEVER_INLINE bool SlowTryEnqueue(int32_t length, size_t need, std::span<uint8_t>& dst)
		{
			size_t writeSequence = _writeSequencePending;
			if (!HasRoom(need, writeSequence))
				return false;

			size_t writeOffset = writeSequence & _mask;
			size_t tail = _capacity - writeOffset;
			if (tail < need)
			{
				// Not enough contiguous tail: stamp a wrap marker, restart the entry at 0, and
				// recheck free room since the skipped tail counts as consumed space.
				if (tail >= static_cast<size_t>(HeaderLength))
					*reinterpret_cast<uint32_t*>(_base + writeOffset) = s_wrapMarker;

				writeSequence += tail;
				writeOffset = 0;

				if (!HasRoom(need, writeSequence))
					return false;
			}

			_writeSequencePending = writeSequence;
			dst = Reserve(length, need, writeOffset);
			return true;
		}

	public:
		// Publishes the entry reserved by the preceding Enqueue/TryEnqueue. No length argument:
		// the reserved size is cached from the reserve call, so the caller can't desync it (and
		// a stray double-Commit is a no-op, since the cached size is cleared here).
		ALWAYS_INLINE void Commit()
		{
			_writeSequencePending += _commitLength;
			_commitLength = 0;
			_writeSequence.store(_writeSequencePending, std::memory_order_release);
		}

		// -------- Consumer (Reader) --------

		[[nodiscard]] ALWAYS_INLINE bool TryPeek(std::span<const uint8_t>& rsrc)
		{
			if (_disposed)
				return false;

			size_t readSequence = _readSequence.load(std::memory_order_relaxed);
			size_t writeSequence = _writeSequence.load(std::memory_order_acquire);

			if (readSequence == writeSequence)
				return false;

			size_t readOffset = readSequence & _mask;
			size_t tail = _capacity - readOffset;

			if (tail < static_cast<size_t>(HeaderLength))
			{
				readSequence += tail;
				readOffset = 0;
				if (readSequence == writeSequence)
					return false;
				tail = _capacity;
			}

			uint32_t length = *reinterpret_cast<uint32_t*>(_base + readOffset);

			if (length == s_wrapMarker)
			{
				readSequence += tail;
				readOffset = 0;
				if (readSequence == writeSequence)
					return false;
				tail = _capacity;
				length = *reinterpret_cast<uint32_t*>(_base + readOffset);
			}

			uint8_t* src = _base + readOffset + HeaderLength;
			rsrc = std::span<const uint8_t>(src, static_cast<size_t>(length));
			return true;
		}

		ALWAYS_INLINE void Dequeue()
		{
			if (_disposed)
				throw std::runtime_error("ObjectDisposedException");

			size_t readSequence = _readSequence.load(std::memory_order_relaxed);
			size_t writeSequence = _writeSequence.load(std::memory_order_acquire);

			if (readSequence == writeSequence)
				throw std::runtime_error("ByteQueue is empty");

			size_t readOffset = readSequence & _mask;
			size_t tail = _capacity - readOffset;

			if (tail < static_cast<size_t>(HeaderLength))
			{
				readSequence += tail;
				readOffset = 0;
				if (readSequence == writeSequence)
					throw std::runtime_error("ByteQueue is empty");
				tail = _capacity;
			}

			uint32_t length = *reinterpret_cast<uint32_t*>(_base + readOffset);

			if (length == s_wrapMarker)
			{
				readSequence += tail;
				readOffset = 0;
				if (readSequence == writeSequence)
					throw std::runtime_error("ByteQueue is empty");
				tail = _capacity;
				length = *reinterpret_cast<uint32_t*>(_base + readOffset);
			}

			_readSequence.store(readSequence + EntryLength(static_cast<int32_t>(length)), std::memory_order_release);
		}
	};
}
//END_FILE HFT/Tools/ByteQueue.hpp
