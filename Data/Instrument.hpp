#pragma once

#include "Json.hpp"
#include "String.hpp"
#include "Timestamp.hpp"
#include "SharedArray.hpp"
#include "MarketByPrice.hpp"
#include "Tick.hpp"
#include "Tools.hpp"
#include "Symbology.hpp"
#include <cstdint>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace Data
{

	#pragma pack(push, 1)
	template <typename T> requires Tools::ByteEnum<T>
	struct Header
	{
		T Type;
		uint8_t _reserved[3] = {0};

		Header(T type) : Type(type) {}
		Header() = default;

		struct glaze
		{
			static constexpr auto value = glz::object(
				"Type", &Header::Type
			);
		};
	};

	static_assert(sizeof(Header<InstrumentType>) == 4);

	// Coarse trading-session state, folded down from CME MDP 3.0 SecurityTradingStatus (tag 326).
	// JSON/glaze reflection is automatic via the generic glz::meta<T> enum specialization in Json.hpp
	// (serialized by name, e.g. "Open"), so no per-enum glaze block is required.
	enum class TradingStatus : uint8_t
	{
		Unknown = 0,   // uninitialized, CME UnknownorInvalid(20) / NoValue(255)
		Open,          // ReadyToTrade(17)
		Closed,        // Close(4), NotAvailableForTrading(18), PostClose(26)
		Auction,       // PreOpen(21), NewPriceIndication(15), PreCross(24), Cross(25)
		Halted,        // TradingHalt(2)
	};

	struct InstrumentHeader
	{
		Data::Header<Data::InstrumentType> Header = Data::Header<Data::InstrumentType>(Data::InstrumentType::Instrument);
		Data::InstrumentType InstrumentType;
		uint8_t CoreGroupId;
		Data::TradingStatus TradingStatus;
		uint8_t Reserved0[1] = {0};
		Tools::String8 Exchange;
		Tools::String8 Root;
		double TickSize;
		double InverseTickSize;
		double DisplayFactor;
		int32_t InstrumentHeaderId;
		int32_t InstrumentId;
		int32_t ExchangeInstrumentId;
		uint8_t Reserved1[4] = {0};
		std::string ToString() const
		{
			return Tools::Json::Serialize(this);
		}

		struct glaze
		{
			using T = InstrumentHeader;
			static constexpr auto value = glz::object(
				"InstrumentType", &T::InstrumentType,
				"CoreGroupId", &T::CoreGroupId,
				"Exchange", &T::Exchange,
				"Root", &T::Root,
				"InstrumentHeaderId", &T::InstrumentHeaderId,
				"InstrumentId", &T::InstrumentId,
				"TickSize", &T::TickSize,
				"InverseTickSize", &T::InverseTickSize
			);
		};
	};

	static_assert(sizeof(InstrumentHeader) == 64, "InstrumentHeader size must be 64 bytes");

	struct ForexHeader
	{
		Data::InstrumentHeader InstrumentHeader;
		Tools::String4 BaseCurrency;
		Tools::String4 QuoteCurrency;

		std::unique_ptr<Data::Symbology> Symbology() const
		{
			throw std::logic_error("Not implemented");
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(this);
		}

		struct glaze
		{
			using T = ForexHeader;
			static constexpr auto value = glz::object(
				"InstrumentHeader", &T::InstrumentHeader,
				"BaseCurrency", &T::BaseCurrency,
				"QuoteCurrency", &T::QuoteCurrency
			);
		};
	};

	struct FutureHeader
	{
		Data::InstrumentHeader InstrumentHeader;
		double Multiplier;
		Tools::Timestamp MaturityDate;
		Data::MaturityType MaturityType;

		std::unique_ptr<Data::FutureSymbology> Symbology() const
		{
			return std::make_unique<Data::FutureSymbology>(InstrumentHeader.Exchange.ToString(), InstrumentHeader.Root.ToString(), MaturityType, MaturityDate);
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(this);
		}

		struct glaze
		{
			using T = FutureHeader;
			static constexpr auto value = glz::object(
				"InstrumentHeader", &T::InstrumentHeader,
				"Multiplier", &T::Multiplier,
				"MaturityDate", &T::MaturityDate,
				"MaturityType", &T::MaturityType
			);
		};
	};

	struct SpreadHeader
	{
		Data::InstrumentHeader InstrumentHeader;
		double Multiplier;
		Tools::Timestamp LongMaturityDate;
		Tools::Timestamp ShortMaturityDate;
		int32_t LongInstrumentId;
		int32_t ShortInstrumentId;
		Data::MaturityType LongMaturityType;
		Data::MaturityType ShortMaturityType;


		std::unique_ptr<Data::SpreadSymbology> Symbology() const
		{
			return std::make_unique<Data::SpreadSymbology>(InstrumentHeader.Exchange.ToString(), InstrumentHeader.Root.ToString(), LongMaturityType, LongMaturityDate, ShortMaturityType, ShortMaturityDate);
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(this);
		}

		struct glaze
		{
			using T = SpreadHeader;
			static constexpr auto value = glz::object(
				"InstrumentHeader", &T::InstrumentHeader,
				"Multiplier", &T::Multiplier,
				"LongMaturityDate", &T::LongMaturityDate,
				"LongMaturityType", &T::LongMaturityType,
				"ShortMaturityDate", &T::ShortMaturityDate,
				"ShortMaturityType", &T::ShortMaturityType,
				"LongInstrumentId", &T::LongInstrumentId,
				"ShortInstrumentId", &T::ShortInstrumentId
			);
		};
	};

	struct InstrumentHeader128
	{
		InstrumentHeader128() : Raw{} 
		{
		}

		union {
			InstrumentHeader Base;
			ForexHeader Forex;
			FutureHeader Future;
			SpreadHeader Spread;
			uint8_t Raw[128];
		};

		InstrumentHeader& AsInstrumentHeader()
		{
			return Base;
		}

		const InstrumentHeader& AsInstrumentHeader() const
		{
			return Base;
		}
		
		FutureHeader& AsFuture()
		{
			return Future;
		}

		const FutureHeader& AsFuture() const
		{
			return Future;
		}

		ForexHeader& AsForex()
		{
			return Forex;
		}

		const ForexHeader& AsForex() const
		{
			return Forex;
		}

		SpreadHeader& AsSpread()
		{
			return Spread;
		}

		const SpreadHeader& AsSpread() const
		{
			return Spread;
		}

		std::unique_ptr<Data::Symbology> Symbology() const
		{
			switch (Base.InstrumentType)
			{
				case Data::InstrumentType::Future:
					return AsFuture().Symbology();
				case Data::InstrumentType::Forex:
					return AsForex().Symbology();
				case Data::InstrumentType::Spread:
					return AsSpread().Symbology();
				default:
					throw std::invalid_argument("Instrument type is not supported.");
			}
		}
	};

	static_assert(sizeof(InstrumentHeader128) == 128, "InstrumentHeader128 size must be 128 bytes");
	static_assert(Tools::PlainOldData<InstrumentHeader128>, "InstrumentHeader128 must be POD");

	#pragma pack(pop)

	class Instrument
	{
	protected:
		Socket::SharedArrayEntry<InstrumentHeader128> _headerEntry;
		Socket::SharedArrayEntry<MarketByPrice64> _mbpEntry;
        std::unique_ptr<Data::Symbology> _symbology;
        double _multiplier = 1.0;

		Instrument(int32_t id, Socket::SharedArrayEntry<InstrumentHeader128> headerEntry, Socket::SharedArrayEntry<MarketByPrice64> mbpEntry)
        : _headerEntry(headerEntry),
          _mbpEntry(mbpEntry),
          InstrumentId(id),
          TickDecimals(Tools::GetNumberOfDecimalPlaces(TickSize()))
		{
		}

	public:
		const int32_t InstrumentId;
		const int32_t TickDecimals;

		double Multiplier() const { return _multiplier; }
        
        Data::Symbology& Symbology() const { return *_symbology; }

		virtual ~Instrument() = default;

		const InstrumentHeader& Header() const
		{
			return _headerEntry.GetReadonlyRef().AsInstrumentHeader();
		}

		MarketByPrice64 MarketByPriceCopy()
		{
			return _mbpEntry.Read();
		}

		uint64_t MarketByPriceSeq()
		{
			return _mbpEntry.GetSeq();
		}

		const MarketByPrice64& MarketByPriceRef()
		{
			return _mbpEntry.GetReadonlyRef();
		}

		bool IsInSession()
		{
			return true;
		}

		double InverseTickSize() const
		{
			return Header().InverseTickSize;
		}

		double TickSize() const
		{
			return Header().TickSize;
		}

		std::string Symbol() const
		{
			return Symbology().Symbol();
		}

		std::string ShortSymbol() const
		{
			return Symbology().ShortSymbol();
		}

		std::string Exchange() const
		{
			return Symbology().Exchange();
		}

		std::string Root() const
		{
			return Symbology().Root();
		}

		double GetProfit(double buyPrice, double sellPrice, int32_t quantity) const
		{
			if (quantity == 0)
				return 0.0;
			
			return GetValue(sellPrice - buyPrice) * quantity;
		}

		double GetValue(double price) const
		{
			return price * Multiplier();
		}

		double TicksToPrice(int32_t ticks) const
		{
			return ticks * TickSize();
		}

		int32_t RoundToTicks(double price) const
		{
			return Tools::RoundToInt(price * InverseTickSize());
		}

		double RoundPrice(double price)
		{
			return RoundToTicks(price) * TickSize();
		}

		int32_t FloorToTicks(double price) const
		{
			return Tools::FloorToInt(price * InverseTickSize());
		}

		double FloorPrice(double price) const
		{
			return FloorToTicks(price) * TickSize();
		}

		int32_t CeilingToTicks(double price) const
		{
			return Tools::CeilingToInt(price * InverseTickSize());
		}

		double CeilingPrice(double price) const
		{
			return CeilingToTicks(price) * TickSize();
		}

		bool TryGetQuote(Quote& quote)
		{
			while (true)
			{
				uint64_t seq0 = _mbpEntry.GetSeq();
				
				if (Socket::Protocol::IsWriteInProgress(seq0))
				{
					_mm_pause();
					continue;
				}

				const MarketByPrice64& mbp = MarketByPriceRef();

                if (!IsInSession())
                {
                    quote = {};
                    return false;
                }

				if (mbp.BidsCount() == 0 || mbp.AsksCount() == 0)
				{
                    uint64_t seq1 = _mbpEntry.GetSeq();
                    if (seq0 == seq1)
                    {
                        quote = {};
                        return false;
                    }
					continue;
				}

				quote = Quote
				{
				   .TickSize = TickSize(),
				   .Bid = mbp.BestBid(),
				   .Ask = mbp.BestAsk(), 
				};

				uint64_t seq1 = _mbpEntry.GetSeq();
				
				if (seq0 == seq1)
					return true;
			}
		}

		std::string ToString() const
		{
			return "Instrument " + std::to_string(InstrumentId) + " " + Symbology().ToString();
		}
	};

	class Future : public Instrument
	{
	public:


		Future(int32_t id, Socket::SharedArrayEntry<Data::FutureHeader> headerEntry, Socket::SharedArrayEntry<MarketByPrice64> mbpEntry) : Instrument(id, headerEntry.Cast<InstrumentHeader128>(), mbpEntry)
		{
			_symbology = FutureHeader().Symbology();
			_multiplier = FutureHeader().Multiplier;
		}

		Data::MaturityType MaturityType() const
		{
			return FutureHeader().MaturityType;
		}

		Tools::Timestamp MaturityDate() const
		{
			return FutureHeader().MaturityDate;
		}

		const Data::FutureHeader& FutureHeader() const
		{
			return _headerEntry.GetReadonlyRef().AsFuture();
		}
	};

	class Forex final : public Instrument
	{
	public:
		Forex(int32_t id, Socket::SharedArrayEntry<Data::ForexHeader> headerEntry, Socket::SharedArrayEntry<MarketByPrice64> mbpEntry) : Instrument(id, headerEntry.Cast<InstrumentHeader128>(), mbpEntry)
		{
			_symbology = ForexHeader().Symbology();
			_multiplier = 1.0;
		}

		Tools::String4 BaseCurrency() const
		{
			return ForexHeader().BaseCurrency;
		}

		Tools::String4 QuoteCurrency() const
		{
			return ForexHeader().QuoteCurrency;
		}

		const Data::ForexHeader& ForexHeader() const
		{
			return _headerEntry.GetReadonlyRef().AsForex();
		}
	};

	class Spread final : public Future
	{
	private:
		const Future& _long;
		const Future& _short;

	public:
		Spread(int32_t id, Socket::SharedArrayEntry<Data::SpreadHeader> headerEntry, Socket::SharedArrayEntry<MarketByPrice64> mbpEntry, const Future& longFuture, const Future& shortFuture) : Future(id, headerEntry.Cast<Data::FutureHeader>(), mbpEntry), _long(longFuture), _short(shortFuture)
		{
			_symbology = SpreadHeader().Symbology();
			_multiplier = SpreadHeader().Multiplier;
		}

		Data::MaturityType LongMaturityType() const
		{
			return _long.MaturityType();
		}

		Tools::Timestamp LongMaturityDate() const
		{
			return _long.MaturityDate();
		}

		Data::MaturityType ShortMaturityType() const
		{
			return _short.MaturityType();
		}

		Tools::Timestamp ShortMaturityDate() const
		{
			return _short.MaturityDate();
		}

		const Future& Long() const
		{
			return _long;
		}

		const Future& Short() const
		{
			return _short;
		}

		const Data::SpreadHeader& SpreadHeader() const
		{
			return _headerEntry.GetReadonlyRef().AsSpread();
		}
	};
}