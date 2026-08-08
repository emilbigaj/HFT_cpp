//BEGIN_FILE HFT/Provider/RiskLayer.hpp
#pragma once

#include "Context.hpp"
#include "Order.hpp"
#include "Bitset.hpp"
#include "Tools.hpp"
#include <cmath>
#include <filesystem>
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
    std::vector<uint64_t> _maxClientOrderIds;
    Execution::OrderRejectedSource _orderRejectedSource;

public:
    RiskLayer(Provider::ServerContext& serverContext, Execution::OrderRejectedSource orderRejectedSource)
    : _serverContext(serverContext), _orderRejectedSource(orderRejectedSource)
    {
        _maxClientOrderIds.resize(static_cast<size_t>(_serverContext.ServerHeader().GetReadonlyRef().ClientIds.Length()), 0ULL);
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

        if (!instrument.IsInSession())
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::NotInSession));
        }

        return orderRejectedReasons;
    }

    ALWAYS_INLINE Tools::Bitset64 ValidateCreate(const Execution::OrderTarget& orderTarget, const Execution::OrderState& orderState)
    {
        int clientId = orderTarget.OrderHeader.OrderId.ClientId();
        Tools::Bitset64 orderRejectedReasons;
        if (orderTarget.OrderHeader.Seq != 1)
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::SeqOutOfOrder));
        }

        if (orderTarget.OrderHeader.OrderId <= _maxClientOrderIds[static_cast<size_t>(clientId)])
        {
            orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::ClientOrderIdOutOfOrder));
        }
        else
        {
            _maxClientOrderIds[static_cast<size_t>(clientId)] = orderTarget.OrderHeader.OrderId;
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

    // An ack retires the target that produced it. The slot's worst case drops to the highest quantity
    // still unacked, so the instrument aggregate releases the difference.
    ALWAYS_INLINE void OnOrderState(const Execution::OrderState& orderState)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        if (orderState.OrderStateReason == Execution::OrderStateReason::Acked)
        {
            Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantityBefore = orderRisk.GetWorstOrderQuantity(orderState.OrderProfile.Quantity);
            orderRisk.Ack(orderState.OrderProfile.Quantity);
            int32_t worstOrderQuantityAfter = orderRisk.GetWorstOrderQuantity(orderState.OrderProfile.Quantity);
            int32_t worstOrderQuantityDelta = (worstOrderQuantityAfter - worstOrderQuantityBefore) * orderState.OrderProfile.Sign();

            if (worstOrderQuantityDelta == 0)
                return;

            Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(orderState.OrderHeader.OrderId.InstrumentId()).GetRef();
            riskLimit.WorstLongWorkingQuantity += worstOrderQuantityDelta * (side == Data::Side::Buy ? 1 : 0);
            riskLimit.WorstShortWorkingQuantity += worstOrderQuantityDelta * (side == Data::Side::Sell ? 1 : 0);
        }
        else if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
        {
            // Release on Done rather than on a reason match: a cancel, reject or expiry that carries a
            // label this switch does not know would otherwise leak its whole reservation, permanently.
            Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderState.OrderHeader.OrderId).GetRef();
            Data::Side side = orderState.OrderProfile.Side();

            int32_t worstOrderQuantity = orderRisk.GetWorstOrderQuantity(orderState.OrderProfile.Quantity);
            int32_t released = worstOrderQuantity - orderState.QuantityFilled;

            orderRisk = Execution::OrderRisk{};

            if (released == 0)
                return;

            Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(orderState.OrderHeader.OrderId.InstrumentId()).GetRef();
            riskLimit.WorstLongWorkingQuantity -= released * (side == Data::Side::Buy ? 1 : 0);
            riskLimit.WorstShortWorkingQuantity -= released * (side == Data::Side::Sell ? 1 : 0);
        }
    }

    // A fill converts reservation into position, so the reservation shrinks by exactly the fill.
    ALWAYS_INLINE void OnFill(const Execution::Fill& fill)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        Data::Side side = fill.OrderProfile.Side();
        Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(fill.OrderHeader.OrderId.InstrumentId()).GetRef();
        riskLimit.WorstLongWorkingQuantity -= fill.OrderProfile.Quantity * (side == Data::Side::Buy ? 1 : 0);
        riskLimit.WorstShortWorkingQuantity -= fill.OrderProfile.Quantity * (side == Data::Side::Sell ? 1 : 0);
    }

    // An exchange reject retires exactly the target it names; a server reject never reserved anything.
    ALWAYS_INLINE void OnOrderRejected(const Execution::OrderRejected& orderRejected)
    {
        if (_orderRejectedSource != Execution::OrderRejectedSource::Server)
            return;

        if (orderRejected.OrderRejectedSource == Execution::OrderRejectedSource::Server)
            return;

        const Execution::OrderState& orderState = _serverContext.GetOrderState(orderRejected.OrderHeader.OrderId.GlobalIndex()).GetReadonlyRef();
        Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderRejected.OrderHeader.OrderId).GetRef();
        Data::Side side = orderRejected.OrderProfile.Side();

        int32_t worstOrderQuantityBefore = orderRisk.GetWorstOrderQuantity(orderState.OrderProfile.Quantity);
        orderRisk.Reject(orderRejected.OrderProfile.Quantity);
        int32_t worstOrderQuantityAfter = orderRisk.GetWorstOrderQuantity(orderState.OrderProfile.Quantity);
        int32_t worstOrderQuantityDelta = worstOrderQuantityAfter - worstOrderQuantityBefore;

        if (worstOrderQuantityDelta == 0)
            return;

        Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(orderRejected.OrderHeader.OrderId.InstrumentId()).GetRef();
        riskLimit.WorstLongWorkingQuantity += worstOrderQuantityDelta * (side == Data::Side::Buy ? 1 : 0);
        riskLimit.WorstShortWorkingQuantity += worstOrderQuantityDelta * (side == Data::Side::Sell ? 1 : 0);
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

            int32_t globalOrderIndex = orderTarget.OrderHeader.OrderId.GlobalIndex();
            const Execution::OrderTarget& existingTarget = _serverContext.GetOrderTarget(globalOrderIndex).GetReadonlyRef();
            const Execution::OrderState& orderState = _serverContext.GetOrderState(globalOrderIndex).GetReadonlyRef();

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
                if (_orderRejectedSource == Execution::OrderRejectedSource::Server)
                {
                    orderRejectedReasons = ValidateOrderHeader(orderState.OrderHeader, orderTarget.OrderHeader);
                    if (!orderRejectedReasons.IsEmpty())
                    {
                        return false;
                    }

                    if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::StateIsDone));

                    if (orderState.OrderHeader.Seq + 1 == orderTarget.OrderHeader.Seq && orderState.OrderProfile == orderTarget.OrderProfile)
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

                    bool isAmend = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Amend;

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
                        if (existingTarget.OrderTargetAction == Execution::OrderTargetAction::Cancel)
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

            // 10. RISK LIMITS
            // Only check risk on New or Amend (increasing size)
            if (!isCancel)
            {
                Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(instrumentId).GetRef();

                int32_t quantityFilled = orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId ? orderState.QuantityFilled : 0;
                int32_t workingQuantity = orderTarget.OrderProfile.Quantity - quantityFilled;
                int32_t absWorkingQuantity = std::abs(workingQuantity);

                // Max Order Quantity
                if (absWorkingQuantity > riskLimit.MaxOrderQuantity)
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::QuantityExceedsRiskLimit));
                    return false;
                }

                int32_t ackedOrderQuantity = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create ? 0 : orderState.OrderProfile.Quantity;

                Execution::OrderRisk& orderRisk = _serverContext.GetOrderRisk(orderTarget.OrderHeader.OrderId).GetRef();

                if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create)
                    orderRisk = Execution::OrderRisk{};

                int32_t sign = orderTarget.OrderProfile.Sign();
                int32_t worstQuantityFilledBefore = orderRisk.GetWorstOrderQuantity(ackedOrderQuantity) * sign;

                Execution::OrderRejectedReason reason = Execution::OrderRejectedReason::Unknown;
                if (!orderRisk.TryAdd(orderTarget.OrderProfile.Quantity, reason))
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(reason));
                    return false;
                }

                int32_t worstQuantityFilledAfter = orderRisk.GetWorstOrderQuantity(ackedOrderQuantity) * sign;
                // GetWorstOrderQuantity is a magnitude. Both aggregates are signed — long positive,
                // short negative, which is what GetShortQuantityAllowance and the position check below
                // assume — so the delta has to carry the order's sign. Adding an unsigned delta to
                // WorstShortWorkingQuantity drives it positive, and then the short leg of the position
                // check can never trip however much is working.
                int32_t worstWorkingQuantityDelta = (worstQuantityFilledAfter - worstQuantityFilledBefore) * sign;

                // branchless
                int32_t worstLongWorkingQuantity = riskLimit.WorstLongWorkingQuantity + worstWorkingQuantityDelta * (sign == 1 ? 1 : 0);
                int32_t worstShortWorkingQuantity = riskLimit.WorstShortWorkingQuantity + worstWorkingQuantityDelta * (sign == -1 ? 1 : 0);

                Position& serverPosition = _serverContext.GetPosition(instrumentId);
                int32_t quantity = serverPosition.Header().Quantity;
                int32_t worstLongQuantity = quantity + worstLongWorkingQuantity;
                int32_t worstShortQuantity = quantity + worstShortWorkingQuantity;

                if (worstLongQuantity > riskLimit.MaxPositionQuantity || worstShortQuantity < -riskLimit.MaxPositionQuantity)
                {
                    orderRisk.Reject(orderTarget.OrderProfile.Quantity);
                    orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::PositionExceedsRiskLimit));
                }
                else
                {
                    riskLimit.WorstLongWorkingQuantity = worstLongWorkingQuantity;
                    riskLimit.WorstShortWorkingQuantity = worstShortWorkingQuantity;
                }
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