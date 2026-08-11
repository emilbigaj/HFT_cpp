#pragma once

#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include "Bitset.hpp"
#include "Json.hpp"
#include "Tools.hpp"

namespace Execution
{
    struct OrderId;

    // This class is not thread-safe. Only one thread should ever use it.
    class OrderIdAllocator
    {
    public:

        // --- Configuration ---
        // 64-bit id layout (low -> high): localIndex 6 | clientId 6 | strategyId 6 | instrumentId 14 | generation 32.
        // These bit widths are the wire contract with the C# implementation - change them in lockstep.
        static constexpr int32_t LocalIndexBits = 6;
        static constexpr int32_t ClientBits = 6;
        static constexpr int32_t StrategyBits = 6;
        static constexpr int32_t InstrumentBits = 14;
        static constexpr int32_t GenerationBits = 32;
        static constexpr int32_t IndexBits = LocalIndexBits + ClientBits; // global order index
        static constexpr int32_t OrdersPerClient = 1 << LocalIndexBits;
        static constexpr int32_t OrdersPerClientBitShift = LocalIndexBits;
        // The house book. Reserved before any client can connect so the allocator can never hand it
        // out, and so the slot stays addressable by the id/allocation guards. See Spec.md.
        static constexpr int32_t ServerStrategyId = 0;
        static constexpr int32_t MaxClientId = (1 << ClientBits) - 1;         // 63
        static constexpr int32_t MaxStrategyId = (1 << StrategyBits) - 1;     // 63
        // The instrument field's top value names no instrument, and is reserved at every size. An id that
        // carries no instrument — a session-wide request, an unresolved reference — must not read as
        // instrument 0, nor as whatever the highest real instrument becomes once the allocated space grows.
        // Reserving the top of the field rather than the top of today's allocation is what makes that hold,
        // and ThrowIfInstrumentIdOutOfRange below already refuses everything above MaxInstrumentId.
        static constexpr int32_t NoInstrumentId = (1 << InstrumentBits) - 1;  // 16 383
        static constexpr int32_t MaxInstrumentId = NoInstrumentId - 1;        // 16 382

    private:

        // --- Derived Masks & Shifts ---
        static constexpr uint64_t s_indexMask = (1ULL << IndexBits) - 1ULL;
        static constexpr uint64_t s_strategyMask = (1ULL << StrategyBits) - 1ULL;
        static constexpr uint64_t s_instrumentMask = (1ULL << InstrumentBits) - 1ULL;

        static constexpr int32_t s_strategyShift = IndexBits;
        static constexpr int32_t s_instrumentShift = IndexBits + StrategyBits;
        static constexpr int32_t s_generationShift = IndexBits + StrategyBits + InstrumentBits;

        // --- State ---
        inline static uint64_t s_generation = static_cast<uint64_t>(std::time(nullptr));

        OrderIdAllocator() = delete;

    public:

        // Pure slot/generation allocation: ClientId/StrategyId/InstrumentId are already inside orderId
        // (stamped by Client::Create / packed by the caller). This method only assigns LocalIndex +
        // Generation; identity validation is the caller's job. A template id (Generation == 0) gets the
        // lowest free slot; an allocated id re-activates its own slot (fails if busy). On exhaustion the
        // template is left intact so the rejection can still report which instrument the create was for.
        static bool TryAllocate(Tools::Bitset64& isOrderActive, OrderId& orderId); // defined below OrderId

        ALWAYS_INLINE static void ThrowIfInstrumentIdOutOfRange(int32_t instrumentId)
        {
            if (static_cast<uint32_t>(instrumentId) > static_cast<uint32_t>(MaxInstrumentId))
                ThrowInstrumentIdOutOfRange(instrumentId);
        }

        ALWAYS_INLINE static void ThrowIfClientIdOutOfRange(int32_t clientId)
        {
            if (static_cast<uint32_t>(clientId) > static_cast<uint32_t>(MaxClientId))
                ThrowClientIdOutOfRange(clientId);
        }

        ALWAYS_INLINE static void ThrowIfStrategyIdOutOfRange(int32_t strategyId)
        {
            if (static_cast<uint32_t>(strategyId) > static_cast<uint32_t>(MaxStrategyId))
                ThrowStrategyIdOutOfRange(strategyId);
        }

    private:

        [[noreturn]] NEVER_INLINE COLD static void ThrowInstrumentIdOutOfRange(int32_t instrumentId)
        {
            throw std::out_of_range(
                "OrderIdAllocator::ThrowIfInstrumentIdOutOfRange(" + std::to_string(instrumentId) +
                "), instrumentId should not be greater than: " + std::to_string(MaxInstrumentId));
        }

        [[noreturn]] NEVER_INLINE COLD static void ThrowClientIdOutOfRange(int32_t clientId)
        {
            throw std::out_of_range(
                "OrderIdAllocator::ThrowIfClientIdOutOfRange(" + std::to_string(clientId) +
                "), clientId should not be greater than: " + std::to_string(MaxClientId));
        }

