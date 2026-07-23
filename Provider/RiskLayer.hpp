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
    Provider::ServerContext _serverContext;
    std::vector<uint64_t> _maxClientOrderIds;
    Execution::OrderRejectedSource _orderRejectedSource;

public:
    explicit RiskLayer(const std::filesystem::path& serverName, Execution::OrderRejectedSource orderRejectedSource)
    : _serverContext(serverName, Tools::Access::Read), _orderRejectedSource(orderRejectedSource)
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

            const Execution::RiskLimit& riskLimit = _serverContext.GetRiskLimit(instrumentId).GetReadonlyRef();
            const Execution::PositionHeader& localPosition = _serverContext.GetPositionHeader(orderTarget.OrderHeader.OrderId.StrategyId(), orderTarget.OrderHeader.OrderId.InstrumentId()).GetReadonlyRef();

            bool isCancel = orderTarget.OrderTargetAction == Execution::OrderTargetAction::Cancel;
            if (!isCancel && orderTarget.OrderHeader.OrderId.IsAlgoOrder() && localPosition.AlgoStatus == Execution::AlgoStatus::Paused)
            {
                orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::AlgoIsPaused));
                return false;
            }

            // 10. RISK LIMITS
            // Only check risk on New or Amend (increasing size)
            if (!isCancel)
            {
                int32_t quantityFilled = orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId ? orderState.QuantityFilled : 0;
                int32_t workingQuantity = orderTarget.OrderProfile.Quantity - quantityFilled;
                int32_t absWorkingQuantity = std::abs(workingQuantity);

                // Max Order Quantity
                if (absWorkingQuantity > riskLimit.MaxOrderQuantity)
                {
                    orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::QuantityTooLarge));
                }

                Position& serverPosition = _serverContext.GetPosition(instrumentId);
                int32_t currentPosition = serverPosition.Header().Quantity;

                int32_t projectedPosition = currentPosition + workingQuantity;

                if (std::abs(projectedPosition) > riskLimit.MaxPositionQuantity)
                {
                    bool isIncreasing = std::abs(projectedPosition) > std::abs(currentPosition);

                    if (isIncreasing)
                    {
                        orderRejectedReasons.Set(static_cast<int32_t>(Execution::OrderRejectedReason::PositionTooLarge));
                    }
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