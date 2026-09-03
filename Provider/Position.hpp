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
        // Who owns this book's order slots: the client's id, or strategy 0 for server-wide rows.
        // The header's OrderId is not a sound source of identity (template rows carry ids before
        // any allocation), so the owner is fixed at construction, matching C#.
        const int32_t _clientId;

    public:
        Data::Instrument& Instrument;
        bool IsTradingSuspended;

        Position(Data::Instrument& instrument, Socket::SharedArrayEntry<Execution::PositionHeader> headerEntry, Context& context, int32_t clientId)
            : _context(context), _headerEntry(headerEntry), _isOrderActive(0), _clientId(clientId), Instrument(instrument), IsTradingSuspended(true)
        {
        }

        // Context keys order rows by OrderId; build one whose GlobalIndex is (clientId, localIndex).
        ALWAYS_INLINE Execution::OrderId GetOrderId(int32_t localOrderIndex) const
        {
            return Execution::OrderId().ClientId(_clientId).LocalIndex(localOrderIndex);
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