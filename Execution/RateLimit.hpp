//BEGIN_FILE HFT/Execution/RateLimit.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Json.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"

namespace Execution
{
#pragma pack(push, 1)
	struct RateLimit
	{
		Tools::Duration Duration = Tools::Duration::FromNanoseconds(0); //  0, 8
		int32_t Limit = 0;                                              //  8, 4
		int32_t RateLimitId = -1;                                       // 12, 4

		// Unlimited in a 1 second window: only the 255-per-bucket burst cap remains (see Spec.md).
		static RateLimit GetMaxLimits(int32_t rateLimitId)
		{
			return RateLimit{ Tools::Duration::FromSeconds(static_cast<int64_t>(1)), std::numeric_limits<int32_t>::max(), rateLimitId };
		}

		static RateLimit GetMinLimits(int32_t rateLimitId)
		{
			return RateLimit{ Tools::Duration::FromSeconds(static_cast<int64_t>(1)), 0, rateLimitId };
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = RateLimit;
			static constexpr auto value = glz::object(
				"Duration", &T::Duration,
				"Limit", &T::Limit,
				"RateLimitId", &T::RateLimitId
			);
		};
	};
	static_assert(sizeof(RateLimit) == 16, "RateLimit must be 16 bytes");

	// A rolling-window throttle in 64 bytes, so it can live in a shared array and the GUI can read
	// it from another process. Exact send timestamps are replaced by BucketCount coarse buckets;
	// they span one bucket MORE than Duration, so the count covers a superset of the window and can
	// only over-state it, never under-state it. A bucket refuses at 255, which is the burst limit.
	// Total is the running sum of the buckets, so a send never loops over them (see Spec.md).
	struct RollingRateLimit
	{
		static constexpr int32_t BucketCount = 32;

		Execution::RateLimit RateLimit;   //  0, 16
		Tools::Timestamp BucketTimestamp; // 16, 8   start of the newest bucket
		int32_t BucketIndex = 0;          // 24, 4   newest bucket
		int32_t Total = 0;                // 28, 4   sum of Counts, kept as they change
		uint8_t Counts[BucketCount] = {}; // 32, 32
		                                  // = 64

		RollingRateLimit() = default;

		explicit RollingRateLimit(Execution::RateLimit rateLimit)
		{
			RateLimit = rateLimit;
			BucketTimestamp = Tools::Timestamp(0); // nanos 0: the first send rolls the whole ring forward
		}

		// BucketCount-1, not BucketCount: the buckets then span Duration + one bucket, which is
		// what keeps the count conservative.
		ALWAYS_INLINE int64_t BucketNanoseconds() const
		{
			return RateLimit.Duration.TotalNanoseconds / (BucketCount - 1);
		}

		// Messages sent in the window ending at timestamp, rounded UP to a bucket boundary. Safe
		// from a reader in another process: it mutates nothing and decays at the reader's own clock.
		[[nodiscard]] int32_t GetCount(Tools::Timestamp timestamp) const
		{
			// Step 1: how many whole buckets have passed since the writer last rolled
			int64_t elapsedNanoseconds = timestamp.NanosSinceEpoch - BucketTimestamp.NanosSinceEpoch;
			int64_t expiredBuckets = elapsedNanoseconds <= 0 ? 0 : elapsedNanoseconds / BucketNanoseconds();

			// Step 2: the whole ring has aged out, nothing is in the window
			if (expiredBuckets >= BucketCount)
				return 0;

			// Step 3: start from the running total, which covers every bucket as of that last roll
			int32_t count = Total;

			// Step 4: take off the oldest buckets that have expired since, usually none
			for (int64_t offset = BucketCount - expiredBuckets; offset < BucketCount; offset++)
			{
				int32_t bucketIndex = BucketIndex - static_cast<int32_t>(offset);
				if (bucketIndex < 0)
					bucketIndex += BucketCount;
				count -= Counts[bucketIndex];
			}
			return count;
		}

		[[nodiscard]] bool CanSendOrder(Tools::Timestamp timestamp) const
		{
			// Step 1: the window is full
			if (GetCount(timestamp) >= RateLimit.Limit)
				return false;

			// Step 2: the send would land in the newest bucket and that bucket is at its burst cap
			int64_t elapsedNanoseconds = timestamp.NanosSinceEpoch - BucketTimestamp.NanosSinceEpoch;
			bool isNewestBucketCurrent = elapsedNanoseconds >= 0 && elapsedNanoseconds < BucketNanoseconds();
			return !isNewestBucketCurrent || Counts[BucketIndex] < UINT8_MAX;
		}

