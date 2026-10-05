//BEGIN_FILE HFT/Provider/RiskLayer.hpp
#pragma once

#include "Context.hpp"
#include "Order.hpp"
#include "Bitset.hpp"
#include "Tools.hpp"
#include <cmath>
#include <limits>
#include <iostream>

namespace Provider
{

// This class is not thread safe. Only one thread should ever use it.
class RiskLayer
{
private:
    // The server's context on the server, the client's own on a client: OrderRisks and WorkingRisks are per context.
    Provider::Context& _context;
    // CLIENT-side only: the high-water mark of this client's own allocations. Valid there because
    // validation runs at send time on one thread, so validation order IS allocation order. The
    // server must NOT run this check: ids come from one per-client counter but travel on per-core-
    // group rings read by different threads, so two same-instant creates on different instruments
    // can legitimately arrive out of allocation order — the old per-client vector rejected the
    // slower one (ClientOrderIdOutOfOrder) and paused the algo, nondeterministically.
    Execution::OrderId _maxClientOrderId;
    Execution::OrderRejectedSource _orderRejectedSource;

public:
    RiskLayer(Provider::Context& context, Execution::OrderRejectedSource orderRejectedSource)
    : _context(context), _orderRejectedSource(orderRejectedSource)
    {
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateClient(int32_t clientId, int32_t strategyId)
    {
        Tools::Bitset64 orderRejectedReasons;
        const ServerHeader& serverHeader = _context.ServerHeader().GetReadonlyRef();

        bool isClientIdValid = clientId >= 0 && clientId < serverHeader.ClientIds.Length();
        if (!isClientIdValid)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientIdNotValid));
            return orderRejectedReasons;
        }
        if (!serverHeader.ClientIds[clientId])
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientIdNotAllocated));
        }

        bool isStrategyIdValid = strategyId >= 0 && strategyId < serverHeader.ClientIds.Length();
        if (!isStrategyIdValid)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StrategyIdNotValid));
            return orderRejectedReasons;
        }
        if (!serverHeader.ClientIds[strategyId])
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StrategyIdNotAllocated));
        }
        return orderRejectedReasons;
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateInstrument(int32_t strategyId, int32_t instrumentId)
    {
        Tools::Bitset64 orderRejectedReasons;
        const ServerHeader& serverHeader = _context.ServerHeader().GetReadonlyRef();

        bool isValidInstrumentId = instrumentId >= 0 && instrumentId < serverHeader.InstrumentIds.Length();
        if (!isValidInstrumentId)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::InstrumentIdNotValid));
            return orderRejectedReasons;
        }

        if (!_context.GetInstrumentIdsByClientId(strategyId).GetReadonlyRef()[instrumentId])
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::InstrumentNotAllocated));
        }

        Data::Instrument& instrument = _context.GetInstrument(instrumentId);

        // Session state IS the exchange's TradingStatus; Unknown counts as closed. The server must
        // publish each instrument's status at startup or nothing trades (see Spec.md).
        if (instrument.Header().TradingStatus != Data::TradingStatus::Open)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::NotInSession));
        }

        return orderRejectedReasons;
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateCreate(const Execution::OrderTarget& orderTarget, const Execution::OrderState& orderState)
    {
        Tools::Bitset64 orderRejectedReasons;
        if (orderTarget.OrderHeader.Seq != 1)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SeqOutOfOrder));
        }

        // Client side only - see _maxClientOrderId. The server keeps OrderIndexIsBusy and the
        // amend/cancel header checks; a duplicate create cannot reach it anyway (the ring is
        // read-once, Recover() skips the backlog, a restarted client seeds higher generations).
        if (_orderRejectedSource == Execution::OrderRejectedSource::Client)
        {
            if (orderTarget.OrderHeader.OrderId <= _maxClientOrderId)
            {
                orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientOrderIdOutOfOrder));
            }
            else
            {
                _maxClientOrderId = orderTarget.OrderHeader.OrderId;
            }
        }

        if (orderState.OrderStateStatus == Execution::OrderStateStatus::Active)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::OrderIndexIsBusy));
        }
        return orderRejectedReasons;
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateOrderHeader(const Execution::OrderHeader& stateOrderHeader, const Execution::OrderHeader& targetOrderHeader)
    {
        // NOTE: since the ids moved inside ClientOrderId, the per-field checks below are implied by
        // ClientOrderId equality - they can only fire together with ClientOrderIdIsWrong. Kept as
        // harmless belt-and-braces, matching the C# implementation.
        Tools::Bitset64 orderRejectedReasons;
        if (targetOrderHeader.OrderId.IsAlgoOrder() && stateOrderHeader.OrderId.ClientId() != targetOrderHeader.OrderId.ClientId())
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientIdIsWrong));
        }
        if (stateOrderHeader.OrderId.StrategyId() != targetOrderHeader.OrderId.StrategyId())
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StrategyIdIsWrong));
        }
        if (stateOrderHeader.OrderId != targetOrderHeader.OrderId)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientOrderIdIsWrong));
        }
        if (stateOrderHeader.OrderId.InstrumentId() != targetOrderHeader.OrderId.InstrumentId())
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::InstrumentIdIsWrong));
        }
        return orderRejectedReasons;
    }

    // The single home of aggregate arithmetic - every hook and the validator commit go through it.
    // Aggregates are per LEG (an outright is its own single leg, weight +1). Applies an ORDER-unit
    // magnitude delta (negative = release) to each leg's side of exposure. legSide = orderSide *
    // sign(weight) - the sign is applied exactly ONCE, here; signing anywhere else squares it away
    // and drives the short aggregate positive (see Spec.md).
    ALWAYS_INLINE void ApplyWorstWorkingQuantityDelta(Execution::OrderId orderId, int32_t orderSideSign, int32_t magnitudeDelta)
    {
        if (magnitudeDelta == 0)
            return;

        for (const Data::InstrumentLeg& leg : _context.GetInstrument(orderId.InstrumentId()).Legs())
        {
            int32_t legSide = orderSideSign * ((leg.Weight > 0) - (leg.Weight < 0));
            int32_t legMagnitudeDelta = magnitudeDelta * std::abs(leg.Weight);
            // Seq-bumped (single writer: two plain seq stores) so a reader sees an untorn row and the TCP mirror ships the change.
            Socket::SharedArrayEntry<Execution::WorkingRisk>& workingRiskEntry = _context.GetWorkingRisk(leg.InstrumentId);
            Execution::WorkingRisk& workingRisk = workingRiskEntry.GetRef();
            workingRiskEntry.AcquireLock();
            workingRisk.WorstLongWorkingQuantity += legSide > 0 ? legMagnitudeDelta : 0;
            workingRisk.WorstShortWorkingQuantity -= legSide < 0 ? legMagnitudeDelta : 0;
            workingRiskEntry.ReleaseLock();
        }
    }

    ALWAYS_INLINE void OnOrderState(const Execution::OrderState& orderState)
    {
        // Expects the exchange to acknowledge before it trades: a marketable create or amend arrives as Acked,
        // then its fills. The Acked branch releases the old-to-new quantity change, the Done branch releases
        // the rest of what OrderRisk holds. An ack that rides inside a Fill or Done message is not seen here:
        // its quantity stays reserved until Done, which releases exactly what the order holds (see Spec.md
        // "Acceptance before trade").
        if (orderState.OrderStateReason == Execution::OrderStateReason::Acked)
        {
            Execution::OrderRisk& orderRisk = _context.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantityBefore = orderRisk.GetAbsWorstOrderQuantity();
            orderRisk.Ack(orderState.OrderProfile.Quantity);
            int32_t worstOrderQuantityAfter = orderRisk.GetAbsWorstOrderQuantity();
            int32_t worstOrderQuantityDelta = worstOrderQuantityAfter - worstOrderQuantityBefore;

            ApplyWorstWorkingQuantityDelta(orderState.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, worstOrderQuantityDelta);
        }
        else if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
        {
            Execution::OrderRisk& orderRisk = _context.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantity = orderRisk.GetAbsWorstOrderQuantity();
            int32_t released = worstOrderQuantity - std::abs(orderState.QuantityFilled);

            orderRisk = Execution::OrderRisk{};

            ApplyWorstWorkingQuantityDelta(orderState.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, -released);
        }
    }

    // Every fill moves Position; only an outright fill of an order this RiskLayer reserved releases one.
    // Raw fill quantity, NOT a state delta: per-fill releases + the Done remainder telescope to
    // exactly the reserved worst, per leg. A leg fill IS an outright fill - its OrderId carries the
    // leg's InstrumentId, whose single self-leg releases the leg's own reservation directly.
    ALWAYS_INLINE void OnFill(const Execution::Fill& fill, bool isReserved = true)
    {
        int32_t instrumentId = fill.OrderHeader.OrderId.InstrumentId();
        Socket::SharedArrayEntry<Execution::WorkingRisk>& workingRiskEntry = _context.GetWorkingRisk(instrumentId);
        workingRiskEntry.AcquireLock();
        workingRiskEntry.GetRef().Position += fill.Quantity;
        workingRiskEntry.ReleaseLock();

        // A legged instrument's own fill is accounting only (volume/position view on the spread row); risk lives on the
        // legs, so releasing it here would double-release the legs the leg fills already covered. Risk is an outright concept.
        if (!isReserved || _context.GetInstrument(instrumentId).IsLegged())
            return;

        ApplyWorstWorkingQuantityDelta(fill.OrderHeader.OrderId, fill.Sign(), -std::abs(fill.Quantity));
    }

    ALWAYS_INLINE void OnOrderRejected(const Execution::OrderRejected& orderRejected)
    {
        // Nothing to release: a reject from this side never reserved anything here, and a cancel never reserves.
        if (orderRejected.OrderRejectedSource == _orderRejectedSource || orderRejected.OrderTargetAction == Execution::OrderTargetAction::Cancel)
            return;

        Execution::OrderRisk& orderRisk = _context.GetOrderRisk(orderRejected.OrderHeader.OrderId).GetRef();
        Data::Side side = orderRejected.OrderProfile.Side();

        int32_t worstOrderQuantityBefore = orderRisk.GetAbsWorstOrderQuantity();
        orderRisk.Reject(orderRejected.OrderProfile.Quantity);
        int32_t worstOrderQuantityAfter = orderRisk.GetAbsWorstOrderQuantity();
        int32_t worstOrderQuantityDelta = worstOrderQuantityAfter - worstOrderQuantityBefore;

        ApplyWorstWorkingQuantityDelta(orderRejected.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, worstOrderQuantityDelta);
    }

    // The largest |order quantity| (filled included) this order may carry and still pass the position check: its current
    // worst plus the room left on every leg. Past the limit ValidateOrder lets it keep its worst (a cut always passes);
    // isWithinLimit cuts it back to what fits the limit instead, which the server always accepts (see Spec.md).
    ALWAYS_INLINE int32_t GetAbsAllowedOrderQuantity(const Execution::OrderTarget& orderTarget, bool isWithinLimit = false)
    {
        // A Create's row still holds the previous order's values until ValidateOrder resets it.
        int32_t worstOrderQuantity = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create ? 0
            : _context.GetOrderRisk(orderTarget.OrderHeader.OrderId).GetReadonlyRef().GetAbsWorstOrderQuantity();
        int32_t sign = orderTarget.OrderProfile.Sign();
        int64_t absAllowedOrderQuantity = std::numeric_limits<int32_t>::max();

        for (const Data::InstrumentLeg& leg : _context.GetInstrument(orderTarget.OrderHeader.OrderId.InstrumentId()).Legs())
        {
            // legSide = orderSide * sign(weight), as in ApplyWorstWorkingQuantityDelta: a buy calendar reserves the back leg SHORT.
            int32_t legSide = sign * ((leg.Weight > 0) - (leg.Weight < 0));
            const Execution::RiskLimit& riskLimit = _context.GetRiskLimit(leg.InstrumentId).GetReadonlyRef();
            const Execution::WorkingRisk& workingRisk = _context.GetWorkingRisk(leg.InstrumentId).GetReadonlyRef();
            int64_t room = legSide > 0
                ? static_cast<int64_t>(riskLimit.MaxPositionQuantity) - workingRisk.Position - workingRisk.WorstLongWorkingQuantity
                : static_cast<int64_t>(riskLimit.MaxPositionQuantity) + workingRisk.Position + workingRisk.WorstShortWorkingQuantity;
            // Past the limit nothing may grow: keep the worst, or within the limit cut by the overshoot rounded up to whole orders.
            int64_t absWeight = std::abs(leg.Weight);
            int64_t roomOrderQuantity = room >= 0 ? room / absWeight : isWithinLimit ? -((absWeight - 1 - room) / absWeight) : 0;
            absAllowedOrderQuantity = std::min(absAllowedOrderQuantity, worstOrderQuantity + roomOrderQuantity);
        }
        return static_cast<int32_t>(absAllowedOrderQuantity);
    }

    // ValidateOrder's position check: what TryAdd would add must fit the room on every leg.
    ALWAYS_INLINE bool IsWithinRiskLimit(const Execution::OrderTarget& orderTarget)
    {
        return std::abs(orderTarget.OrderProfile.Quantity) <= GetAbsAllowedOrderQuantity(orderTarget);
    }

    ALWAYS_INLINE bool ValidateOrder(const Execution::OrderTarget& orderTarget, Tools::Bitset64& orderRejectedReasons)
    {
        orderRejectedReasons.ClearAll();
        try
        {
            // 1. Basic Bounds Check
            int32_t instrumentId = orderTarget.OrderHeader.OrderId.InstrumentId();
            Data::Instrument& instrument = _context.GetInstrument(instrumentId);
            int32_t strategyId = orderTarget.OrderHeader.OrderId.StrategyId();
            int32_t clientId = orderTarget.OrderHeader.OrderId.ClientId();

            const Execution::OrderTarget& existingTarget = _context.GetOrderTarget(orderTarget.OrderHeader.OrderId).GetReadonlyRef();
            const Execution::OrderState& orderState = _context.GetOrderState(orderTarget.OrderHeader.OrderId).GetReadonlyRef();

            // 3. Validate Creation Logic
            if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create) // check slot is vacant
            {
                orderRejectedReasons = ValidateInstrument(strategyId, instrumentId);
                if (!orderRejectedReasons.IsEmpty())
                {
                    return false;
                }

                orderRejectedReasons = ValidateClient(clientId, strategyId);
                if (!orderRejectedReasons.IsEmpty())
                {
                    return false;
                }

                orderRejectedReasons = ValidateCreate(orderTarget, orderState);
                if (!orderRejectedReasons.IsEmpty())
                {
                    return false;
                }
            }
            else
            {
                bool isReduceOrReplace = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Replace || orderTarget.OrderTargetAction == Execution::OrderTargetAction::Reduce;

                if (_orderRejectedSource == Execution::OrderRejectedSource::Server)
                {
                    orderRejectedReasons = ValidateOrderHeader(orderState.OrderHeader, orderTarget.OrderHeader);
                    if (!orderRejectedReasons.IsEmpty())
                    {
                        return false;
                    }

                    if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StateIsDone));

                    if (isReduceOrReplace && orderState.OrderHeader.Seq + 1 == orderTarget.OrderHeader.Seq && orderState.OrderProfile == orderTarget.OrderProfile)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsActive));

                    if (existingTarget.OrderHeader.Seq > orderTarget.OrderHeader.Seq)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsStale));

                    if (isReduceOrReplace && orderState.OrderProfile.Side() != orderTarget.OrderProfile.Side())
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SideNotValid));
                }

                if (_orderRejectedSource == Execution::OrderRejectedSource::Client)
                {
                    orderRejectedReasons = ValidateOrderHeader(existingTarget.OrderHeader, orderTarget.OrderHeader);
                    if (!orderRejectedReasons.IsEmpty())
                    {
                        return false;
                    }

                    if (orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId) // state == target ??
                    {
                        if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
                            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StateIsDone));
                        if (isReduceOrReplace && existingTarget.OrderTargetStatus == Execution::OrderStateStatus::Done && orderState.OrderProfile == orderTarget.OrderProfile)
                            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsActive));
                    }

                    if (existingTarget.OrderHeader.Seq >= orderTarget.OrderHeader.Seq)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SeqOutOfOrder));

                    if (existingTarget.OrderTargetStatus == Execution::OrderStateStatus::Active) // lastTarget = newTarget ??
                    {
                        if (isReduceOrReplace && existingTarget.OrderProfile == orderTarget.OrderProfile)
                            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsActive));

                        // An in-flight amend whose total is at or below the reported fills leaves the
                        // venue nothing to work (CME leaves-0 semantics): it will be CANCELLED, not
                        // rejected, so follow-ups must bounce here instead of chasing a dead order.
                        // The generation guard is essential - on a recycled slot with a create in
                        // flight, the state row still holds the PREVIOUS order's fills, which must
                        // not condemn the new order.
                        bool existingTargetWillCancel = existingTarget.OrderHeader.OrderId == orderState.OrderHeader.OrderId
                            && existingTarget.OrderProfile.Sign() * (existingTarget.OrderProfile.Quantity - orderState.QuantityFilled) <= 0;
                        if (existingTarget.OrderTargetAction == Execution::OrderTargetAction::Cancel || existingTargetWillCancel)
                            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::CancelIsActive));
                    }

                    if (isReduceOrReplace && existingTarget.OrderProfile.Side() != orderTarget.OrderProfile.Side())
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SideNotValid));
                }
            }

            const Execution::PositionHeader& localPosition = _context.GetPositionHeader(orderTarget.OrderHeader.OrderId.StrategyId(), orderTarget.OrderHeader.OrderId.InstrumentId()).GetReadonlyRef();

            bool isCancel = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Cancel;
            if (!isCancel && orderTarget.OrderHeader.OrderId.IsAlgoOrder() && localPosition.AlgoStatus == Execution::AlgoStatus::Paused)
            {
                orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::AlgoIsPaused));
                return false;
            }

            // The client checks only its own algo orders: a manual order's position sits on another strategy's row.
            if (_orderRejectedSource == Execution::OrderRejectedSource::Client && !orderTarget.OrderHeader.OrderId.IsAlgoOrder())
                return orderRejectedReasons.IsEmpty();

            if (!orderRejectedReasons.IsEmpty())
                return false;

            // Order-entry throttle, one rolling window per CoreGroup (a CoreGroup maps to an iLink
            // session, which is the scope CME throttles). One combined window sized at the tighter
            // line can never breach either exchange line, and what it costs is create and amend
            // throughput while cancels are flying. Plain ref, no seq bump - the CoreGroup thread
            // owns the row.
            if (_orderRejectedSource == Execution::OrderRejectedSource::Server)
            {
                Execution::RollingRateLimit& rollingRateLimit = _context.GetRateLimit(instrument.Header().CoreGroupId).GetRef();

                // A cancel, or a reduce that really is one against the order's current state, is counted but never refused:
                // both only take risk off. A reduce that can't be verified (one behind an unacked replace) is throttled like any amend.
                bool isReduce = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Reduce && orderTarget.OrderProfile.IsReduceOf(orderState.OrderProfile);
                if (isCancel || isReduce)
                {
                    rollingRateLimit.SendOrder(Clock::GetUtcNow());
                }
                else if (!rollingRateLimit.TrySendOrder(Clock::GetUtcNow()))
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TooManyOrdersPerSecond));
                    return false;
                }
            }

            // 10. RISK LIMITS - per LEG (an outright is the 1-leg degenerate case).
            // Only check risk on Create, Replace or Reduce
            if (!isCancel)
            {

                int32_t quantityFilled = orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId ? orderState.QuantityFilled : 0;
                int32_t workingQuantity = orderTarget.OrderProfile.Quantity - quantityFilled;

                // Max order quantity per leg, in LEG units - before TryAdd, so rejects need no back-out.
                for (const Data::InstrumentLeg& leg : instrument.Legs())
                {
                    if (std::abs(workingQuantity * leg.Weight) > _context.GetRiskLimit(leg.InstrumentId).GetReadonlyRef().MaxOrderQuantity)
                    {
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::QuantityExceedsRiskLimit));
                        return false;
                    }
                }

                // Phase 1 - pure: check every leg, write nothing.
                if (!IsWithinRiskLimit(orderTarget))
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::PositionExceedsRiskLimit));
                    return false;
                }

                Execution::OrderRisk& orderRisk = _context.GetOrderRisk(orderTarget.OrderHeader.OrderId).GetRef();

                if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create)
                    orderRisk = Execution::OrderRisk{};

                // Phase 2 - commit through the same arithmetic the release hooks use. Single-writer:
                // nothing can change between the phases, so check-then-apply is atomic by ownership.
                int32_t worstOrderQuantityBefore = orderRisk.GetAbsWorstOrderQuantity();
                Execution::OrderRejectedReason reason = Execution::OrderRejectedReason::Unknown;
                if (!orderRisk.TryAdd(orderTarget.OrderProfile.Quantity, reason))
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(reason));
                    return false;
                }
                int32_t worstMagnitudeDelta = orderRisk.GetAbsWorstOrderQuantity() - worstOrderQuantityBefore;
                ApplyWorstWorkingQuantityDelta(orderTarget.OrderHeader.OrderId, orderTarget.OrderProfile.Sign(), worstMagnitudeDelta);
            }
        }
        catch (const std::exception& ex)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ExceptionThrownByRiskLayer));
            std::cerr << "Exception in RiskLayer: " << ex.what() << std::endl;
        }

        return orderRejectedReasons.IsEmpty();
    }
};

}