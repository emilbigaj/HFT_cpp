#pragma once

#include <cstdint>
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
		AlgoStatus = 200
	};
	
#pragma pack(push, 1)
	struct AllocateClient final
	{
		Data::Header<AllocateType> Header = Data::Header<AllocateType>(AllocateType::Client);
		int32_t ClientId = 0;
		Tools::String128 ClientName;
	};
	static_assert(Tools::PlainOldData<AllocateClient>);



	struct AllocateInstrument final
	{
		Data::Header<AllocateType> Header = Data::Header<AllocateType>(AllocateType::Instrument);
		int32_t ClientId = -1;
		int32_t InstrumentHeaderId = -1;
		int32_t InstrumentId = -1;
		Tools::String64 Symbol;
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

#pragma pack(pop)
}