//BEGIN_FILE HFT/Data/Tick.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <span>
#include <sstream>
#include <cstring>
#include <iostream>

#include "Json.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"

namespace Data
{
	enum class Side : int8_t
	{
		Flat = 0,
		Buy = 1,
		Sell = -1,
	};
	enum class TickType : int8_t
	{
        Trade = 0,
        Quote = 1,
        MarketByPrice = 2,
        MarketByPriceSnapshot = 3,
        MarketByPriceUpdate = 4,
        MarketByPricePartialUpdate = 5,
        MarketByPriceDelta = 6,
        Settlement = 7,
	};
#pragma pack(push, 1)

	struct Level
	{
		int32_t Ticks;
		int32_t Quantity;

		std::string ToString() const
		{
			std::stringstream ss;
			ss << "(" << Ticks << ", " << Quantity << ")";
			return ss.str();
		}
		
        struct glaze
        {
            using T = Level;
            static constexpr auto value = glz::object(
                "Ticks", &T::Ticks,
                "Quantity", &T::Quantity
            );
		};
    };

    struct TickHeader
	{
		Data::TickType TickType;
		int8_t Reserved[3] = {0, 0, 0};
		int32_t InstrumentId;
		Tools::Timestamp ExchangeTimestamp;
		Tools::Timestamp SendingTimestamp;
		Tools::Timestamp NicTimestamp;
		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}
		
