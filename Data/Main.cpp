#include "MarketByPrice.hpp"
#include "Tick.hpp"
#include <cstring>
#include <iostream>
#include <random>

// Minimal check harness: prints each failure, returns non-zero if any check failed.
static int32_t s_failures = 0;
#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::cout << "FAIL  " << #cond << "  (line " << __LINE__ << ")" << std::endl;        \
            ++s_failures;                                                                        \
        }                                                                                        \
    } while (0)

static int32_t SumLevels(const Data::SideByPrice64& side)
{
    int32_t sum = 0;
    Data::SideByPrice64::Enumerator enumerator = side.GetEnumerator();
    while (enumerator.MoveNext())
        sum += enumerator.Current().Quantity;
    return sum;
}

static void CheckQuantityInvariant(Data::Side sideValue, uint32_t seed)
{
    std::mt19937 rng(seed);
    Data::SideByPrice64 side(sideValue);
    int32_t center = 1000;
    int32_t delta;
    int32_t mismatches = 0;

    for (int32_t i = 0; i < 200000; i++)
    {
        int32_t roll = static_cast<int32_t>(rng() % 100);
        int32_t ticks;
        if (roll < 2)
            ticks = center + static_cast<int32_t>(rng() % 400) - 200;           // far re-anchors and far-worse drops
        else if (roll < 10)
            ticks = side.BestTicks() + (static_cast<int32_t>(sideValue) * static_cast<int32_t>(rng() % 70)); // better prices, aliased into the window
        else
            ticks = side.BestTicks() - (static_cast<int32_t>(sideValue) * static_cast<int32_t>(rng() % 70)); // worse, incl. evictions at the window edge

        if (roll == 0)
            center = ticks;

        int32_t quantity = (rng() % 3 == 0) ? 0 : static_cast<int32_t>(rng() % 50) + 1;
        side.TrySetQuantity(ticks, quantity, delta);

        if (side.Quantity() != SumLevels(side))
            mismatches++;
    }
    CHECK(mismatches == 0);
}

int main()
{
    std::cout << "Data Tests" << std::endl;

    // ---------------------------------------------------------------------
    // Wire layout
    // ---------------------------------------------------------------------
    CHECK(sizeof(Data::SideByPrice64) == 320);
    CHECK(sizeof(Data::MarketByPrice64) == 664);
    CHECK(sizeof(Data::Settlement) == 64);
    CHECK(offsetof(Data::Settlement, Price) == 32);

    // ---------------------------------------------------------------------
    // SideByPrice64::Quantity == sum of the enumerated levels after every TrySetQuantity
    // ---------------------------------------------------------------------
    for (uint32_t seed = 1; seed <= 4; seed++)
    {
        CheckQuantityInvariant(Data::Side::Buy, seed);
        CheckQuantityInvariant(Data::Side::Sell, seed);
    }

    {
        Data::SideByPrice64 bids(Data::Side::Buy);
        int32_t delta;
        bids.TrySetQuantity(100, 5, delta);
        bids.TrySetQuantity(99, 7, delta);
        CHECK(bids.Quantity() == 12);
        // better by exactly the capacity: the slot aliases the best level, which is overwritten
        bids.TrySetQuantity(164, 3, delta);
        CHECK(bids.Quantity() == SumLevels(bids));
        // far better: re-anchor, every other level dropped
        bids.TrySetQuantity(400, 9, delta);
        CHECK(bids.Quantity() == 9);
        CHECK(bids.Count() == 1);
        // far better zero and far worse: ignored
        CHECK(!bids.TrySetQuantity(1000, 0, delta));
        CHECK(!bids.TrySetQuantity(300, 4, delta));
        CHECK(bids.Quantity() == 9);
    }

    // ---------------------------------------------------------------------
    // MarketByPrice64::Clear rebuilds both sides (re-stamps Side, zeroes Quantity)
    // ---------------------------------------------------------------------
    {
        Data::MarketByPrice64 mbp;
        int32_t delta;
        mbp.TrySetBidQuantity(100, 5, delta);
        mbp.TrySetAskQuantity(101, 6, delta);
        std::memset(static_cast<void*>(&mbp.Bids), 0, sizeof(mbp.Bids));
        mbp.Clear();
        CHECK(mbp.Bids.Side() == Data::Side::Buy);
        CHECK(mbp.Asks.Side() == Data::Side::Sell);
        CHECK(mbp.Bids.Quantity() == 0);
        CHECK(mbp.Asks.Quantity() == 0);
        CHECK(mbp.IsEmpty());
    }

    // ---------------------------------------------------------------------
    // Quote::MicroPrice
    // ---------------------------------------------------------------------
    {
        Data::Quote quote{ .TickSize = 0.5, .Bid = Data::Level(100, 1), .Ask = Data::Level(102, 3) };
        CHECK(quote.MicroPrice() == (100 * 3 + 102 * 1) * 0.5 / 4);
    }

    if (s_failures != 0)
    {
        std::cout << s_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All Data tests passed" << std::endl;
    return 0;
}
