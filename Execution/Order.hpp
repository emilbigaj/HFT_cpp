//BEGIN_FILE HFT/Execution/Order.hpp
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <magic_enum.hpp>

#include "OrderIdAllocator.hpp"
#include "Timestamp.hpp"
#include "Json.hpp"
#include "Tick.hpp"
#include "Bitset.hpp"
#include "Instrument.hpp"
#include "RateLimit.hpp"

namespace Execution
{
	enum class OrderType : uint8_t
	{
		OrderState = 10,
		OrderTarget = 11,
		OrderRejected = 12,
		Fill = 13,
		Position = 14,
		AheadOfOrder = 15,
		RiskLimit = 16,
	};

	enum class TimeInForce : uint8_t
	{
		Day = 0,
		GoodTillCancel = 1,
		ImmediateOrCancel = 2,
		FillOrKill = 3,
		OpeningAuction = 4,
		ClosingAuction = 5,
	};

	enum class OrderStateDoneReason : uint8_t
	{
		None = 0,
		Filled = 1,
		Canceled = 2,
		Rejected = 3,
	};

	enum class OrderStateStatus : uint8_t
	{
		Done = 0,
		Active = 1,
	};

	enum class OrderRejectedReason : uint8_t
	{
		Unknown = 0,

		// ---- 00..09: Bad orderheader (Create-time validity) ----
		ClientIdNotValid       = 1,
		ClientIdNotAllocated   = 2,
		StrategyIdNotValid     = 3,
		StrategyIdNotAllocated = 4,
		InstrumentIdNotValid   = 5,
		InstrumentNotAllocated = 6,

		// ---- 10..19: Wrong Amend/Cancel orderheader (mismatch with existing) ----
		ClientIdIsWrong        = 10,
		StrategyIdIsWrong      = 11,
		InstrumentIdIsWrong    = 12,
		ClientOrderIdIsWrong   = 13,
		SeqIsWrong             = 14,

		// ---- 20..29: Bad orderprofile ----
		QuantityNotValid       = 20,
		PriceNotValid          = 21,
		SideNotValid           = 22,
		OrderTypeNotSupported  = 23, // order type/TIF itself rejected — change the order, don't resend

		// ---- 30..39: Sequencing / lifecycle: client misuse of the order slot ----
		ConnectionBroken          = 30,
		SeqOutOfOrder             = 31,
		ClientOrderIdOutOfOrder   = 32,
		CantAllocateClientOrderId = 33,
		OrderIndexIsBusy          = 34,
		OrderNotFound             = 35,
		DuplicateOrderId            = 36, // ClOrdID reuse / would overwrite a resting order
		// removed, already added above

		// ---- 40..49: Discarded: intentional no-ops; system decided not to act, no alert ----
		StateIsDone            = 40,
		CreateIsActive         = 41,
		CancelIsActive         = 42,
		TargetIsActive         = 43,
		TargetIsStale          = 44,
		AlgoIsPaused           = 45,


		// ---- 50..59: Risk and business limits ----
		NotInSession           = 50,
		PositionIsSuspended    = 51,
		QuantityTooLarge       = 52,
		PositionTooLarge       = 53,
		NotEnoughMargin        = 54,
		TooManyOrdersPerSecond = 55,
		TooManyOrdersPerSession    = 56,
		MessageEfficiencyViolated  = 57,
		TooManyActiveOrders = 58,
		NotAuthorizedToTrade = 59, // entitlement/permission wall — stop retrying

		// ---- 60..69: System ----
		ExceptionThrownByRiskLayer = 60,
	};


	enum class OrderRejectedSource : uint8_t
	{
		Client = 0,
		Server = 1,
		Rival = 2,
		Exchange = 3,
	};

	enum class OrderTargetAction : uint8_t
	{
		Create = 0,
		Amend = 1,
		Cancel = 2,
	};

	enum class OrderFlags : uint8_t
	{
		None = 0,
		PostOnly = 1 << 0,
		ReduceOnly = 1 << 1,
		Hidden = 1 << 2,
	};

