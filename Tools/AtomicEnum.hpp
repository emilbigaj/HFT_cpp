#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace Tools
{
	// Atomic {state, epoch} in one word, for any byte-backed enum. The state lives in the low
	// byte; the epoch fills the high 56 bits and advances on EVERY transition, so no value in
	// the word's history ever repeats and a compare-exchange against a stale snapshot always
	// fails - a thread that slept through state changes cannot act on the world it remembers,
	// even if the state byte has cycled back to what it saw (ABA).
	//
	// Contract: one owner thread calls Store() with plain authority - it may overwrite a
	// concurrently successful TryTransition(), which is the intent (the owner's word is final).
	// Any thread may Load() and TryTransition() from a Snapshot it holds. The CAS must use the
	// snapshot taken when the evidence for the transition was observed - never a fresh Load(),
	// which would adopt the new epoch and defeat the check.
	template <typename TState> requires std::is_enum_v<TState> && (sizeof(TState) == 1)
	class AtomicEnum
	{
	private:
		static constexpr int32_t s_stateBits = 8;
		static constexpr uint64_t s_stateMask = (1ULL << s_stateBits) - 1ULL;

		std::atomic<uint64_t> _word;

		static constexpr uint64_t Pack(uint64_t epoch, TState state)
		{
			return (epoch << s_stateBits) | static_cast<uint64_t>(state);
		}

	public:
		struct Snapshot
		{
			uint64_t Word = 0;

			TState State() const
			{
				return static_cast<TState>(Word & s_stateMask);
			}

			uint64_t Epoch() const
			{
				return Word >> s_stateBits;
			}
		};

		explicit AtomicEnum(TState state) : _word(Pack(0, state)) {}

		inline Snapshot Load() const
		{
			return Snapshot{_word.load(std::memory_order_acquire)};
		}

		inline TState State() const
		{
			return Load().State();
		}

		inline uint64_t Epoch() const
		{
			return Load().Epoch();
		}

		// Owner thread only: transition unconditionally, advancing the epoch.
		inline void Store(TState state)
		{
			uint64_t word = _word.load(std::memory_order_relaxed);
			_word.store(Pack((word >> s_stateBits) + 1, state), std::memory_order_release);
		}

		// Any thread: transition only if the word still IS the snapshot - same state, same
		// epoch. Returns false (and does nothing) if the world moved on since the snapshot.
		inline bool TryTransition(Snapshot snapshot, TState desired)
		{
			uint64_t expected = snapshot.Word;
			return _word.compare_exchange_strong(expected, Pack(snapshot.Epoch() + 1, desired),
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
	};
}