        [[noreturn]] NEVER_INLINE COLD static void ThrowStrategyIdOutOfRange(int32_t strategyId)
        {
            throw std::out_of_range(
                "OrderIdAllocator::ThrowIfStrategyIdOutOfRange(" + std::to_string(strategyId) +
                "), strategyId should not be greater than: " + std::to_string(MaxStrategyId));
        }

    public:

        ALWAYS_INLINE static void Free(Tools::Bitset64& activeOrders, uint64_t orderId)
        {
            // Resolve Local Index directly from ID
            int32_t localIndex = GetLocalIndex(orderId);

            activeOrders.Clear(localIndex);
        }

        // --- Data Extraction ---

        ALWAYS_INLINE static int32_t GetFirstGlobalIndex(int32_t clientId)
        {
            return OrdersPerClient * clientId;
        }

        ALWAYS_INLINE static int32_t GetLastGlobalIndex(int32_t clientId)
        {
            return GetFirstGlobalIndex(clientId) + OrdersPerClient - 1;
        }

        ALWAYS_INLINE static int32_t GetGlobalIndex(uint64_t orderId)
        {
            return static_cast<int32_t>(orderId & s_indexMask);
        }

        ALWAYS_INLINE static int32_t GetStrategyId(uint64_t orderId)
        {
            return static_cast<int32_t>((orderId >> s_strategyShift) & s_strategyMask);
        }

        ALWAYS_INLINE static int32_t GetInstrumentId(uint64_t orderId)
        {
            return static_cast<int32_t>((orderId >> s_instrumentShift) & s_instrumentMask);
        }

        ALWAYS_INLINE static uint64_t GetGeneration(uint64_t orderId)
        {
            return orderId >> s_generationShift;
        }

        ALWAYS_INLINE static int32_t GetLocalIndex(uint64_t orderId)
        {
            return GetGlobalIndex(orderId) & (OrdersPerClient - 1);
        }

        ALWAYS_INLINE static int32_t GetClientId(uint64_t orderId)
        {
            return GetGlobalIndex(orderId) >> OrdersPerClientBitShift;
        }

        // --- Helpers ---

        ALWAYS_INLINE static int32_t ToGlobalIndex(int32_t clientId, int32_t localOrderIndex)
        {
            return (clientId * OrdersPerClient) + localOrderIndex;
        }
    };

#pragma pack(push, 1)
    // Typed 64-bit order id - identical wire layout to the raw uint64_t it replaces.
    // C#-property-style accessors: get = Field(), set = Field(value); setters return *this for chaining.
    // Masks/shifts are derived from OrderIdAllocator's *Bits constants (one source of truth).
    // Implicit conversions to/from uint64_t keep the raw-u64 ecosystem (comparisons, ordering,
    // dictionary keys, Free(u64)) compiling unchanged; ordering by raw value == allocation order,
    // because Generation occupies the top 32 bits.
    struct OrderId
    {
        uint64_t ClientOrderId = 0;

    private:
        static constexpr int32_t s_clientShift = OrderIdAllocator::LocalIndexBits;
        static constexpr int32_t s_strategyShift = s_clientShift + OrderIdAllocator::ClientBits;
        static constexpr int32_t s_instrumentShift = s_strategyShift + OrderIdAllocator::StrategyBits;
        static constexpr int32_t s_generationShift = s_instrumentShift + OrderIdAllocator::InstrumentBits;

        static constexpr uint64_t s_localIndexMask = (1ULL << OrderIdAllocator::LocalIndexBits) - 1ULL;
        static constexpr uint64_t s_clientMask = (1ULL << OrderIdAllocator::ClientBits) - 1ULL;
        static constexpr uint64_t s_strategyMask = (1ULL << OrderIdAllocator::StrategyBits) - 1ULL;
        static constexpr uint64_t s_instrumentMask = (1ULL << OrderIdAllocator::InstrumentBits) - 1ULL;
        static constexpr uint64_t s_generationMask = (1ULL << OrderIdAllocator::GenerationBits) - 1ULL;
        static constexpr uint64_t s_indexMask = (1ULL << OrderIdAllocator::IndexBits) - 1ULL;

    public:
        OrderId() = default;
        constexpr OrderId(uint64_t value) : ClientOrderId(value) {}
        constexpr operator uint64_t() const { return ClientOrderId; }

        // --- Getters ---

        ALWAYS_INLINE int32_t LocalIndex() const
        {
            return static_cast<int32_t>(ClientOrderId & s_localIndexMask);
        }

        ALWAYS_INLINE int32_t ClientId() const
        {
            return static_cast<int32_t>((ClientOrderId >> s_clientShift) & s_clientMask);
        }

        ALWAYS_INLINE int32_t StrategyId() const
        {
            return static_cast<int32_t>((ClientOrderId >> s_strategyShift) & s_strategyMask);
        }

        ALWAYS_INLINE int32_t InstrumentId() const
        {
            return static_cast<int32_t>((ClientOrderId >> s_instrumentShift) & s_instrumentMask);
        }

