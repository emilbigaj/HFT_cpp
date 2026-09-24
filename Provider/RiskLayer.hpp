//BEGIN_FILE HFT/Provider/RiskLayer.hpp
#pragma once

#include "Context.hpp"
#include "Order.hpp"
#include "Bitset.hpp"
#include "Tools.hpp"
#include <cmath>
#include <iostream>

namespace Provider
{

// This class is not thread safe. Only one thread should ever use it.
class RiskLayer
{
private:
    // Borrowed, not owned. The server hands in the context it opened with Access::Write, because the
    // ledger writes RiskLimit and OrderRisk rows — a context of its own would be Access::Read and every
    // GetRef() below would throw "Readonly" straight into the ExceptionThrownByRiskLayer catch,
    // rejecting every order. A client passes a read-only one; its reservation block is gated off.
    Provider::ServerContext& _serverContext;
    // CLIENT-side only: the high-water mark of this client's own allocations. Valid there because
    // validation runs at send time on one thread, so validation order IS allocation order. The
    // server must NOT run this check: ids come from one per-client counter but travel on per-core-
    // group rings read by different threads, so two same-instant creates on different instruments
    // can legitimately arrive out of allocation order — the old per-client vector rejected the
    // slower one (ClientOrderIdOutOfOrder) and paused the algo, nondeterministically.
    Execution::OrderId _maxClientOrderId;
    Execution::OrderRejectedSource _orderRejectedSource;

public:
    RiskLayer(Provider::ServerContext& serverContext, Execution::OrderRejectedSource orderRejectedSource)
    : _serverContext(serverContext), _orderRejectedSource(orderRejectedSource)
    {
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateClient(int32_t clientId, int32_t strategyId)
    {
        Tools::Bitset64 orderRejectedReasons;
        const ServerHeader& serverHeader = _serverContext.ServerHeader().GetReadonlyRef();

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
        const ServerHeader& serverHeader = _serverContext.ServerHeader().GetReadonlyRef();

        bool isValidInstrumentId = instrumentId >= 0 && instrumentId < serverHeader.InstrumentIds.Length();
        if (!isValidInstrumentId)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::InstrumentIdNotValid));
            return orderRejectedReasons;
        }