	enum class FillType : uint8_t
	{
		Maker = 0,
		Taker = 1,
		Auction = 2,
	};

#pragma pack(push, 1)
	struct RiskLimit
	{
        Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::RiskLimit);
		int32_t InstrumentId = -1;
		Tools::Timestamp Timestamp = Tools::Timestamp::MinValue;
		int32_t StrategyId = -1;
		int32_t MaxOrderQuantity = 0;
		int32_t MaxPositionQuantity = 0;
		Execution::RateLimit MaxOrdersPerSession;
		Execution::RateLimit MaxOrdersPerSecond;

		RiskLimit() = default;
		explicit RiskLimit(int32_t instrumentId) : InstrumentId(instrumentId) {}

		static RiskLimit GetMaxLimits(int32_t instrumentId)	
		{
			RiskLimit maxLimit(instrumentId);
			maxLimit.MaxOrderQuantity = std::numeric_limits<int32_t>::max();
			maxLimit.MaxPositionQuantity = std::numeric_limits<int32_t>::max();
			maxLimit.MaxOrdersPerSession = Execution::RateLimit(Tools::Duration::FromDays(1LL), 1'000'000);
			maxLimit.MaxOrdersPerSecond = Execution::RateLimit(Tools::Duration::FromSeconds(static_cast<int64_t>(1LL)), 300);
			return maxLimit;
		}

		static RiskLimit GetMinLimits(int32_t instrumentId)	
		{
			RiskLimit minLimit(instrumentId);
			minLimit.MaxOrderQuantity = 0;
			minLimit.MaxPositionQuantity = 0;
			minLimit.MaxOrdersPerSession = Execution::RateLimit(Tools::Duration::FromDays(1LL), 0);
			minLimit.MaxOrdersPerSecond = Execution::RateLimit(Tools::Duration::FromSeconds(static_cast<int64_t>(1LL)), 0);	
			return minLimit;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

        struct glaze
		{
			using T = RiskLimit;
			static constexpr auto value = glz::object(
                "Header", &T::Header,
				"InstrumentId", &T::InstrumentId,
				"MaxOrderQuantity", &T::MaxOrderQuantity,
				"MaxPositionQuantity", &T::MaxPositionQuantity,
				"MaxOrdersPerSession", &T::MaxOrdersPerSession,
				"MaxOrdersPerSecond", &T::MaxOrdersPerSecond
			);
		};
	};

	struct OrderProfile
	{
		int32_t Ticks = 0;
		int32_t Quantity = 0;

		Data::Side Side() const
		{
			return static_cast<Data::Side>(Sign());
		}

		int32_t Sign() const
		{
			return (Quantity > 0) - (Quantity < 0);
		}

		bool operator==(const OrderProfile& other) const
		{
			return Ticks == other.Ticks && Quantity == other.Quantity;
		}

		bool operator!=(const OrderProfile& other) const
		{
			return !(*this == other);
		}

		bool IsThisMoreAggressive(int32_t ticks) const
		{
			return (Ticks - ticks) * Sign() > 0;
		}

		bool IsThisCrossing(int32_t ticks) const
		{
			return (Ticks - ticks) * Sign() >= 0;
		}

		static OrderProfile Cancel()
		{
			return OrderProfile(0, 0);
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = OrderProfile;
			static constexpr auto value = glz::object(
				"Ticks", &T::Ticks,
				"Quantity", &T::Quantity
			);
		};
	};

	struct OrderHeader
	{
		int32_t Seq = 0;
		Execution::OrderId OrderId;
		Tools::Timestamp ExchangeTimestamp = Tools::Timestamp(0);
		Tools::Timestamp NicTimestamp = Tools::Timestamp(0);
		struct glaze
		{
			using T = OrderHeader;
			static constexpr auto value = glz::object(
				"Seq", &T::Seq,
				"OrderId", &T::OrderId,
				"ExchangeTimestamp", &T::ExchangeTimestamp,
				"NicTimestamp", &T::NicTimestamp
			);
		};
	};

	static_assert(sizeof(OrderHeader) == 28, "OrderHeader must be 28 bytes");
	static_assert(Tools::PlainOldData<OrderHeader>, "OrderHeader must be unmanaged");