        ALWAYS_INLINE uint32_t Generation() const
        {
            return static_cast<uint32_t>(ClientOrderId >> s_generationShift);
        }

        // --- Setters (mask-out / mask-in over ClientOrderId; a value wider than its field wraps within the field only) ---

        ALWAYS_INLINE OrderId& LocalIndex(int32_t localIndex)
        {
            ClientOrderId = (ClientOrderId & ~s_localIndexMask) | (static_cast<uint64_t>(static_cast<uint32_t>(localIndex)) & s_localIndexMask);
            return *this;
        }

        ALWAYS_INLINE OrderId& ClientId(int32_t clientId)
        {
            ClientOrderId = (ClientOrderId & ~(s_clientMask << s_clientShift)) | ((static_cast<uint64_t>(static_cast<uint32_t>(clientId)) & s_clientMask) << s_clientShift);
            return *this;
        }

        ALWAYS_INLINE OrderId& StrategyId(int32_t strategyId)
        {
            ClientOrderId = (ClientOrderId & ~(s_strategyMask << s_strategyShift)) | ((static_cast<uint64_t>(static_cast<uint32_t>(strategyId)) & s_strategyMask) << s_strategyShift);
            return *this;
        }

        ALWAYS_INLINE OrderId& InstrumentId(int32_t instrumentId)
        {
            ClientOrderId = (ClientOrderId & ~(s_instrumentMask << s_instrumentShift)) | ((static_cast<uint64_t>(static_cast<uint32_t>(instrumentId)) & s_instrumentMask) << s_instrumentShift);
            return *this;
        }

        ALWAYS_INLINE OrderId& Generation(uint32_t generation)
        {
            ClientOrderId = (ClientOrderId & ~(s_generationMask << s_generationShift)) | (static_cast<uint64_t>(generation) << s_generationShift);
            return *this;
        }

        // --- Derived ---

        ALWAYS_INLINE int32_t GlobalIndex() const // clientId * OrdersPerClient + localIndex
        {
            return static_cast<int32_t>(ClientOrderId & s_indexMask);
        }

        ALWAYS_INLINE bool IsAllocated() const // Generation 0 is reserved as the template marker
        {
            return Generation() > 0;
        }

        ALWAYS_INLINE bool IsAlgoOrder() const // client trades its own strategy (clientId == strategyId)
        {
            return ClientId() == StrategyId();
        }

        std::string ToString() const
        {
            return Tools::Json::Serialize(*this);
        }

        // Reads/writes as a JSON object, like every other struct in the project. ClientOrderId is the source
        // of truth on read; the decoded fields are emitted for readability and ignored on read (no-op
        // setters). Lambda getters (not &T::Field) because the accessors are get/set-overloaded.
        struct glaze
        {
            using T = OrderId;
            static constexpr auto value = glz::object(
                "ClientOrderId", &T::ClientOrderId,
                "LocalIndex",   glz::custom<[](T&, int32_t) {},  [](const auto& o) { return o.LocalIndex(); }>,
                "ClientId",     glz::custom<[](T&, int32_t) {},  [](const auto& o) { return o.ClientId(); }>,
                "StrategyId",   glz::custom<[](T&, int32_t) {},  [](const auto& o) { return o.StrategyId(); }>,
                "InstrumentId", glz::custom<[](T&, int32_t) {},  [](const auto& o) { return o.InstrumentId(); }>,
                "Generation",   glz::custom<[](T&, uint32_t) {}, [](const auto& o) { return o.Generation(); }>,
                "GlobalIndex",  glz::custom<[](T&, int32_t) {},  [](const auto& o) { return o.GlobalIndex(); }>,
                "IsAllocated",  glz::custom<[](T&, bool) {},     [](const auto& o) { return o.IsAllocated(); }>
            );
        };
    };
#pragma pack(pop)

    static_assert(sizeof(OrderId) == 8, "OrderId must be 8 bytes");
    static_assert(Tools::PlainOldData<OrderId>, "OrderId must be unmanaged");

    ALWAYS_INLINE bool OrderIdAllocator::TryAllocate(Tools::Bitset64& isOrderActive, OrderId& orderId)
    {
        if (orderId.IsAllocated()) // reuse/recovery of a real id: re-activate its slot
        {
            int32_t localIndex = orderId.LocalIndex();
            if (isOrderActive[localIndex])
                return false;
            isOrderActive.Set(localIndex);
            s_generation = std::max(s_generation, static_cast<uint64_t>(orderId.Generation()) + uint64_t{1});
            return true;
        }
        else // template id: allocate a slot, keep the caller's ids
        {
            int32_t localIndex = isOrderActive.LowestClear();
            if (localIndex < 0)
                return false;

            isOrderActive.Set(localIndex);
            uint64_t currentGen = s_generation++;

            orderId.LocalIndex(localIndex);
            orderId.Generation(static_cast<uint32_t>(currentGen));
            return true;
        }
    }
}
