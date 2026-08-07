#include "Order.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

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

int main()
{
    using Execution::OrderId;
    using Execution::OrderIdAllocator;

    // ---------------------------------------------------------------------
    // Struct sizes (wire contract; also compile-time static_asserts in the headers)
    // ---------------------------------------------------------------------
    CHECK(sizeof(Execution::OrderId) == 8);
    CHECK(sizeof(Execution::OrderHeader) == 28);
    CHECK(sizeof(Execution::OrderState) == 60);
    CHECK(sizeof(Execution::Fill) == 52);
    CHECK(sizeof(Execution::OrderRejected) == 52);
    CHECK(sizeof(Execution::OrderTarget) == 44);
    CHECK(sizeof(Execution::PositionHeader) == 57);

    // ---------------------------------------------------------------------
    // Bit-budget constants
    // ---------------------------------------------------------------------
    CHECK(OrderIdAllocator::OrdersPerClient == 64);
    CHECK(OrderIdAllocator::MaxClientId == 63);
    CHECK(OrderIdAllocator::MaxStrategyId == 63);
    CHECK(OrderIdAllocator::NoInstrumentId == 16383);   // the field's top value, reserved: names no instrument
    CHECK(OrderIdAllocator::MaxInstrumentId == 16382);   // so the highest allocatable instrument is one below it
    CHECK(OrderIdAllocator::IndexBits == 12);

    // The reserved value is never allocatable: range validation refuses it, so nothing can hand out an
    // id that would later read as "no instrument".
    {
        bool refusedReserved = false, acceptedHighest = true;
        try { OrderIdAllocator::ThrowIfInstrumentIdOutOfRange(OrderIdAllocator::NoInstrumentId); }
        catch (const std::out_of_range&) { refusedReserved = true; }
        try { OrderIdAllocator::ThrowIfInstrumentIdOutOfRange(OrderIdAllocator::MaxInstrumentId); }
        catch (const std::out_of_range&) { acceptedHighest = false; }
        CHECK(refusedReserved);
        CHECK(acceptedHighest);
    }

    // ---------------------------------------------------------------------
    // Per-field set/get round-trips at corner values (0 and max per field, mixed)
    // ---------------------------------------------------------------------
    {
        OrderId id;
        CHECK(id.ClientOrderId == 0ULL);
        id.LocalIndex(63).ClientId(0).StrategyId(63).InstrumentId(0).Generation(0xFFFFFFFFu);
        CHECK(id.LocalIndex() == 63);
        CHECK(id.ClientId() == 0);
        CHECK(id.StrategyId() == 63);
        CHECK(id.InstrumentId() == 0);
        CHECK(id.Generation() == 0xFFFFFFFFu);
    }
    {
        OrderId id;
        id.LocalIndex(63).ClientId(63).StrategyId(63).InstrumentId(OrderIdAllocator::NoInstrumentId).Generation(0xFFFFFFFFu);
        CHECK(id.ClientOrderId == 0xFFFFFFFFFFFFFFFFULL); // every field at its top value fills the word exactly

        id.LocalIndex(0).ClientId(0).StrategyId(0).InstrumentId(0).Generation(0);
        CHECK(id.ClientOrderId == 0ULL);
    }

    // ---------------------------------------------------------------------
    // Setter isolation: writing one field never disturbs the others;
    // a value wider than its field wraps within the field only
    // ---------------------------------------------------------------------
    {
        OrderId id{0xFFFFFFFFFFFFFFFFULL};
        id.ClientId(0);
        CHECK(id.ClientId() == 0);
        CHECK(id.LocalIndex() == 63);
        CHECK(id.StrategyId() == 63);
        CHECK(id.InstrumentId() == OrderIdAllocator::NoInstrumentId);
        CHECK(id.Generation() == 0xFFFFFFFFu);

        id.ClientId(64 + 5); // 7 bits -> wraps to 5 within the 6-bit field
        CHECK(id.ClientId() == 5);
        CHECK(id.LocalIndex() == 63);   // low neighbour untouched
        CHECK(id.StrategyId() == 63);   // high neighbour untouched
    }

    // ---------------------------------------------------------------------
    // Template flow: build template -> stamp identity -> allocate
    // ---------------------------------------------------------------------
    {
        Tools::Bitset64 active(0ULL);

        OrderId a;
        a.ClientId(3).StrategyId(3).InstrumentId(777);
        CHECK(!a.IsAllocated()); // generation 0 == template
        CHECK(OrderIdAllocator::TryAllocate(active, a));
        CHECK(a.IsAllocated());
        CHECK(a.LocalIndex() == 0);
        CHECK(a.ClientId() == 3);
        CHECK(a.StrategyId() == 3);
        CHECK(a.InstrumentId() == 777); // caller's ids preserved
        CHECK(a.GlobalIndex() == OrderIdAllocator::ToGlobalIndex(3, 0));

        OrderId b;
        b.ClientId(3).StrategyId(3).InstrumentId(777);
        CHECK(OrderIdAllocator::TryAllocate(active, b));
        CHECK(b.LocalIndex() == 1);                              // lowest free slot
        CHECK(static_cast<uint64_t>(b) > static_cast<uint64_t>(a)); // monotonic raw-u64 ordering (generation in top 32 bits)

        // Reuse flow: re-activating a busy slot fails; free then re-activate returns the identical id
        OrderId aCopy = a;
        CHECK(!OrderIdAllocator::TryAllocate(active, aCopy)); // slot busy
        OrderIdAllocator::Free(active, a);
        CHECK(OrderIdAllocator::TryAllocate(active, aCopy)); // re-activate
        CHECK(aCopy == a);                                   // identical value back

        // Exhaustion: template left intact so the rejection can report its instrument
        Tools::Bitset64 full(0xFFFFFFFFFFFFFFFFULL);
        OrderId t;
        t.InstrumentId(42);
        CHECK(!OrderIdAllocator::TryAllocate(full, t));
        CHECK(!t.IsAllocated());
        CHECK(t.InstrumentId() == 42);
    }

    // ---------------------------------------------------------------------
    // u64 interop: implicit conversion round-trip, equality, ordering,
    // and derived accessors agree with the allocator decode functions
    // ---------------------------------------------------------------------
    {
        OrderId id;
        id.LocalIndex(9).ClientId(7).StrategyId(11).InstrumentId(1234).Generation(99);

        uint64_t raw = id;   // implicit OrderId -> u64
        OrderId round = raw; // implicit u64 -> OrderId
        CHECK(round == id);
        CHECK(raw == id.ClientOrderId);
        CHECK(id > 0);

        CHECK(OrderIdAllocator::GetLocalIndex(raw) == id.LocalIndex());
        CHECK(OrderIdAllocator::GetClientId(raw) == id.ClientId());
        CHECK(OrderIdAllocator::GetStrategyId(raw) == id.StrategyId());
        CHECK(OrderIdAllocator::GetInstrumentId(raw) == id.InstrumentId());
        CHECK(OrderIdAllocator::GetGlobalIndex(raw) == id.GlobalIndex());
        CHECK(OrderIdAllocator::GetGeneration(raw) == id.Generation());
    }

    // ---------------------------------------------------------------------
    // OrderHeader.OrderId decodes the packed id - reached explicitly, no header shortcuts
    // ---------------------------------------------------------------------
    {
        Execution::OrderHeader header{};
        header.Seq = 7;
        header.OrderId = OrderId().LocalIndex(9).ClientId(5).StrategyId(5).InstrumentId(300).Generation(1000);

        CHECK(header.OrderId.ClientId() == 5);
        CHECK(header.OrderId.StrategyId() == 5);
        CHECK(header.OrderId.InstrumentId() == 300);
        CHECK(header.OrderId.LocalIndex() == 9);
        CHECK(header.OrderId.GlobalIndex() == OrderIdAllocator::ToGlobalIndex(5, 9));
        CHECK(header.OrderId.IsAlgoOrder());

        // JSON: OrderId serializes as an object - ClientOrderId (raw, source of truth) + decoded props
        std::string json = Tools::Json::SerializeToLine(header);
        CHECK(json.find("\"ClientOrderId\":" + std::to_string(header.OrderId.ClientOrderId)) != std::string::npos);
        CHECK(json.find("\"ClientId\":5") != std::string::npos);       // decoded prop emitted for readability
        CHECK(json.find("\"InstrumentId\":300") != std::string::npos);
        CHECK(json.find("\"IsAllocated\":true") != std::string::npos);

        // Round-trip: read side takes only the raw ClientOrderId; the object reconstructs losslessly
        Execution::OrderHeader roundTrip = Tools::Json::Deserialize<Execution::OrderHeader>(json);
        CHECK(roundTrip.OrderId == header.OrderId);
        CHECK(roundTrip.Seq == header.Seq);
        CHECK(roundTrip.OrderId.ClientId() == 5);
        CHECK(roundTrip.OrderId.InstrumentId() == 300);
    }

    // ---------------------------------------------------------------------
    // OrderState carries ExchangeOrderId (moved off the header); JSON round-trip
    // ---------------------------------------------------------------------
    {
        Execution::OrderState state{};
        state.OrderHeader.OrderId = OrderId().ClientId(3).StrategyId(3).InstrumentId(88).Generation(5);
        state.ExchangeOrderId = 0xABCDEF0123456789ULL;

        std::string json = Tools::Json::SerializeToLine(state);
        Execution::OrderState roundTrip = Tools::Json::Deserialize<Execution::OrderState>(json);
        CHECK(roundTrip.ExchangeOrderId == state.ExchangeOrderId);
        CHECK(roundTrip.OrderHeader.OrderId == state.OrderHeader.OrderId);
    }

    // ---------------------------------------------------------------------
    // PositionHeader JSON round-trip (pre-existing smoke test, new layout)
    // ---------------------------------------------------------------------
    {
        Execution::PositionHeader positionHeader
        {
            .Header = Data::Header<Execution::OrderType>(Execution::OrderType::Position),
            .OrderHeader = {},
            .Quantity = 10,
            .AvgPrice = std::numeric_limits<double>::quiet_NaN(),
            .RealizedProfit = 0,
        };
        positionHeader.OrderHeader.OrderId = OrderId().ClientId(2).StrategyId(2).InstrumentId(55);

        std::string json = Tools::Json::Serialize(positionHeader);
        Execution::PositionHeader positionHeader2 = Tools::Json::Deserialize<Execution::PositionHeader>(json);
        CHECK(positionHeader2.OrderHeader.OrderId == positionHeader.OrderHeader.OrderId);
        CHECK(positionHeader2.Quantity == 10);
    }

    if (s_failures == 0)
    {
        std::cout << "ExecutionTests PASSED" << std::endl;
        return 0;
    }

    std::cout << "ExecutionTests FAILED: " << s_failures << " check(s)" << std::endl;
    return 1;
}
