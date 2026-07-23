//BEGIN_FILE HFT/Server/Position.hpp
#pragma once

#include <cmath>
#include "Order.hpp"
#include "Instrument.hpp"
#include "Bitset.hpp"
#include "Tick.hpp"
#include "Timestamp.hpp"

namespace Provider
{
    class Context; // Forward declare

    struct Profit
    {
        Tools::Timestamp Timestamp;
        double Total;
        double Floating;
        double Realized;
        int32_t Quantity;
        double AvgPrice;
        double MidPrice;

        Profit(Tools::Timestamp timestamp, double total, double floating, double realized, int32_t quantity, double avgPrice, double midPrice)
            : Timestamp(timestamp), Total(total), Floating(floating), Realized(realized), Quantity(quantity), AvgPrice(avgPrice), MidPrice(midPrice) {}
    };

    class Position
    {
    private:
        Context& _context;
        Socket::SharedArrayEntry<Execution::PositionHeader> _headerEntry;
        Tools::Bitset64 _isOrderActive;

    public:
        Data::Instrument& Instrument;
        bool IsTradingSuspended;

        Position(Data::Instrument& instrument, Socket::SharedArrayEntry<Execution::PositionHeader> headerEntry, Context& context)
            : _context(context), _headerEntry(headerEntry), _isOrderActive(0), Instrument(instrument), IsTradingSuspended(true)
        {
        }

        const Execution::PositionHeader& Header() { return _headerEntry.GetReadonlyRef(); }

        Profit GetProfit()
        {
            if (_headerEntry.IsEmpty())
                return Profit(Tools::Timestamp::UtcNow(), std::nan(""), std::nan(""), 0.0, 0, std::nan(""), std::nan(""));

            Execution::PositionHeader positionHeader = _headerEntry.Read();
            Data::Quote quote
            {
                .TickSize = 0.0,
                .Bid = Data::Level { .Ticks = 0, .Quantity = 0 },
                .Ask = Data::Level { .Ticks = 0, .Quantity = 0 }
            };

            if (Instrument.TryGetQuote(quote))
            {
                double floating = Instrument.GetProfit(positionHeader.AvgPrice, quote.MidPrice(), positionHeader.Quantity);
                return Profit(Tools::Timestamp::UtcNow(), floating + positionHeader.RealizedProfit, floating, positionHeader.RealizedProfit, positionHeader.Quantity, positionHeader.AvgPrice, quote.MidPrice());
            }
            else
            {
                return Profit(Tools::Timestamp::UtcNow(), std::nan(""), std::nan(""), positionHeader.RealizedProfit, positionHeader.Quantity, positionHeader.AvgPrice, std::nan(""));
            }
        }

        void OnOrderDone(int32_t localOrderIndex)
        {
            _isOrderActive.Clear(localOrderIndex);
        }

        void OnOrderActive(int32_t localOrderIndex)
        {
            _isOrderActive.Set(localOrderIndex);
        }

        Tools::Bitset64 IsOrderActive() const { return _isOrderActive; }

        bool TryGetQuote(Data::Quote& quote); // Implemented inline in Context.hpp to avoid circular references
    };
}

//END_FILE HFT/Server/Position.hpp