		bool TrySendOrder(Tools::Timestamp timestamp)
		{
			// Step 1: roll the ring up to now, so nothing in it is expired and Total is the count
			Advance(timestamp);

			// Step 2: the window is full
			if (Total >= RateLimit.Limit)
				return false;

			// Step 3: the newest bucket is at its burst cap; refuse rather than wrap
			uint8_t& count = Counts[BucketIndex];
			if (count == UINT8_MAX)
				return false;

			// Step 4: record the send in the newest bucket and in the running total
			count++;
			Total++;
			return true;
		}

		// Counts a send that goes out regardless of the limit, a cancel; the bucket still stops at 255 rather than wrapping.
		void SendOrder(Tools::Timestamp timestamp)
		{
			// Step 1: roll the ring up to now, so nothing in it is expired and Total is the count
			Advance(timestamp);

			// Step 2: the newest bucket is at its burst cap; leave the count rather than wrap
			uint8_t& count = Counts[BucketIndex];
			if (count == UINT8_MAX)
				return;

			// Step 3: record the send in the newest bucket and in the running total
			count++;
			Total++;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = RollingRateLimit;
			static constexpr auto value = glz::object(
				"RateLimit", &T::RateLimit,
				"BucketTimestamp", &T::BucketTimestamp,
				"BucketIndex", &T::BucketIndex,
				"Total", &T::Total,
				"Counts", &T::Counts
			);
		};

	private:
		// Rolls the newest bucket forward to timestamp, zeroing every bucket it passes.
		void Advance(Tools::Timestamp timestamp)
		{
			// Step 1: still inside the newest bucket, nothing to roll; this is the common case
			int64_t bucketNanoseconds = BucketNanoseconds();
			int64_t elapsedNanoseconds = timestamp.NanosSinceEpoch - BucketTimestamp.NanosSinceEpoch;
			if (elapsedNanoseconds < bucketNanoseconds)
				return;

			// Step 2: a whole ring's worth has passed, start again from empty at this timestamp
			int64_t steps = elapsedNanoseconds / bucketNanoseconds;
			if (steps >= BucketCount)
			{
				std::memset(Counts, 0, sizeof(Counts));
				Total = 0;
				BucketIndex = 0;
				BucketTimestamp = timestamp;
				return;
			}

			// Step 3: step the newest bucket forward, zeroing each bucket it lands on and taking it off Total
			for (int64_t step = 0; step < steps; step++)
			{
				BucketIndex = BucketIndex + 1 < BucketCount ? BucketIndex + 1 : 0;
				Total -= Counts[BucketIndex];
				Counts[BucketIndex] = 0;
			}

			// Step 4: the newest bucket starts on the boundary the steps carried it to, not at
			// timestamp, so buckets stay aligned
			BucketTimestamp = BucketTimestamp + Tools::Duration::FromNanoseconds(steps * bucketNanoseconds);
		}
	};
	static_assert(sizeof(RollingRateLimit) == 64, "RollingRateLimit must be 64 bytes");
	static_assert(offsetof(RollingRateLimit, BucketTimestamp) == 16 && offsetof(RollingRateLimit, BucketIndex) == 24
		&& offsetof(RollingRateLimit, Total) == 28 && offsetof(RollingRateLimit, Counts) == 32);
	static_assert(Tools::PlainOldData<RollingRateLimit>, "RollingRateLimit must be unmanaged");
#pragma pack(pop)

	class SessionRateLimit
	{
	private:
		int32_t _limit;
		int32_t _ordersSentToday;

	public:
		explicit SessionRateLimit(int32_t limit) : _limit(limit), _ordersSentToday(0) {}

		bool CanSendOrder(Tools::Timestamp /*timestamp*/) const
		{
			return _ordersSentToday < _limit;
		}

		bool TrySendOrder(Tools::Timestamp timestamp)
		{
			if (!CanSendOrder(timestamp))
				return false;

			_ordersSentToday++;
			return true;
		}

		void Reset()
		{
			_ordersSentToday = 0;
		}
	};
}
//END_FILE HFT/Execution/RateLimit.hpp
