

#include "Client.hpp"
#include "Instrument.hpp"
#include "MarketByPrice.hpp"
#include "Order.hpp"
#include "SharedArray.hpp"
#include "Tick.hpp"
#include "Tools.hpp"
#include <cstdint>

namespace Strategy
{

class Strategy
{
    Provider::Client& _client;
    Data::Instrument& _instrument;
    Provider::Position& _position;

public:
    Strategy(Provider::Client& client, Data::Instrument& instrument)
    :   _client(client),
        _instrument(instrument),
        _position(client.ClientContext.GetPosition(instrument.InstrumentId))
    {

        _client.OrderState = [](const Execution::OrderState&)
        {
        };

        _client.Fill = [](const Execution::Fill& fill)
        {
            Tools::PrintLine(fill.ToString());
        };
        
        _client.Trade = [](const Data::Trade&)
        {

        };
        
        _client.MarketByPrice = [this](const Data::MarketByPrice& mbp, std::span<uint8_t>) 
        {
            Execute(mbp.TickHeader.InstrumentId);
        };
    }



    void Execute(int32_t instrumentId)
    {     
        Data::MarketByPrice64 market = _client.ClientContext.GetMarketByPrice64(instrumentId).Read();

        int32_t offset = 1;

        if (market.BidsCount() > 0 && market.AsksCount() > 0)
        {
            Data::Level bestBid = market.BestBid();
            Data::Level bestAsk = market.BestAsk();


            Execution::OrderProfile buy
            {
                .Ticks = bestBid.Ticks - offset,
                .Quantity = 1,
            };

            Execution::OrderProfile sell
            {
                .Ticks = bestAsk.Ticks + offset,
                .Quantity = -1,
            };
            
            Target(buy, sell);
        }
        



    }

    void Target(Execution::OrderProfile buy, Execution::OrderProfile sell)
    {
        const Execution::OrderState& buyState = _client.ClientContext.GetOrderState(0).GetReadonlyRef();
        const Execution::OrderState& sellState = _client.ClientContext.GetOrderState(1).GetReadonlyRef();
        const Execution::OrderTarget& buyTarget = _client.ClientContext.GetOrderTarget(0).GetReadonlyRef();
        const Execution::OrderTarget& sellTarget = _client.ClientContext.GetOrderTarget(1).GetReadonlyRef();

        Execution::OrderProfile buyStateProfile = buyState.OrderHeader.OrderId == buyTarget.OrderHeader.OrderId ? buyState.OrderProfile : buyTarget.OrderProfile;
        Execution::OrderProfile sellStateProfile = sellState.OrderHeader.OrderId == sellTarget.OrderHeader.OrderId ? sellState.OrderProfile : sellTarget.OrderProfile;
        

        if (buyStateProfile != buy)
        {
            if (buyState.OrderHeader.OrderId > 0 && buyState.OrderStateStatus == Execution::OrderStateStatus::Active)
            {
                Amend(buyTarget, buy);
            }
            else
            {
                Create(buy);
            }
        }

        if (sellStateProfile != sell)
        {
            if (sellState.OrderHeader.OrderId > 0 && sellState.OrderStateStatus == Execution::OrderStateStatus::Active)
            {
                Amend(sellTarget, sell);
            }
            else
            {
                Create(sell);
            }
        }
    }


    void Create(Execution::OrderProfile orderProfile)
    {
        Execution::OrderTarget orderTarget
        {
            .Header = Data::Header<Execution::OrderType>(Execution::OrderType::OrderTarget),
            .OrderHeader =
            {
                // Template id: instrument only - Client::Create stamps ClientId/StrategyId and allocates.
                .OrderId = Execution::OrderId().InstrumentId(_instrument.InstrumentId),
                .ExchangeTimestamp = Tools::Timestamp(0),
                .NicTimestamp = Tools::Timestamp(0),
            },
            .OrderTargetAction = Execution::OrderTargetAction::Create,
            .OrderTargetStatus = Execution::OrderStateStatus::Active,
            .OrderProfile = orderProfile,
        };
        _client.OnOrderTarget(orderTarget);
    }

    void Amend(Execution::OrderTarget orderTarget, Execution::OrderProfile orderProfile)
    {
        orderTarget.OrderTargetAction = Execution::OrderTargetAction::Amend;
        orderTarget.OrderProfile = orderProfile;
        orderTarget.OrderHeader.Seq++;
        orderTarget.OrderTargetStatus = Execution::OrderStateStatus::Active;
        _client.OnOrderTarget(orderTarget);
    }

};

}

