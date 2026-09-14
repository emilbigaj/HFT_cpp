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

	// TradingStatus lives in Tick.hpp now (it rides the instrument data ring as a tick).

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
	// TradingStatus is the runtime-updated byte at offset 6 (Header 4 | InstrumentType | CoreGroupId),
	// verified against C# field order - the 09-10 report's "offset 7" counts from 1. The reserved
	// byte at 7 is where HaltReason goes when it is carried, with no size change.
	static_assert(offsetof(InstrumentHeader, TradingStatus) == 6);

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
		// MaturityType deleted (was the tail byte after MaturityDate; no other offsets moved):
		// the date alone identifies a contract, and bare-ISO-date symbols sort chronologically.

		std::unique_ptr<Data::FutureSymbology> Symbology() const
		{
			return std::make_unique<Data::FutureSymbology>(InstrumentHeader.Exchange.ToString(), InstrumentHeader.Root.ToString(), MaturityDate);
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
				"MaturityDate", &T::MaturityDate
			);
		};
	};

	struct InstrumentHeader128;

	struct LegHeader
	{
		int32_t InstrumentHeaderId = -1; // sibling header id, NOT instrument id
		int32_t Weight = 0;              // signed: calendar = +1 front, -1 back

		struct glaze
		{
			using T = LegHeader;
			static constexpr auto value = glz::object(
				"InstrumentHeaderId", &T::InstrumentHeaderId,
				"Weight", &T::Weight
			);
		};
	};
	static_assert(sizeof(LegHeader) == 8, "LegHeader must be 8 bytes");

	struct LeggedHeader
	{
		// 128-byte overlay budget: InstrumentHeader 64 + Multiplier 8 + LegCount 4 + reserved 4 + 6*8 legs = 124.
		Data::InstrumentHeader InstrumentHeader;
		double Multiplier;   // VESTIGIAL: spreads have no multiplier (legs carry them);
		                     // field kept for byte parity until a coordinated layout trim
		int32_t LegCount;
		uint8_t Reserved[4];
		LegHeader Leg0, Leg1, Leg2, Leg3, Leg4, Leg5;

		// Live legs, maturity-ascending - same invariant as the ticker. Clamped: LegCount comes
		// from shared memory and a span constructor does no validation.
		std::span<const LegHeader> Legs() const
		{
			return { &Leg0, static_cast<size_t>(std::clamp(LegCount, 0, 6)) };
		}

		// Legs reference sibling headers by id; the context hooks this, like InstrumentDetails.GetLeg.
		static inline std::function<InstrumentHeader128(int32_t)> GetLegHeader;

		std::unique_ptr<Data::SpreadSymbology> Symbology() const;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = LeggedHeader;
			static constexpr auto value = glz::object(
				"InstrumentHeader", &T::InstrumentHeader,
				"Multiplier", &T::Multiplier,
				"LegCount", &T::LegCount,
				"Leg0", &T::Leg0,
				"Leg1", &T::Leg1,
				"Leg2", &T::Leg2,
				"Leg3", &T::Leg3,
				"Leg4", &T::Leg4,
				"Leg5", &T::Leg5
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
			LeggedHeader Legged;
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
		
		// Overlay accessors are TYPE-GUARDED: a realtime context holds spreads and unfilled slots
		// too, and reading the wrong overlay is silent garbage, not an error, without the check.
		FutureHeader& AsFuture()
		{
			ThrowIfNot(Data::InstrumentType::Future);
			return Future;
		}

		const FutureHeader& AsFuture() const
		{
			ThrowIfNot(Data::InstrumentType::Future);
			return Future;
		}

		ForexHeader& AsForex()
		{
			ThrowIfNot(Data::InstrumentType::Forex);
			return Forex;
		}

		const ForexHeader& AsForex() const
		{
			ThrowIfNot(Data::InstrumentType::Forex);
			return Forex;
		}

		LeggedHeader& AsLegged()
		{
			ThrowIfNot(Data::InstrumentType::Spread);
			return Legged;
		}

		const LeggedHeader& AsLegged() const
		{
			ThrowIfNot(Data::InstrumentType::Spread);
			return Legged;
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
					return AsLegged().Symbology();
				default:
					throw std::invalid_argument("Instrument type is not supported.");
			}
		}

	private:
		void ThrowIfNot(Data::InstrumentType instrumentType) const
		{
			if (Base.InstrumentType != instrumentType)
				throw std::logic_error("InstrumentHeader128: overlay read as the wrong instrument type.");
		}
	};

	static_assert(sizeof(InstrumentHeader128) == 128, "InstrumentHeader128 size must be 128 bytes");
	static_assert(Tools::PlainOldData<InstrumentHeader128>, "InstrumentHeader128 must be POD");
	static_assert(sizeof(LeggedHeader) <= sizeof(InstrumentHeader128), "LeggedHeader must fit within InstrumentHeader128");

	// Symbology built from the legs (maturity-ascending, signed-weight tokens), resolved through
	// the GetLegHeader hook the context installs.
	inline std::unique_ptr<Data::SpreadSymbology> LeggedHeader::Symbology() const
	{
		if (!GetLegHeader)
			throw std::logic_error("LeggedHeader::Symbology: GetLegHeader hook is not installed.");

		std::vector<std::unique_ptr<Data::Symbology>> symbologies;
		std::vector<int32_t> weights;
		for (const LegHeader& legHeader : Legs())
		{
			symbologies.push_back(GetLegHeader(legHeader.InstrumentHeaderId).Symbology());
			weights.push_back(legHeader.Weight);
		}
		return std::make_unique<Data::SpreadSymbology>(InstrumentHeader.Exchange.ToString(), InstrumentHeader.Root.ToString(), std::move(symbologies), std::move(weights));
	}

	#pragma pack(pop)

	// A resolved leg for risk decomposition: instrument id + signed weight. 8 bytes, so a full
	// 6-leg view fits in one cache line; ids not Instrument refs - the risk loops only need the id.
	struct InstrumentLeg
	{
		int32_t InstrumentId = -1;
		int32_t Weight = 0;
	};

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
			_legs = { InstrumentLeg{ id, 1 } };
			_quote.TickSize = TickSize();
		}

		// Legs view for risk decomposition: an outright is its own single leg (weight +1); a
		// Spread overwrites with its resolved legs. Frozen at construction - header identity is
		// immutable, same invariant as the symbology.
		std::vector<InstrumentLeg> _legs;

	public:
		std::span<const InstrumentLeg> Legs() const { return _legs; }
		// > 1: every instrument is its own single leg (base ctor); legged means legs BEYOND itself.
		bool IsLegged() const { return _legs.size() > 1; }

		// Raised on TRANSITIONS only. The duplicate filter compares against this private mirror,
		// NOT the header row: the server writes the row before the tick reaches the ring, so a
		// row-based guard never fires. Header().TradingStatus stays the any-time read.
		std::function<void(const Data::TradingStatusUpdate&)> TradingStatusUpdateEvent;

		// --- One strategy run per ReadSocket pass (see Spec.md / 2026-09-14 report) ---
		// Phase 2 events: QuoteChanged fires on a NET change against the START of the pass (a
		// quote that moves and moves back within one pass is not a change); MarketByPriceChanged
		// fires once per pass for any touched book. Per-delta consumers stay in phase 1
		// (Client::MarketByPrice - the C++ rendering of C#'s Instrument.MarketByPriceDelta).
		std::function<void()> QuoteChanged;
		std::function<void()> MarketByPriceChanged;

		// Phase 1, per folded delta: refresh the quote cache from the book image, remembering the
		// quote as it was when this pass FIRST touched the book.
		void ApplyMarketByPriceDelta(const Data::MarketByPrice64& mbp64)
		{
			if (!_isDirty)
			{
				_quoteAtPassStart = _quote;
				_isDirty = true;
			}
			_quote.Bid = mbp64.BidsCount() > 0 ? mbp64.BestBid() : Level{};
			_quote.Ask = mbp64.AsksCount() > 0 ? mbp64.BestAsk() : Level{};
			_isQuoteValid = mbp64.BidsCount() > 0 && mbp64.AsksCount() > 0;
		}

		// Phase 2, once per pass.
		void RaiseChanged()
		{
			_isDirty = false;
			bool quoteChanged = _quote.Bid.Ticks != _quoteAtPassStart.Bid.Ticks
				|| _quote.Bid.Quantity != _quoteAtPassStart.Bid.Quantity
				|| _quote.Ask.Ticks != _quoteAtPassStart.Ask.Ticks
				|| _quote.Ask.Quantity != _quoteAtPassStart.Ask.Quantity;
			if (quoteChanged && QuoteChanged)
				QuoteChanged();
			if (MarketByPriceChanged)
				MarketByPriceChanged();
		}

		void OnTradingStatusUpdate(const Data::TradingStatusUpdate& tradingStatusUpdate)
		{
			if (_tradingStatus != tradingStatusUpdate.TradingStatus)
			{
				_tradingStatus = tradingStatusUpdate.TradingStatus;
				if (TradingStatusUpdateEvent)
					TradingStatusUpdateEvent(tradingStatusUpdate);
			}
		}

	private:
		Data::TradingStatus _tradingStatus = Data::TradingStatus::Unknown;
		Data::Quote _quote = {};
		Data::Quote _quoteAtPassStart = {};
		bool _isDirty = false;
		bool _isQuoteValid = false;

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

	// A spread is imaginary: economically the position IS its outright legs, so risk, positions
	// and P&L live on the legs - the spread keeps only its book, its order flow, and a volume
	// row. NOT a Future: no multiplier of its own (legs carry them), no maturity of its own.
	class Spread final : public Instrument
	{
	private:
		const Future& _long;
		const Future& _short;

	public:
		Spread(int32_t id, Socket::SharedArrayEntry<Data::LeggedHeader> headerEntry, Socket::SharedArrayEntry<MarketByPrice64> mbpEntry, const Future& longFuture, const Future& shortFuture) : Instrument(id, headerEntry.Cast<Data::InstrumentHeader128>(), mbpEntry), _long(longFuture), _short(shortFuture)
		{
			_symbology = Legged().Symbology();
			// Current runtime scope is 2-leg ±1 calendars; wider weights are out of scope and
			// must fail loudly at construction, not misprice risk quietly.
			for (const Data::LegHeader& legHeader : Legged().Legs())
				if (legHeader.Weight != 1 && legHeader.Weight != -1)
					throw std::invalid_argument("Spread: only ±1 leg weights are supported.");
			_legs = { Data::InstrumentLeg{ longFuture.InstrumentId, 1 }, Data::InstrumentLeg{ shortFuture.InstrumentId, -1 } };
		}

		Tools::Timestamp LongMaturityDate() const
		{
			return _long.MaturityDate();
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

		const Data::LeggedHeader& Legged() const
		{
			return _headerEntry.GetReadonlyRef().AsLegged();
		}
	};
}