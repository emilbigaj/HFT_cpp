//BEGIN_FILE HFT/Execution/RateLimit.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <cmath>

#include "DateTime.hpp"
#include "Json.hpp"
#include "Order.hpp"
#include "String.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"

namespace Execution
{
#pragma pack(push, 1)
	struct MessageEfficiencyTier
	{
		int32_t DailyRawMessages = 0;
		int32_t Benchmark = 0;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = MessageEfficiencyTier;
			static constexpr auto value = glz::object(
				"DailyRawMessages", &T::DailyRawMessages,
				"Benchmark", &T::Benchmark
			);
		};
	};
	static_assert(sizeof(MessageEfficiencyTier) == 8);

	// One product group's CME messaging-efficiency day: a shared row in <server>/MessageEfficiency, indexed by
	// InstrumentHeader::ProductGroupId. The server claims and loads it at allocation, ends the day on Closed and posts it on
	// its audit channel; C# strategies count into it. C++ only reads and writes the row: it never counts.
	struct MessageEfficiency
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::MessageEfficiency);   //  0, 4   the server posts the row itself on its audit channel
		Tools::DateTime TradeDate;                                                                 //  4, 8
		Tools::TimeSpan TradeTime;                                                                 // 12, 8
		MessageEfficiencyTier Tier0;                                                               // 20, 8
		MessageEfficiencyTier Tier1;                                                               // 28, 8
		MessageEfficiencyTier Tier2;                                                               // 36, 8
		MessageEfficiencyTier Tier3;                                                               // 44, 8
		Tools::String4 ProductGroupRoot;                                                           // 52, 4
		int32_t ProductGroupId = -1;                                                               // 56, 4
		int32_t RawMessages = 0;                                                                   // 60, 4
		double WeightedMessages = 0;                                                               // 64, 8
		double InverseQuantityTraded = 1;                                                          // 72, 8
		int32_t QuantityTraded = 0;                                                                // 80, 4

		// Starts the next day once dateTime reaches the day's end; true when it did.
		bool Reset(Tools::DateTime dateTime)
		{
			Tools::DateTime tradeDateTime = TradeDate.Add(TradeTime);
			if (dateTime >= tradeDateTime)
			{
				TradeDate = dateTime.Date().AddDays(1);
				RawMessages = 0;
				WeightedMessages = 0;
				QuantityTraded = 0;
				InverseQuantityTraded = 1;
				return true;
			}
			return false;
		}

		double Efficiency() const
		{
			return WeightedMessages * InverseQuantityTraded;
		}

		void OnFill(int32_t quantity)
		{
			QuantityTraded += std::abs(quantity);
			InverseQuantityTraded = 1.0 / QuantityTraded;
		}

		// Permissive default (simulation): tiers so high every message is allowed.
		static MessageEfficiency GetMaxLimits(Tools::String4 productGroupRoot)
		{
			MessageEfficiency messageEfficiency;
			messageEfficiency.ProductGroupRoot = productGroupRoot;
			messageEfficiency.Tier0 = messageEfficiency.Tier1 = messageEfficiency.Tier2 = messageEfficiency.Tier3 = MessageEfficiencyTier{ .DailyRawMessages = INT32_MAX, .Benchmark = INT32_MAX };
			return messageEfficiency;
		}

		// Restrictive default (live until configured): zero tiers block every message.
		static MessageEfficiency GetMinLimits(Tools::String4 productGroupRoot)
		{
			MessageEfficiency messageEfficiency;
			messageEfficiency.ProductGroupRoot = productGroupRoot;
			messageEfficiency.Tier0 = messageEfficiency.Tier1 = messageEfficiency.Tier2 = messageEfficiency.Tier3 = MessageEfficiencyTier{ .DailyRawMessages = 0, .Benchmark = 0 };
			return messageEfficiency;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = MessageEfficiency;
			static constexpr auto value = glz::object(
				"Efficiency", glz::custom<glz::skip{}, &T::Efficiency>,
				"Header", &T::Header,
				"TradeDate", &T::TradeDate,
				"TradeTime", &T::TradeTime,
				"Tier0", &T::Tier0,
				"Tier1", &T::Tier1,
				"Tier2", &T::Tier2,
				"Tier3", &T::Tier3,
				"ProductGroupRoot", &T::ProductGroupRoot,
				"ProductGroupId", &T::ProductGroupId,
				"RawMessages", &T::RawMessages,
				"WeightedMessages", &T::WeightedMessages,
				"InverseQuantityTraded", &T::InverseQuantityTraded,
				"QuantityTraded", &T::QuantityTraded
			);
		};
	};
	static_assert(Tools::PlainOldData<MessageEfficiency>);
	static_assert(sizeof(MessageEfficiency) == 84 && offsetof(MessageEfficiency, TradeDate) == 4 && offsetof(MessageEfficiency, ProductGroupRoot) == 52
		&& offsetof(MessageEfficiency, QuantityTraded) == 80, "MessageEfficiency must match the C# row (84 bytes)");

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
