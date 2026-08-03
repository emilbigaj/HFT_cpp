//BEGIN_FILE HFT/Tools/ObjectQueue.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include "Tools.hpp"
#include "Memory.hpp"

namespace Tools
{
	/// <summary>
	/// Ultra-fast single-producer single-consumer ring queue of one fixed object type, backed by
	/// unmanaged memory. The typed counterpart to ByteQueue: where that one carries variable-length
	/// byte payloads, this one carries whole objects, so a hand-off between two threads needs no
	/// length header, no copy through a span, and no reinterpret_cast at either end.
	///
	/// DESIGN PHILOSOPHY:
	/// 1. Zero-Allocation: Fixed unmanaged memory allocation upfront to avoid GC/heap pressure.
	/// 2. Fixed Slots: Every entry occupies the same padded slot, so an entry can never straddle the
	///    end of the buffer. That removes the wrap markers and gap-skipping ByteQueue needs for
	///    variable-length payloads — the cursor mask alone does the wrapping.
	/// 3. Power-of-Two: Capacity is 2^N to allow fast bitwise masking instead of slow modulo division.
	/// 4. SPSC Thread-Safe: Uses relaxed and acquire-release atomics for wait-free concurrent operations.
	/// 5. HugePages and NUMA Pinning: Backed by an anonymous Tools::Memory mapping (huge pages when large).
	/// 6. False-Sharing Free: The producer's published cursor and the consumer's published cursor each own a
	///    dedicated cache line, and each thread caches the other's cursor privately on its own line so the
	///    steady state never reads the other thread's line. Slots are padded to whole cache lines so a
	///    producer write and a concurrent consumer read never share a data line either.
	/// 7. Never Overwrites: a full queue fails the enqueue rather than dropping the consumer's oldest entry.
	/// </summary>
	template <typename T> requires Tools::PlainOldData<T>
	class ObjectQueue final
	{
	private:
		static constexpr int32_t CacheLine = 64;

		// One object per slot, padded up to whole cache lines: the producer writing one slot must never
		// dirty a line the consumer is reading from another.
		static constexpr size_t SlotLength = ((sizeof(T) + CacheLine - 1) / CacheLine) * CacheLine;

		// -------- Backing store (owns the mapping; read-only in steady state) --------
		// Placed first so it stays off the cache-line-isolated cursor lines below.
		Tools::Memory _memory;

		// -------- Configuration (read-only after construction; safe to share a line) --------
		uint8_t* _base;
		size_t _capacity;   // in objects, not bytes
		size_t _mask;
		bool _disposed;

		// -------- Producer cache line: published write cursor + the producer's private view of the reader --------
		alignas(CacheLine) std::atomic<size_t> _writeSequence;
		size_t _readSequenceCached;

		// -------- Consumer cache line: published read cursor + the consumer's private view of the writer --------
		alignas(CacheLine) std::atomic<size_t> _readSequence;
		size_t _writeSequenceCached;

	public:
		// `capacity` counts objects, not bytes. Always anonymous (MAP_PRIVATE | MAP_ANONYMOUS);
		// Tools::Memory selects huge pages automatically when the mapping is large enough.
		explicit ObjectQueue(int32_t capacity)
			: _memory(Tools::Memory::CreateAnonymous(ValidateCapacity(capacity))),
			  _base(_memory.Ptr),
			  _capacity(static_cast<size_t>(capacity)),
			  _mask(static_cast<size_t>(capacity) - 1),
			  _disposed(false)
		{
			// Lock the cache-line layout so it cannot silently regress. offsetof on a type
			// that contains std::string (via Tools::Memory) is well-defined here but trips
			// -Winvalid-offsetof, so the layout probe is locally silenced.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
			static_assert(offsetof(ObjectQueue, _writeSequence) % CacheLine == 0, "write cursor must be cache-line aligned");
			static_assert(offsetof(ObjectQueue, _readSequence) % CacheLine == 0, "read cursor must be cache-line aligned");
			static_assert(offsetof(ObjectQueue, _readSequence) - offsetof(ObjectQueue, _writeSequence) >= CacheLine, "cursors must live on different cache lines");
			static_assert(offsetof(ObjectQueue, _readSequenceCached) / CacheLine == offsetof(ObjectQueue, _writeSequence) / CacheLine, "the producer's cached cursor must share the producer line");
			static_assert(offsetof(ObjectQueue, _writeSequenceCached) / CacheLine == offsetof(ObjectQueue, _readSequence) / CacheLine, "the consumer's cached cursor must share the consumer line");
			static_assert(sizeof(ObjectQueue) - offsetof(ObjectQueue, _readSequence) <= CacheLine, "read cursor line must not be shared by trailing members");
#pragma GCC diagnostic pop

			_writeSequence.store(0, std::memory_order_relaxed);
			_readSequence.store(0, std::memory_order_relaxed);
			_readSequenceCached = 0;
			_writeSequenceCached = 0;
		}