		struct glaze
        {
            using T = TickHeader;
            static constexpr auto value = glz::object(
                "TickType", &T::TickType,
                "InstrumentId", &T::InstrumentId,
                "ExchangeTimestamp", &T::ExchangeTimestamp,
				"SendingTimestamp", &T::SendingTimestamp,
                "NicTimestamp", &T::NicTimestamp
            );
		};
	};


	struct Quote
	{
		double TickSize;
		Level Bid;
		Level Ask;

		double MidPrice() const
		{
			return (Bid.Ticks + Ask.Ticks) * 0.5 * TickSize;
		}

		double BidPrice() const
		{
			return Bid.Ticks * TickSize;
		}

		double AskPrice() const
		{
			return Ask.Ticks * TickSize;
		}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}
		
        struct glaze
        {
            using T = Quote;
            static constexpr auto value = glz::object(
                "TickSize", &T::TickSize,
                "Bid", &T::Bid,
                "Ask", &T::Ask
            );
		};
	};

	struct Trade
	{
		Data::TickHeader TickHeader;
		Data::Level Level;
		int8_t Direction;
        uint8_t _reserved[64 - sizeof(Data::TickHeader) - sizeof(Data::Level) - sizeof(int8_t)] = {};

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}
		
		struct glaze
        {
            using T = Trade;
            static constexpr auto value = glz::object(
                "TickHeader", &T::TickHeader,
                "Level", &T::Level,
                "Direction", &T::Direction
            );
		};
	};
	
	static_assert(Tools::PlainOldData<Trade>);
	
	/// <summary>   
	/// Market-by-Price wire message: header + counts + trailing Level arrays (bids then asks).
	/// Layout:
	/// [ TickHeader | BidsCount:int32_t | AsksCount:int32_t | bids[0..BidsCount-1] | asks[0..AsksCount-1] ]
	/// </summary>
	struct MarketByPrice
	{
		Data::TickHeader TickHeader;
		int32_t BidsCount;
		int32_t AsksCount;
		std::string ToString() const
		{
            return Tools::Json::Serialize(*this);
		}
		
        

        struct glaze
        {
            static std::span<const Level> GetBidsSpan(const MarketByPrice& mbp)
            {
                return std::span<const Level>(GetBidsPtr(&mbp), static_cast<uint32_t>(mbp.BidsCount));
            }

            static std::span<const Level> GetAsksSpan(const MarketByPrice& mbp)
            {
                return std::span<const Level>(GetAsksPtr(&mbp), static_cast<uint32_t>(mbp.AsksCount));
            }

            using T = MarketByPrice;
            static constexpr auto value = glz::object(
                "TickHeader", &T::TickHeader,
                "BidsCount", &T::BidsCount,
                "AsksCount", &T::AsksCount,
                "Bids", [](const T& mbp) { return GetBidsSpan(mbp); },
                "Asks", [](const T& mbp) { return GetAsksSpan(mbp); }
            );
	    };
// ----- Pointer helpers (unsafe equivalence) -----

		static Level* GetBidsPtr(MarketByPrice* mbpPtr)
		{
			return reinterpret_cast<Level*>(reinterpret_cast<int8_t*>(mbpPtr) + sizeof(MarketByPrice));
		}

		static const Level* GetBidsPtr(const MarketByPrice* mbpPtr)
		{
			return reinterpret_cast<const Level*>(reinterpret_cast<const int8_t*>(mbpPtr) + sizeof(MarketByPrice));
		}

		static Level* GetAsksPtr(MarketByPrice* mbpPtr)
		{
			return GetBidsPtr(mbpPtr) + mbpPtr->BidsCount;
		}

		static const Level* GetAsksPtr(const MarketByPrice* mbpPtr)
		{
			return GetBidsPtr(mbpPtr) + mbpPtr->BidsCount;
		}

		std::string BidsAsString() const
		{
			const Level* bidsPtr = GetBidsPtr(this);
			std::span<const Level> bids(bidsPtr, static_cast<uint32_t>(BidsCount));
			return ToString(bids);
		}

		std::string AsksAsString() const
		{
			const Level* asksPtr = GetAsksPtr(this);
			std::span<const Level> asks(asksPtr, static_cast<uint32_t>(AsksCount));
			return ToString(asks);
		}

		static std::string ToString(std::span<const Level> levels)
		{
			std::stringstream sb;
			sb << "[";
			
			int32_t count = static_cast<int32_t>(levels.size());
			for (int32_t i = 0; i < count; i++)
			{
				if (i > 0) 
					sb << ", ";
					
				sb << levels[static_cast<uint32_t>(i)].ToString();
			}
			
			sb << "]";
			return sb.str();
		}

		// ----- Size helpers -----

		int32_t SizeOf() const
		{
			return SizeOf(BidsCount, AsksCount);
		}

		int32_t SizeOfLevels() const
		{
			return (BidsCount + AsksCount) * static_cast<int32_t>(sizeof(Level));
		}

		static constexpr int32_t SizeOf(int32_t bidsCount, int32_t asksCount)
		{
			return static_cast<int32_t>(sizeof(MarketByPrice) + static_cast<size_t>(bidsCount + asksCount) * sizeof(Level));
		}

		// ===== Instance Span-based accessors =====

		/// <summary>Span over bids stored immediately after the header.</summary>
		std::span<Level> BidsAsSpan(std::span<uint8_t> src)
		{
			int32_t headerBytes = static_cast<int32_t>(sizeof(MarketByPrice));
			return std::span<Level>(reinterpret_cast<Level*>(src.data() + headerBytes), static_cast<uint32_t>(BidsCount));
		}

		std::span<const Level> BidsAsSpan(std::span<const uint8_t> src) const
		{
			int32_t headerBytes = static_cast<int32_t>(sizeof(MarketByPrice));
			return std::span<const Level>(reinterpret_cast<const Level*>(src.data() + headerBytes), static_cast<uint32_t>(BidsCount));
		}

		/// <summary>Span over asks stored after the bids block.</summary>
		std::span<Level> AsksAsSpan(std::span<uint8_t> src)
		{
			int32_t headerBytes = static_cast<int32_t>(sizeof(MarketByPrice));
			int32_t levelBytes = static_cast<int32_t>(sizeof(Level));
			int32_t bidsBytes = BidsCount * levelBytes;
			
			return std::span<Level>(reinterpret_cast<Level*>(src.data() + headerBytes + bidsBytes), static_cast<uint32_t>(AsksCount));
		}

		std::span<const Level> AsksAsSpan(std::span<const uint8_t> src) const
		{
			int32_t headerBytes = static_cast<int32_t>(sizeof(MarketByPrice));
			int32_t levelBytes = static_cast<int32_t>(sizeof(Level));
			int32_t bidsBytes = BidsCount * levelBytes;
			return std::span<const Level>(reinterpret_cast<const Level*>(src.data() + headerBytes + bidsBytes), static_cast<uint32_t>(AsksCount));
		}


		static MarketByPrice& SnapshotAsUpdate(std::span<const uint8_t> past, std::span<const uint8_t> future, std::span<uint8_t>& dst)
		{
			const MarketByPrice& pastMbp = *reinterpret_cast<const MarketByPrice*>(past.data());
			const MarketByPrice& futureMbp = *reinterpret_cast<const MarketByPrice*>(future.data());

			int32_t maxBidChanges = pastMbp.BidsCount + futureMbp.BidsCount;
			int32_t maxAskChanges = pastMbp.AsksCount + futureMbp.AsksCount;
			int32_t maxSize = SizeOf(maxBidChanges, maxAskChanges);
			
			if (static_cast<int32_t>(dst.size()) < maxSize)
				throw std::invalid_argument("Destination span too small");

			MarketByPrice& updateMbp = *reinterpret_cast<MarketByPrice*>(dst.data());
			updateMbp = MarketByPrice
			{
				.TickHeader = Data::TickHeader
                {
					.TickType = TickType::MarketByPriceUpdate,
					.InstrumentId = futureMbp.TickHeader.InstrumentId,
					.ExchangeTimestamp = futureMbp.TickHeader.ExchangeTimestamp,
					.SendingTimestamp = futureMbp.TickHeader.SendingTimestamp,
					.NicTimestamp = futureMbp.TickHeader.NicTimestamp
				},
				.BidsCount = 0,
				.AsksCount = 0
			};

			std::span<const Level> pastBids = pastMbp.BidsAsSpan(past);
			std::span<const Level> pastAsks = pastMbp.AsksAsSpan(past);
			std::span<const Level> futureBids = futureMbp.BidsAsSpan(future);
			std::span<const Level> futureAsks = futureMbp.AsksAsSpan(future);

			updateMbp.BidsCount = maxBidChanges;
			std::span<Level> dstBids = updateMbp.BidsAsSpan(dst);
			
			int32_t bidsWritten = DiffBids(pastBids, futureBids, dstBids);
			updateMbp.BidsCount = bidsWritten;

			updateMbp.AsksCount = maxAskChanges;
			std::span<Level> dstAsks = updateMbp.AsksAsSpan(dst);

			int32_t asksWritten = DiffAsks(pastAsks, futureAsks, dstAsks);
			updateMbp.AsksCount = asksWritten;

            dst = dst.subspan(0, static_cast<size_t>(updateMbp.SizeOf()));
			return updateMbp;
		}

	private:
		template <bool IsAsks>
		static int32_t Diff(std::span<const Level> past, std::span<const Level> future, std::span<Level> update)
		{
			uint32_t iPast = 0;
			uint32_t iFuture = 0;
			uint32_t iUpdate = 0;

			uint32_t pastLen = static_cast<uint32_t>(past.size());
			uint32_t futureLen = static_cast<uint32_t>(future.size());

			while (iPast < pastLen && iFuture < futureLen)
			{
				int32_t pastTicks = past[iPast].Ticks;
				int32_t futureTicks = future[iFuture].Ticks;

				if (pastTicks == futureTicks)
				{
					int32_t pastQty = past[iPast].Quantity;
					int32_t futureQty = future[iFuture].Quantity;
					
					if (pastQty != futureQty)
						update[iUpdate++] = future[iFuture];
						
					iPast++;
					iFuture++;
				}
				else
				{
					bool isPastMissingInFuture = false;
					
					if constexpr (IsAsks)
						isPastMissingInFuture = futureTicks > pastTicks;
					else
						isPastMissingInFuture = futureTicks < pastTicks;
						
					if (isPastMissingInFuture)
					{
						update[iUpdate++] = Level { .Ticks = pastTicks, .Quantity = 0 };
						iPast++;
					}
					else
					{
						update[iUpdate++] = future[iFuture];
						iFuture++;
					}
				}
			}

			while (iFuture < futureLen)
			{
				update[iUpdate++] = future[iFuture];
				iFuture++;
			}

			while (iPast < pastLen)
			{
				int32_t removedTicks = past[iPast].Ticks;
				update[iUpdate++] = Level { .Ticks = removedTicks, .Quantity = 0 };
				iPast++;
			}

			return static_cast<int32_t>(iUpdate);
		}

		static int32_t DiffBids(std::span<const Level> pastBids, std::span<const Level> futureBids, std::span<Level> dst)
		{
			return Diff<false>(pastBids, futureBids, dst);
		}

		static int32_t DiffAsks(std::span<const Level> pastAsks, std::span<const Level> futureAsks, std::span<Level> dst)
		{
			return Diff<true>(pastAsks, futureAsks, dst);
		}
	};
#pragma pack(pop)
}

//END_FILE HFT/Data/Tick.hpp