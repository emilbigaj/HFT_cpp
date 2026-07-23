#pragma once

#include <atomic>
#include <cstdint>
#include <cassert>
#include <immintrin.h> // _mm_pause
#include <concepts>

// CRITICAL: This optimized implementation relies on the x86-64 TSO memory model.
#if !defined(__x86_64__) && !defined(_M_X64)
#error "CRITICAL: This code relies on x86-64 TSO memory model. It is unsafe on ARM64."
#endif

// Define macro if not available globally
#ifndef ALWAYS_INLINE
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#endif
#endif

namespace Tools
{
	// ===============================================================
	// Reader: Stateless, static, zero-allocation
	// ===============================================================
	struct SeqLockReader final
	{
		ALWAYS_INLINE static uint64_t Read(const std::atomic<uint64_t>& sequence)
		{
			return sequence.load(std::memory_order_acquire);
		}

		ALWAYS_INLINE static bool IsWriteInProgress(uint64_t sequence)
		{
			return (sequence & 1UL) != 0UL;
		}

		ALWAYS_INLINE static bool IsStable(uint64_t sequence)
		{
			return (sequence & 1UL) == 0UL;
		}

		/// <summary>Spin until an even (stable) epoch is observed.</summary>
		ALWAYS_INLINE static uint64_t BeginRead(const std::atomic<uint64_t>& sequence)
		{
			while (true)
			{
				uint64_t snapshot = Read(sequence);
				if (IsStable(snapshot))
					return snapshot;

				_mm_pause();
			}
		}

		/// <summary>Validate that no writer intervened.</summary>
		ALWAYS_INLINE static bool Validate(uint64_t beforeSequence, const std::atomic<uint64_t>& sequence)
		{
			// COMPILER BARRIER (Load-Load Fence):
			// Ensures Payload Reads (happening before this call) do not sink 
			// below the validation check.
			std::atomic_thread_fence(std::memory_order_acquire);

			uint64_t afterSequence = sequence.load(std::memory_order_relaxed);
			return beforeSequence == afterSequence && IsStable(afterSequence);
		}

		// ---------- Pointer helpers ----------

		ALWAYS_INLINE static uint64_t Read(const std::atomic<uint64_t>* sequencePtr)
		{
			return Read(*sequencePtr);
		}

		ALWAYS_INLINE static uint64_t BeginRead(const std::atomic<uint64_t>* sequencePtr)
		{
			return BeginRead(*sequencePtr);
		}
	};

	// ===============================================================
	// Concepts & RAII
	// ===============================================================

	template<typename T>
	concept SeqLockStrategy = requires(T t, std::atomic<uint64_t>* ptr)
	{
		// Enforce static methods matching the signature
		{ T::AcquireLock(ptr) } -> std::same_as<void>;
		{ T::ReleaseLock(ptr) } -> std::same_as<void>;
	};

	template <SeqLockStrategy LockType>
	class RAIILock final
	{
		std::atomic<uint64_t>* _seqPtr;

	public:
		explicit RAIILock(std::atomic<uint64_t>* seqPtr) : _seqPtr(seqPtr)
		{
			assert(_seqPtr != nullptr);
			LockType::AcquireLock(_seqPtr);
		}

		~RAIILock()
		{
			LockType::ReleaseLock(_seqPtr);
		}

		RAIILock(const RAIILock&) = delete;
		RAIILock& operator=(const RAIILock&) = delete;
		RAIILock(RAIILock&&) = delete;
		RAIILock& operator=(RAIILock&&) = delete;
	};

	// ===============================================================
	// Single Writer (Fastest, No CAS)
	// Removed inheritance to eliminate vtable overhead.
	// ===============================================================
	class SingleSeqLockWriter final
	{
		// Padded to prevent False Sharing. 
		alignas(64) std::atomic<uint64_t> _sequence{ 0 };

	public:
		[[nodiscard]] const std::atomic<uint64_t>& SeqRef() const
		{
			return _sequence;
		}
		
		[[nodiscard]] std::atomic<uint64_t>* SeqPtr()
		{
			return &_sequence;
		}

		// Instance wrappers for ease of use
		void AcquireLock() { AcquireLock(&_sequence); }
		void ReleaseLock() { ReleaseLock(&_sequence); }
		void Reset() { Reset(&_sequence); }