		~ObjectQueue()
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

		ObjectQueue(const ObjectQueue&) = delete;
		ObjectQueue& operator=(const ObjectQueue&) = delete;

	private:
		// Validates the ring capacity (a power-of-two count of objects) and returns the bytes its slots need.
		static int32_t ValidateCapacity(int32_t capacity)
		{
			if (capacity < 2 || (capacity & (capacity - 1)) != 0)
				throw std::invalid_argument("ObjectQueue capacity must be a power of two and >= 2.");
			if (static_cast<int64_t>(capacity) * static_cast<int64_t>(SlotLength) > INT32_MAX)
				throw std::invalid_argument("ObjectQueue capacity is too large for the size of its object.");
			return capacity * static_cast<int32_t>(SlotLength);
		}

		// The slot a cursor addresses; the mask wraps it, since every slot is the same size.
		[[nodiscard]] ALWAYS_INLINE uint8_t* Slot(size_t sequence) const
		{
			return _base + (sequence & _mask) * SlotLength;
		}

		// Reload the consumer's published cursor and re-test for room. Only reached when the cached
		// cursor says full, so the consumer's cache line stays cold on the fast path.
		NEVER_INLINE bool ReloadReadSequence(size_t writeSequence)
		{
			_readSequenceCached = _readSequence.load(std::memory_order_acquire);
			return writeSequence - _readSequenceCached < _capacity;
		}

		// Reload the producer's published cursor and re-test for an entry; the mirror of the above.
		NEVER_INLINE bool ReloadWriteSequence(size_t readSequence)
		{
			_writeSequenceCached = _writeSequence.load(std::memory_order_acquire);
			return readSequence != _writeSequenceCached;
		}

	public:
		// -------- Producer (Writer) --------

		// Copies `value` into the ring and publishes it; false when the queue is full, leaving the
		// queue untouched so the caller can spin, back off, or report the loss. A full queue never
		// overwrites an entry the consumer has not taken.
		[[nodiscard]] ALWAYS_INLINE bool TryEnqueue(const T& value)
		{
			if (_disposed)
				throw std::runtime_error("ObjectDisposedException");

			// The producer owns the write cursor, so it reads its own without synchronising.
			const size_t writeSequence = _writeSequence.load(std::memory_order_relaxed);
			if (writeSequence - _readSequenceCached >= _capacity && !ReloadReadSequence(writeSequence))
				return false;

			std::memcpy(Slot(writeSequence), &value, sizeof(T));
			_writeSequence.store(writeSequence + 1, std::memory_order_release);
			return true;
		}

		// -------- Consumer (Reader) --------

		// Copies the oldest entry into `value` and frees its slot; false when the queue is empty,
		// leaving `value` untouched.
		[[nodiscard]] ALWAYS_INLINE bool TryDequeue(T& value)
		{
			if (_disposed)
				return false;

			// The consumer owns the read cursor, so it reads its own without synchronising.
			const size_t readSequence = _readSequence.load(std::memory_order_relaxed);
			if (readSequence == _writeSequenceCached && !ReloadWriteSequence(readSequence))
				return false;

			std::memcpy(&value, Slot(readSequence), sizeof(T));
			_readSequence.store(readSequence + 1, std::memory_order_release);
			return true;
		}

		// The number of objects the ring holds; fixed at construction.
		[[nodiscard]] ALWAYS_INLINE size_t Capacity() const { return _capacity; }
	};
}
//END_FILE HFT/Tools/ObjectQueue.hpp
