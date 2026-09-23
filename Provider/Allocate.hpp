#pragma once

#include <cstdint>
#include "../Tools/Json.hpp"
#include "../Tools/String.hpp"
#include "Bitset.hpp"
#include "Tools.hpp"
#include "Order.hpp"
#include "Timestamp.hpp"

namespace Provider
{
	enum class AllocateType : uint8_t
	{
		Client = 100,
		Instrument = 101,
	};

	enum class ControlType : uint8_t
	{
		AlgoStatus = 200,
		RiskLimit = 201,
	};
}

// ControlType's values (200, 201) sit outside magic_enum's default [-128, 128) reflection range;
// without this the glaze enum meta sees an empty enumerator list and serialization fails to compile.
template <>
struct magic_enum::customize::enum_range<Provider::ControlType>
{
	static constexpr int min = 0;
	static constexpr int max = 255;
};

namespace Provider
{
	
#pragma pack(push, 1)
	struct AllocateClient final
	{
		Data::Header<AllocateType> Header = Data::Header<AllocateType>(AllocateType::Client);
		int32_t ClientId = 0;
		Tools::String128 ClientName;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = AllocateClient;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"ClientId", &T::ClientId,
				"ClientName", &T::ClientName
			);
		};
	};
	static_assert(Tools::PlainOldData<AllocateClient>);



	struct AllocateInstrument final
	{
		Data::Header<AllocateType> Header = Data::Header<AllocateType>(AllocateType::Instrument);
		int32_t ClientId = -1;
		int32_t InstrumentHeaderId = -1;
		int32_t InstrumentId = -1;
		int32_t ExchangeInstrumentId = -1;
		Tools::String64 Symbol;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = AllocateInstrument;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"ClientId", &T::ClientId,
				"InstrumentHeaderId", &T::InstrumentHeaderId,
				"InstrumentId", &T::InstrumentId,
				"ExchangeInstrumentId", &T::ExchangeInstrumentId,
				"Symbol", &T::Symbol
			);
		};
	};
	static_assert(Tools::PlainOldData<AllocateInstrument>);

	struct ControlAlgoStatus final
	{
		Data::Header<ControlType> Header = Data::Header<ControlType>(ControlType::AlgoStatus);
		int32_t ClientId = -1;
		int32_t StrategyId = -1;
		int32_t InstrumentId = -1;
		Execution::AlgoStatus AlgoStatus = Execution::AlgoStatus::Paused;
	};
	static_assert(Tools::PlainOldData<ControlAlgoStatus>);
	static_assert(sizeof(ControlAlgoStatus) == 17, "ControlAlgoStatus must be 17 bytes");

	// Operator request to change an instrument's risk limits. Sent on the instrument's CoreGroup
	// EXECUTION channel (not admin), so the CoreGroup thread - the RiskLimit row's sole writer -
	// applies it. A client never sends a RiskLimit row; the request carries only the two maxima.
	struct ControlRiskLimit final
	{
		Data::Header<ControlType> Header = Data::Header<ControlType>(ControlType::RiskLimit);
		int32_t ClientId = -1;
		int32_t InstrumentId = -1;
		int32_t MaxOrderQuantity = 0;
		int32_t MaxPositionQuantity = 0;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = ControlRiskLimit;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"ClientId", &T::ClientId,
				"InstrumentId", &T::InstrumentId,
				"MaxOrderQuantity", &T::MaxOrderQuantity,
				"MaxPositionQuantity", &T::MaxPositionQuantity
			);
		};
	};
	static_assert(Tools::PlainOldData<ControlRiskLimit>);
	static_assert(sizeof(ControlRiskLimit) == 20, "ControlRiskLimit must be 20 bytes");

	// One row per CoreGroup in the server-written CoreGroups shared array (index == CoreGroupId):
	// the group's name and the cores its threads pin to. Loaded from <server>/CoreGroups/
	// <name>.coregroup - one static whole-file JSON each, NOT the appended-line form of .risklimit:
	// neither a CoreGroup nor a rate limit is amended at runtime.
	struct CoreGroup final
	{
		Tools::String16 CoreGroupName;    //  0, 16
		int32_t CoreGroupId = -1;         // 16, 4
		int32_t ServerCoreId = -1;        // 20, 4
		int32_t MarketDataCoreId = -1;    // 24, 4
		int32_t StrategyCoreId = -1;      // 28, 4
		int32_t ReservedCoreId = -1;      // 32, 4   could be a second server, strategy or market data core - depends on circumstances.

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = CoreGroup;
			static constexpr auto value = glz::object(
				"CoreGroupName", &T::CoreGroupName,
				"CoreGroupId", &T::CoreGroupId,
				"ServerCoreId", &T::ServerCoreId,
				"MarketDataCoreId", &T::MarketDataCoreId,
				"StrategyCoreId", &T::StrategyCoreId,
				"ReservedCoreId", &T::ReservedCoreId
			);
		};
	};
	static_assert(Tools::PlainOldData<CoreGroup>);
	static_assert(sizeof(CoreGroup) == 36, "CoreGroup must be 36 bytes");
	static_assert(offsetof(CoreGroup, CoreGroupId) == 16);
	static_assert(offsetof(CoreGroup, ReservedCoreId) == 32);

	struct ServerHeader final
	{
		Tools::String128 ServerName;
		Tools::Timestamp Timestamp;
		int32_t InstrumentsCapacity = 4096;
		int32_t InstrumentsCount = 0;

		Tools::Bitset64 InstrumentIds = Tools::Bitset64();
		Tools::Bitset64 ClientIds = Tools::Bitset64();
		// Which CoreGroups (trading segments) exist => the per-socket channel layout. Channel index
		// == CoreGroupId: bit 0 = admin, bits 1..7 = execution. The client reads this at connect to
		// size its channels. WIRE FIELD — the C# side must mirror it byte-for-byte.
		Tools::Bitset64 CoreGroupIds = Tools::Bitset64();

		int32_t OrdersPerClient = 64;

		// Client sockets outlive their client process: a dropped client goes Detached instead of
		// being disposed, so the server keeps writing into its ring and the audit tap keeps reading.
		// WIRE FIELD — the C# side must mirror it byte-for-byte.
		bool Persistance = true;

		int32_t OrdersCapacity() const
		{
			return OrdersPerClient * ClientIds.Length();
		}

		int32_t LocalPositionsCapacity() const
		{
			return InstrumentIds.Length() * ClientIds.Length();
		}
	};
	static_assert(Tools::PlainOldData<ServerHeader>);
	// Persistance is the newest wire field; C# asserts the same two numbers at type-init.
	static_assert(sizeof(ServerHeader) == 173 && offsetof(ServerHeader, Persistance) == 172);
	static_assert(offsetof(AllocateInstrument, ClientId) == 4
		&& offsetof(AllocateInstrument, InstrumentHeaderId) == 8
		&& offsetof(AllocateInstrument, InstrumentId) == 12
		&& offsetof(AllocateInstrument, ExchangeInstrumentId) == 16
		&& offsetof(AllocateInstrument, Symbol) == 20);
	static_assert(sizeof(Data::Header<AllocateType>) == 4);

#pragma pack(pop)
}