		// ---------- Static Implementation (Used by RAII) ----------

        ALWAYS_INLINE static void AcquireLock(std::atomic<uint64_t>* seqPtr)
        {
            // 1. Read relaxed (we own the lock, no contention expected).
            uint64_t currentSeq = seqPtr->load(std::memory_order_relaxed);

            // 2. Write Odd (Relaxed).
            seqPtr->store(currentSeq + 1UL, std::memory_order_relaxed);

            // 3. Acquire-Release Fence.
            // Enforces a strict bidirectional Store-Store barrier for the compiler.
            // On x86-64, this compiles to nothing but prevents compiler reordering.
            std::atomic_thread_fence(std::memory_order_acq_rel);
        }

		ALWAYS_INLINE static void ReleaseLock(std::atomic<uint64_t>* seqPtr)
		{
			uint64_t currentSeq = seqPtr->load(std::memory_order_relaxed);
			
			// Store Even (Release).
			// Ensures all payload writes are committed before we mark as valid.
			seqPtr->store(currentSeq + 1UL, std::memory_order_release);
		}

		ALWAYS_INLINE static void Reset(std::atomic<uint64_t>* sequencePtr)
		{
			sequencePtr->store(0UL, std::memory_order_release);
		}
	};

	// ===============================================================
	// Multi Writer (CAS-protected)
	// ===============================================================
	class MultiSeqLockWriter final
	{
		alignas(64) std::atomic<uint64_t> _sequence{ 0 };

	public:
		[[nodiscard]] const std::atomic<uint64_t>& SeqRef() const { return _sequence; }
		[[nodiscard]] std::atomic<uint64_t>* SeqPtr() { return &_sequence; }

		void AcquireLock() { AcquireLock(&_sequence); }
		void ReleaseLock() { ReleaseLock(&_sequence); }
		void Reset() { Reset(&_sequence); }

		// ---------- Static Implementation ----------

        ALWAYS_INLINE static void AcquireLock(std::atomic<uint64_t>* sequencePtr)
		{
			// 1. Initial Load
			uint64_t currentSeq = sequencePtr->load(std::memory_order_acquire);

			while (true)
			{
				// 2. Check if Even (Unlocked)
				if ((currentSeq & 1UL) == 0UL)
				{
					// 3. Try CAS
					// If successful: returns true, we own the lock.
					// If failed: returns false, AND updates 'currentSeq' with the actual value seen.
					// We use 'acquire' to prevent payload hoisting on success.
					// We use 'relaxed' on failure because we just want to loop again.
					if (sequencePtr->compare_exchange_weak(currentSeq, currentSeq + 1UL, 
						std::memory_order_acquire, 
						std::memory_order_relaxed))
					{
						return;
					}
				}
				else
				{
					// Lock is held by someone else (Odd).
					// Reload to check for updates, using Relaxed to minimize coherence cost while spinning.
					_mm_pause();
					currentSeq = sequencePtr->load(std::memory_order_relaxed);
				}
			}
		}

		ALWAYS_INLINE static void ReleaseLock(std::atomic<uint64_t>* sequencePtr)
		{
			sequencePtr->fetch_add(1, std::memory_order_release);
		}

		ALWAYS_INLINE static void Reset(std::atomic<uint64_t>* sequencePtr)
		{
			sequencePtr->store(0UL, std::memory_order_release);
		}
	};

    class RAIISpinLock final
	{
	private:
		std::atomic<bool>& _lock;

	public:
		explicit RAIISpinLock(std::atomic<bool>& lock) : _lock(lock)
		{
			while (true)
			{
				if (!_lock.exchange(true, std::memory_order_acquire))
				{
					return;
				}

				while (_lock.load(std::memory_order_relaxed))
				{
					_mm_pause();
				}
			}
		}

		~RAIISpinLock()
		{
			_lock.store(false, std::memory_order_release);
		}

		// CRITICAL: Prevent copying and moving to avoid premature lock release
		RAIISpinLock(const RAIISpinLock&) = delete;
		RAIISpinLock& operator=(const RAIISpinLock&) = delete;
		RAIISpinLock(RAIISpinLock&&) = delete;
		RAIISpinLock& operator=(RAIISpinLock&&) = delete;
	};

} // namespace Tools