        if (!_serverContext.GetInstrumentIdsByClientId(strategyId).GetReadonlyRef()[instrumentId])
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::InstrumentNotAllocated));
        }

        Data::Instrument& instrument = _serverContext.GetInstrument(instrumentId);

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

    // ---- retire paths. Server-side only: the client has no authority over the ledger and its
    // ---- RiskLayer maps the arrays read-only, so touching them there would throw.

    // The single home of aggregate arithmetic - every hook and the validator commit go through it.
    // Aggregates are per LEG (an outright is its own single leg, weight +1). Applies an ORDER-unit
    // magnitude delta (negative = release) to each leg's side of exposure. legSide = orderSide *
    // sign(weight) - the sign is applied exactly ONCE, here; signing anywhere else squares it away
    // and drives the short aggregate positive (see Spec.md).
    ALWAYS_INLINE void ApplyWorstWorkingQuantityDelta(Execution::OrderId orderId, int32_t orderSideSign, int32_t magnitudeDelta)
    {
        if (magnitudeDelta == 0)
            return;

        for (const Data::InstrumentLeg& leg : _serverContext.GetInstrument(orderId.InstrumentId()).Legs())
        {
            int32_t legSide = orderSideSign * ((leg.Weight > 0) - (leg.Weight < 0));
            int32_t legMagnitudeDelta = magnitudeDelta * std::abs(leg.Weight);
            Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(leg.InstrumentId).GetRef();
            riskLimit.WorstLongWorkingQuantity += legSide > 0 ? legMagnitudeDelta : 0;
            riskLimit.WorstShortWorkingQuantity -= legSide < 0 ? legMagnitudeDelta : 0;
        }
    }

    ALWAYS_INLINE void OnOrderState(const Execution::OrderState& orderState, int32_t beforeAckedOrderQuantity)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        // Expects the exchange to acknowledge before it trades: a marketable create or amend arrives as Acked,
        // then its fills. The Acked branch releases the old-to-new quantity change, the Done branch releases
        // the rest measured from the acked quantity; a fill that carried an unacked quantity would leak the
        // difference for good. The simulator and CME both honour this (see Spec.md "Acceptance before trade").
        if (orderState.OrderStateReason == Execution::OrderStateReason::Acked)
        {
            Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantityBefore = orderRisk.GetAbsWorstOrderQuantity(beforeAckedOrderQuantity);
            orderRisk.Ack(orderState.OrderProfile.Quantity);
            int32_t worstOrderQuantityAfter = orderRisk.GetAbsWorstOrderQuantity(orderState.OrderProfile.Quantity);
            int32_t worstOrderQuantityDelta = worstOrderQuantityAfter - worstOrderQuantityBefore;

            ApplyWorstWorkingQuantityDelta(orderState.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, worstOrderQuantityDelta);
        }
        else if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
        {
            // Release on Done rather than on a reason match: a cancel, reject or expiry that carries a
            // label this switch does not know would otherwise leak its whole reservation, permanently.
            Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantity = orderRisk.GetAbsWorstOrderQuantity(orderState.OrderProfile.Quantity);
            int32_t released = worstOrderQuantity - std::abs(orderState.QuantityFilled);

            orderRisk = Execution::OrderRisk{};

            ApplyWorstWorkingQuantityDelta(orderState.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, -released);
        }
    }

    // A fill converts reservation into position, so the reservation shrinks by exactly the fill.
    // Raw fill quantity, NOT a state delta: per-fill releases + the Done remainder telescope to
    // exactly the reserved worst, per leg. A leg fill IS an outright fill - its OrderId carries the
    // leg's InstrumentId, whose single self-leg releases the leg's own reservation directly.
    ALWAYS_INLINE void OnFill(const Execution::Fill& fill)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        ApplyWorstWorkingQuantityDelta(fill.OrderHeader.OrderId, fill.Sign(), -std::abs(fill.Quantity));
    }

    // An exchange reject retires exactly the target it names; a server reject never reserved anything.
    ALWAYS_INLINE void OnOrderRejected(const Execution::OrderRejected& orderRejected)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        if (orderRejected.OrderRejectedSource == Execution::OrderRejectedSource::Server)
            return;

        const Execution::OrderState& orderState = _serverContext.GetOrderState(orderRejected.OrderHeader.OrderId).GetReadonlyRef();
        Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderRejected.OrderHeader.OrderId).GetRef();
        Data::Side side = orderRejected.OrderProfile.Side();

        int32_t worstOrderQuantityBefore = orderRisk.GetAbsWorstOrderQuantity(orderState.OrderProfile.Quantity);
        orderRisk.Reject(orderRejected.OrderProfile.Quantity);
        int32_t worstOrderQuantityAfter = orderRisk.GetAbsWorstOrderQuantity(orderState.OrderProfile.Quantity);
        int32_t worstOrderQuantityDelta = worstOrderQuantityAfter - worstOrderQuantityBefore;

        ApplyWorstWorkingQuantityDelta(orderRejected.OrderHeader.OrderId, side == Data::Side::Buy ? 1 : -1, worstOrderQuantityDelta);
    }

    ALWAYS_INLINE bool ValidateOrder(const Execution::OrderTarget& orderTarget, Tools::Bitset64& orderRejectedReasons)
    {
        orderRejectedReasons.ClearAll();
        try
        {
            // 1. Basic Bounds Check
            int32_t instrumentId = orderTarget.OrderHeader.OrderId.InstrumentId();
            int32_t strategyId = orderTarget.OrderHeader.OrderId.StrategyId();
            int32_t clientId = orderTarget.OrderHeader.OrderId.ClientId();

            const Execution::OrderTarget& existingTarget = _serverContext.GetOrderTarget(orderTarget.OrderHeader.OrderId).GetReadonlyRef();
            const Execution::OrderState& orderState = _serverContext.GetOrderState(orderTarget.OrderHeader.OrderId).GetReadonlyRef();

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
                bool isAmend = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Amend;

                if (_orderRejectedSource == Execution::OrderRejectedSource::Server)
                {
                    orderRejectedReasons = ValidateOrderHeader(orderState.OrderHeader, orderTarget.OrderHeader);
                    if (!orderRejectedReasons.IsEmpty())
                    {
                        return false;
                    }

                    if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StateIsDone));

                    if (isAmend && orderState.OrderHeader.Seq + 1 == orderTarget.OrderHeader.Seq && orderState.OrderProfile == orderTarget.OrderProfile)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsActive));

                    if (existingTarget.OrderHeader.Seq > orderTarget.OrderHeader.Seq)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsStale));

                    if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Amend && orderState.OrderProfile.Side() != orderTarget.OrderProfile.Side())
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
                        if (isAmend && existingTarget.OrderTargetStatus == Execution::OrderStateStatus::Done && orderState.OrderProfile == orderTarget.OrderProfile)
                            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::TargetIsActive));
                    }

                    if (existingTarget.OrderHeader.Seq >= orderTarget.OrderHeader.Seq)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SeqOutOfOrder));

                    if (existingTarget.OrderTargetStatus == Execution::OrderStateStatus::Active) // lastTarget = newTarget ??
                    {
                        if (isAmend && existingTarget.OrderProfile == orderTarget.OrderProfile)
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

                    if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Amend && existingTarget.OrderProfile.Side() != orderTarget.OrderProfile.Side())
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SideNotValid));
                }
            }

            const Execution::PositionHeader& localPosition = _serverContext.GetPositionHeader(orderTarget.OrderHeader.OrderId.StrategyId(), orderTarget.OrderHeader.OrderId.InstrumentId()).GetReadonlyRef();

            bool isCancel = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Cancel;
            if (!isCancel && orderTarget.OrderHeader.OrderId.IsAlgoOrder() && localPosition.AlgoStatus == Execution::AlgoStatus::Paused)
            {
                orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::AlgoIsPaused));
                return false;
            }

            // Risk limits are owned by the server. A client maps the arrays read-only, so taking the
            // mutable refs below would throw and every client order would come back
            // ExceptionThrownByRiskLayer. It must not double-count exposure either.
            if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
                return orderRejectedReasons.IsEmpty();

            if (!orderRejectedReasons.IsEmpty())
                return false;

            Data::Instrument& instrument = _serverContext.GetInstrument(instrumentId);

            // Order-entry throttle, one rolling window per CoreGroup (a CoreGroup maps to an iLink
            // session, which is the scope CME throttles). One combined window sized at the tighter
            // line can never breach either exchange line, and what it costs is create and amend
            // throughput while cancels are flying. Plain ref, no seq bump - the CoreGroup thread
            // owns the row.
            if (_orderRejectedSource == Execution::OrderRejectedSource::Server)
            {
                Execution::RollingRateLimit& rollingRateLimit = _serverContext.GetRateLimit(instrument.Header().CoreGroupId).GetRef();

                // A cancel is counted but never refused: it is the message that reduces risk.
                if (isCancel)
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
            // Only check risk on New or Amend (increasing size)
            if (!isCancel)
            {

                int32_t quantityFilled = orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId ? orderState.QuantityFilled : 0;
                int32_t workingQuantity = orderTarget.OrderProfile.Quantity - quantityFilled;

                // Max order quantity per leg, in LEG units - before TryAdd, so rejects need no back-out.
                for (const Data::InstrumentLeg& leg : instrument.Legs())
                {
                    if (std::abs(workingQuantity * leg.Weight) > _serverContext.GetRiskLimit(leg.InstrumentId).GetReadonlyRef().MaxOrderQuantity)
                    {
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::QuantityExceedsRiskLimit));
                        return false;
                    }
                }

                int32_t ackedOrderQuantity = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create ? 0 : orderState.OrderProfile.Quantity;

                Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderTarget.OrderHeader.OrderId).GetRef();

                if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create)
                    orderRisk = Execution::OrderRisk{};

                // The ONE pre-verdict mutation, with its first-class inverse (Reject) on any breach.
                int32_t sign = orderTarget.OrderProfile.Sign();
                int32_t worstQuantityFilledBefore = orderRisk.GetAbsWorstOrderQuantity(ackedOrderQuantity);

                Execution::OrderRejectedReason reason = Execution::OrderRejectedReason::Unknown;
                if (!orderRisk.TryAdd(orderTarget.OrderProfile.Quantity, reason))
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(reason));
                    return false;
                }
                int32_t worstMagnitudeDelta = orderRisk.GetAbsWorstOrderQuantity(ackedOrderQuantity) - worstQuantityFilledBefore;

                // Phase 1 - PURE: check every leg, write nothing. The magnitude delta is >= 0, so
                // legDelta's own sign IS the leg's side - routing by the ORDER's sign corrupts every
                // negative-weight leg (a buy calendar reserves the back leg SHORT, not long).
                for (const Data::InstrumentLeg& leg : instrument.Legs())
                {
                    int32_t legDelta = worstMagnitudeDelta * sign * leg.Weight;
                    const Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(leg.InstrumentId).GetReadonlyRef();
                    int32_t quantity = _serverContext.GetPosition(leg.InstrumentId).Header().Quantity;

                    bool isRiskLimitExceeded = legDelta >= 0
                        ? quantity + riskLimit.WorstLongWorkingQuantity + legDelta > riskLimit.MaxPositionQuantity
                        : quantity + riskLimit.WorstShortWorkingQuantity + legDelta < -riskLimit.MaxPositionQuantity;

                    if (isRiskLimitExceeded)
                    {
                        orderRisk.Reject(orderTarget.OrderProfile.Quantity);
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::PositionExceedsRiskLimit));
                        return false;
                    }
                }

                // Phase 2 - commit through the SAME arithmetic the release hooks use. Single-writer:
                // nothing can change between the phases, so check-then-apply is atomic by ownership.
                ApplyWorstWorkingQuantityDelta(orderTarget.OrderHeader.OrderId, sign, worstMagnitudeDelta);
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