	struct Fill
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::Fill);
		Execution::OrderHeader OrderHeader;
		uint64_t FillId = 0;
		Execution::OrderProfile OrderProfile;
		Execution::FillType FillType = Execution::FillType::Maker;
		uint8_t Reserved[3] = { 0 };

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = Fill;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"OrderHeader", &T::OrderHeader,
				"FillType", &T::FillType,
				"FillId", &T::FillId,
				"OrderProfile", &T::OrderProfile
			);
		};
	};

	static_assert(sizeof(Fill) == 52, "Fill must be 52 bytes");

	struct OrderRejected
    {
        Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::OrderRejected);
        Execution::OrderHeader OrderHeader;
        Execution::OrderTargetAction OrderTargetAction = Execution::OrderTargetAction::Create;
        Execution::OrderRejectedSource OrderRejectedSource = Execution::OrderRejectedSource::Server;
        uint8_t Reserved[2] = {0};
        Execution::OrderProfile OrderProfile;
        Tools::Bitset64 OrderRejectedReasons;

        std::string ToString() const
        {
            return Tools::Json::Serialize(*this);
        }

        std::string OrderRejectedReasonsString() const
        {
            std::stringstream stringBuilder;
            bool isFirst = true;

            for (int32_t reasonIndex : OrderRejectedReasons)
            {
                if (!isFirst)
                    stringBuilder << "|";
                Execution::OrderRejectedReason reason = static_cast<Execution::OrderRejectedReason>(reasonIndex);
                stringBuilder << magic_enum::enum_name(reason);
                isFirst = false;
            }

            return stringBuilder.str();
        }

		inline static const Tools::Bitset64 OrderDiscarded = []
		{
			Tools::Bitset64 b;
			b.Set(static_cast<int32_t>(OrderRejectedReason::CreateIsActive));
			b.Set(static_cast<int32_t>(OrderRejectedReason::StateIsDone));
			b.Set(static_cast<int32_t>(OrderRejectedReason::TargetIsStale));
			b.Set(static_cast<int32_t>(OrderRejectedReason::CancelIsActive));
			b.Set(static_cast<int32_t>(OrderRejectedReason::TargetIsActive));
			b.Set(static_cast<int32_t>(OrderRejectedReason::AlgoIsPaused));
			b.Set(static_cast<int32_t>(OrderRejectedReason::TooManyOrdersPerSecond));
			return b;
		}();

        // Moving this to the bottom allows the lambda to see OrderRejectedReasonsString
        struct glaze
        {
            using T = OrderRejected;
            static constexpr auto value = glz::object(
                "Header", &T::Header,
                "OrderHeader", &T::OrderHeader,
                "OrderTargetAction", &T::OrderTargetAction,
                "OrderRejectedSource", &T::OrderRejectedSource,
                "OrderProfile", &T::OrderProfile,
                "OrderRejectedReasons", &T::OrderRejectedReasons,
                "OrderRejectedReasonsString", glz::custom<nullptr, &T::OrderRejectedReasonsString>
            );
        };
    };

	static_assert(sizeof(OrderRejected) == 52, "OrderRejected must be 52 bytes");

	struct OrderState
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::OrderState);
		Execution::OrderHeader OrderHeader;
		// The exchange's id for this order, assigned on ack; lets us match against the MarketByOrder
		// feed (queue position / own-order recognition). Lives on the state, not the header.
		uint64_t ExchangeOrderId = 0;
		Execution::OrderProfile OrderProfile;
		Execution::TimeInForce TimeInForce = Execution::TimeInForce::Day;
		Execution::OrderStateStatus OrderStateStatus = Execution::OrderStateStatus::Active;
		uint8_t Reserved[2] = { 0, 0 };
		int32_t QuantityFilled = 0;
		int32_t QuantityAhead = 0;

		int32_t WorkingQuantity() const
		{
			return OrderProfile.Quantity - QuantityFilled;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = OrderState;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"OrderHeader", &T::OrderHeader,
				"ExchangeOrderId", &T::ExchangeOrderId,
				"OrderProfile", &T::OrderProfile,
				"TimeInForce", &T::TimeInForce,
				"OrderStateStatus", &T::OrderStateStatus,
				"QuantityFilled", &T::QuantityFilled,
				"QuantityAhead", &T::QuantityAhead
			);
		};
	};

	static_assert(sizeof(OrderState) == 60, "OrderState must be 60 bytes");

	struct AheadOfOrder
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::AheadOfOrder);
        int32_t Quantity = 0;
		uint64_t ClientOrderId = 0;

		AheadOfOrder() = default;
		AheadOfOrder(uint64_t clientOrderId, int32_t quantity) : Quantity(quantity), ClientOrderId(clientOrderId) {}

		struct glaze
		{
			using T = AheadOfOrder;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"ClientOrderId", &T::ClientOrderId,
				"Quantity", &T::Quantity
			);
		};
	};

	struct OrderTarget
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::OrderTarget);
		Execution::OrderHeader OrderHeader;
		Execution::OrderProfile OrderProfile;
		Execution::TimeInForce TimeInForce = Execution::TimeInForce::Day;
		Execution::OrderTargetAction OrderTargetAction = Execution::OrderTargetAction::Create;
		Execution::OrderStateStatus OrderTargetStatus = Execution::OrderStateStatus::Active;
		uint8_t Reserved[1] = { 0 };

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = OrderTarget;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"OrderHeader", &T::OrderHeader,
				"OrderProfile", &T::OrderProfile,
				"TimeInForce", &T::TimeInForce,
				"OrderTargetAction", &T::OrderTargetAction,
				"OrderTargetStatus", &T::OrderTargetStatus
			);
		};
	};

	static_assert(sizeof(OrderTarget) == 44, "OrderTarget must be 44 bytes");

    enum AlgoStatus : uint8_t
    {
        Paused = 0,
        Live = 1
    };

	struct PositionHeader
	{
		Data::Header<OrderType> Header = Data::Header<OrderType>(OrderType::Position);
		Execution::OrderHeader OrderHeader;
		int32_t Quantity = 0;
		Tools::Nanouble AvgPrice = std::numeric_limits<double>::quiet_NaN();
		double RealizedProfit = 0.0;
        int32_t QuantityTraded = 0;
        Execution::AlgoStatus AlgoStatus = Execution::AlgoStatus::Paused;

		void OnFill(const Fill& fill, double tickSize, double multiplier)
		{
			int32_t quantity = fill.OrderProfile.Quantity;
			double price = fill.OrderProfile.Ticks * tickSize;
			OrderHeader = fill.OrderHeader;

			int32_t oldQty = Quantity;
			int32_t newQty = oldQty + quantity;
			int32_t oldSide = (oldQty > 0) - (oldQty < 0);
			int32_t fillSide = (quantity > 0) - (quantity < 0);

			QuantityTraded += std::abs(quantity);

			if (oldQty == 0 || oldSide == fillSide)
			{
				AvgPrice = (oldQty == 0) ?
					price : (AvgPrice * oldQty + price * quantity) / newQty;
			}
			else
			{
				int32_t closed = std::min(std::abs(oldQty), std::abs(quantity));
				double pnl = (price - AvgPrice) * closed * oldSide * multiplier;
				RealizedProfit += pnl;

				if (newQty == 0)
				{
					AvgPrice = std::numeric_limits<double>::quiet_NaN();
				}
				else if (((newQty > 0) - (newQty < 0)) == oldSide)
				{
					// Partial close, side unchanged
				}
				else
				{
					AvgPrice = price;
				}
			}

			Quantity = newQty;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = PositionHeader;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"OrderHeader", &T::OrderHeader,
				"Quantity", &T::Quantity,
				"AvgPrice", &T::AvgPrice,
				"RealizedProfit", &T::RealizedProfit,
				"QuantityTraded", &T::QuantityTraded
			);
		};
	};

	static_assert(sizeof(PositionHeader) == 57, "PositionHeader must be 57 bytes");

#pragma pack(pop)
}
//END_FILE HFT/Execution/Order.hpp