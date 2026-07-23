#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include <bit>
#include <span>
#include <cstring>
#include <iostream>
#include <vector>

#include "Bitset.hpp"
#include "Timestamp.hpp"
#include "Tick.hpp" 

namespace Data
{
	// ─────────────────────────────────────────────────────────────────────────
	// SideByPrice64
	// ─────────────────────────────────────────────────────────────────────────
	#pragma pack(push, 1)
	struct SideByPrice64
	{
		struct Enumerator;
		friend struct MarketByPrice64;

	public:
		static const int32_t Capacity = 64;
		static const int32_t IndexMask = 63;

	private:
        Tools::Bitset64 _bitset;
		int32_t _bestIndex;
		int32_t _bestTicks; 
        enum Side _side;
        [[maybe_unused]] uint8_t _reserved[47] = {0};
		int32_t _quantities[64];

	public:
		enum Side Side() const
		{
			return _side;
		}

		Tools::Bitset64 Bitset() const
		{
			return _bitset;
		}
		
		int32_t Count() const
		{
			return _bitset.Count();
		}
		
		bool IsEmpty() const
		{
			return _bitset.IsEmpty();
		}

		void Clear()
		{
			_bitset.ClearAll();
			_bestIndex = -1;
			_bestTicks = 0;
			std::memset(_quantities, 0, sizeof(_quantities));
		}

        Level operator[](int32_t index) const
        {
            if (index < 0 || index >= Count())
            {
                throw std::out_of_range("Index out of bounds");
            }

            Tools::Bitset64 rotated = _bitset.RotateRight(_bestIndex);
            int32_t bestOffset = rotated.GetNthSetBit(index);

            int32_t ringIndex = MapBestOffsetToRingIndex(bestOffset);
            int32_t ticks = MapBestOffsetToTicks(bestOffset);
            int32_t quantity = _quantities[ringIndex];

            return Level(ticks, quantity);
        }
        
        int32_t BestTicks() const
        {
            return _bestTicks;
        }

		SideByPrice64(enum Side side) : _bitset(0), _bestIndex(-1), _bestTicks(0), _side(side)
		{
			std::memset(_quantities, 0, sizeof(_quantities));
		}

		int32_t MapBestOffsetToRingIndex(int32_t bestOffset) const
		{
			return (_bestIndex + bestOffset) & IndexMask;
		}

		int32_t MapTicksToBestOffset(int32_t ticks) const
		{
			return (int32_t)_side * (_bestTicks - ticks);
		}

		int32_t MapRingIndexToTicks(int32_t ringIndex) const
		{
			int32_t bestOffset = (ringIndex - _bestIndex) & IndexMask;
			return _bestTicks - bestOffset * (int32_t)_side;
		}

		int32_t MapBestOffsetToTicks(int32_t bestOffset) const
		{
			return _bestTicks - bestOffset * (int32_t)_side;
		}

		int32_t GetQuantity(int32_t ticks) const
		{
			int32_t bestOffset = MapTicksToBestOffset(ticks);

			if (bestOffset < 0 || bestOffset > IndexMask)
				return 0;

			int32_t ringIndex = MapBestOffsetToRingIndex(bestOffset);
			
			if (!_bitset[ringIndex])
				return 0;

			return _quantities[ringIndex];
		}

		bool TrySetQuantity(int32_t ticks, int32_t quantity, int32_t& delta)
		{
			if (_bitset.Raw() == 0ULL)
			{
				_bestIndex = 0;
				_bestTicks = ticks;
			}

			int32_t bestOffset = MapTicksToBestOffset(ticks);
			bool isNewZero = quantity == 0;
			bool isBetter = bestOffset < 0;

			if (!isBetter && bestOffset >= Capacity)
			{
				delta = 0;
				return false;
			}

			if (isBetter && bestOffset <= -Capacity)
			{
				if (isNewZero)
				{
					delta = 0;
					return false;
				}

				_bitset.ClearAll();
				_bestTicks = ticks;
				_quantities[_bestIndex] = quantity;
				delta = quantity;
				_bitset.Set(_bestIndex);
				
				return true;
			}

			int32_t ringIndex = MapBestOffsetToRingIndex(bestOffset);
			int32_t isOldNonZero = (!isBetter && _bitset[ringIndex]) ? 1 : 0;

			delta = quantity - _quantities[ringIndex] * isOldNonZero;
			_quantities[ringIndex] = quantity;

			if (isNewZero)
			{
				_bitset.Clear(ringIndex);
				if (ringIndex == _bestIndex)
				{
					int32_t nextBestIndex = _bitset.FirstSet(ringIndex);
					
					if (nextBestIndex == -1)
						nextBestIndex = _bitset.FirstSet(0);
					
					if (nextBestIndex != -1)
					{
						_bestTicks = MapRingIndexToTicks(nextBestIndex);
						_bestIndex = nextBestIndex;
					}
					else 
					{
						_bestIndex = nextBestIndex;
					}
				}
				
				return true;
			}

			_bitset.Set(ringIndex);

			if (isBetter)
			{
				_bestTicks = ticks;
				_bitset.ClearOutside(_bestIndex, ringIndex);
				_bestIndex = ringIndex;
			}

			return true;
		}
		
		Enumerator GetEnumerator() const;
		std::string ToString() const;
	};
	#pragma pack(pop)

	static_assert(Tools::PlainOldData<SideByPrice64>, "SideByPrice64 must be unmanaged");

	// ─────────────────────────────────────────────────────────────────────────
	// Enumerator Definition
	// ─────────────────────────────────────────────────────────────────────────
	struct SideByPrice64::Enumerator
	{
	private:
		SideByPrice64 _sbp; 
		Tools::Bitset64 _rotatedBitset;
		Level _current;

	public:
		Enumerator(const SideByPrice64& src) : _sbp(src), _current(0, 0)
		{
			if (src._bestIndex != -1)
			{
				_rotatedBitset = src.Bitset().RotateRight(src._bestIndex);
			}
			else
			{
				_rotatedBitset = Tools::Bitset64(0);
			}
		}

		Level Current() const
		{
			return _current;
		}

		bool MoveNext()
		{
			if (_rotatedBitset.Raw() == 0ULL)
				return false;

			int32_t bestOffset = static_cast<int32_t>(std::countr_zero(_rotatedBitset.Raw()));
			
			_rotatedBitset.Raw(_rotatedBitset.Raw() & ~(1ULL << bestOffset));

			int32_t ringIndex = _sbp.MapBestOffsetToRingIndex(bestOffset);
			int32_t qty = _sbp._quantities[ringIndex];
			int32_t ticks = _sbp.MapBestOffsetToTicks(bestOffset);

			_current = Level(ticks, qty);
			
			return true;
		}

		struct Iterator
		{
			Enumerator* _enum;
			bool _finished;
			
			Iterator(Enumerator* e, bool finished) : _enum(e), _finished(finished)
			{
				if (!_finished)
					++(*this);
			}
			
			Level operator*() const
			{
				return _enum->Current();
			}
			
			Iterator& operator++()
			{
				if (!_enum->MoveNext())
					_finished = true;
					
				return *this;
			}
			
			bool operator!=(const Iterator& other) const
			{
				return _finished != other._finished;
			}
		};

		Iterator begin()
		{
			return Iterator(this, false);
		}
		
		Iterator end()
		{
			return Iterator(this, true);
		}
	};

	// ── Implementation of Delayed Methods ────────────────────────────────────

	inline SideByPrice64::Enumerator SideByPrice64::GetEnumerator() const
	{
		return Enumerator(*this);
	}

	inline std::string SideByPrice64::ToString() const
	{
		if (_bitset.Raw() == 0ULL)
			return "[]";

		std::stringstream sb;
		sb << "[";
		bool first = true;
		SideByPrice64::Enumerator enumerator = GetEnumerator();
		
		while (enumerator.MoveNext())
		{
			Level lvl = enumerator.Current();
			
			if (!first)
				sb << ", ";
				
			sb << "(" << lvl.Ticks << ", " << lvl.Quantity << ")";
			first = false;
		}
		
		sb << "]";
		return sb.str();
	}

	// ─────────────────────────────────────────────────────────────────────────
	// MarketByPrice64
	// ─────────────────────────────────────────────────────────────────────────
	struct MarketByPrice64
	{
		SideByPrice64 Bids;
		SideByPrice64 Asks;
		Tools::Timestamp ExchangeTimestamp;
		Tools::Timestamp SendingTimestamp;
		Tools::Timestamp NicTimestamp;

		MarketByPrice64() : Bids(Side::Buy), Asks(Side::Sell), ExchangeTimestamp(0), SendingTimestamp(0), NicTimestamp(0)
		{
		}

		std::string ToString() const
		{
			std::stringstream sb;
			sb << "MarketByPrice64 " << ExchangeTimestamp.ToString() << " " << SendingTimestamp.ToString() << " " << NicTimestamp.ToString()
			   << " BidsCount: " << Bids.Count() << " AsksCount: " << Asks.Count() << "\n";

			std::vector<Level> askLevels;
			askLevels.reserve(static_cast<size_t>(Asks.Count()));
			SideByPrice64::Enumerator asksEnum = Asks.GetEnumerator();
			
			while (asksEnum.MoveNext())
				askLevels.push_back(asksEnum.Current());
			
			uint32_t size = static_cast<uint32_t>(askLevels.size());
			
			for (uint32_t i = 0; i < size; ++i)
				sb << askLevels[size - 1 - i].Ticks << " " << askLevels[size - 1 - i].Quantity << " \n";

			sb << "-\n";

			SideByPrice64::Enumerator bidsEnum = Bids.GetEnumerator();
			
			while (bidsEnum.MoveNext())
			{   
				Level bid = bidsEnum.Current();
				sb << bid.Ticks << " " << bid.Quantity << "\n";
			}

			return sb.str();
		}

		std::string BidsAsString() const
		{
			return Bids.ToString();
		}
		
		std::string AsksAsString() const
		{
			return Asks.ToString();
		}

		MarketByPrice& CopyToSnapshot(int32_t instrumentId, std::span<uint8_t>& dst) const
		{
			MarketByPrice& mbp = *reinterpret_cast<MarketByPrice*>(dst.data());
            mbp.TickHeader = 
            {
                .TickType = TickType::MarketByPriceSnapshot,
                .InstrumentId = instrumentId,
                .ExchangeTimestamp = ExchangeTimestamp,
				.SendingTimestamp = SendingTimestamp,
                .NicTimestamp = NicTimestamp
            };
			mbp.BidsCount = Bids.Count();
			mbp.AsksCount = Asks.Count();

			uint32_t b = 0;
			std::span<Level> bidsSpan = mbp.BidsAsSpan(dst);
			SideByPrice64::Enumerator bidsEnum = Bids.GetEnumerator();
			
			while (bidsEnum.MoveNext())
				bidsSpan[b++] = bidsEnum.Current();

			uint32_t a = 0;
			std::span<Level> asksSpan = mbp.AsksAsSpan(dst);
			SideByPrice64::Enumerator asksEnum = Asks.GetEnumerator();
			
			while (asksEnum.MoveNext())
				asksSpan[a++] = asksEnum.Current();

            dst = dst.subspan(0, static_cast<size_t>(mbp.SizeOf()));

			return mbp;
		}

        bool TrySetAsDeltas(std::span<uint8_t> src)
        {
            MarketByPrice& mbp = *reinterpret_cast<MarketByPrice*>(src.data());
            
            if (mbp.TickHeader.ExchangeTimestamp < ExchangeTimestamp)
                return false;

            if (mbp.TickHeader.TickType == TickType::MarketByPriceSnapshot)
                throw std::invalid_argument("Not allowed, Unsupported.");

            ExchangeTimestamp = mbp.TickHeader.ExchangeTimestamp;
			SendingTimestamp = mbp.TickHeader.SendingTimestamp;
            NicTimestamp = mbp.TickHeader.NicTimestamp;

            mbp.TickHeader.TickType = TickType::MarketByPriceDelta;
            bool any = false;

            for (Level& level : mbp.BidsAsSpan(src))
            {
                int32_t bidQuantityDelta;
                TrySetBidQuantity(level.Ticks, level.Quantity, bidQuantityDelta);
                level.Quantity = bidQuantityDelta;
                any |= bidQuantityDelta != 0;
            }

            for (Level& level : mbp.AsksAsSpan(src))
            {
                int32_t askQuantityDelta;
                TrySetAskQuantity(level.Ticks, level.Quantity, askQuantityDelta);
                level.Quantity = askQuantityDelta;
                any |= askQuantityDelta != 0;
            }

            return any;
        }

		bool TrySet(std::span<const uint8_t> rsrc)
		{
			const MarketByPrice& mbp = *reinterpret_cast<const MarketByPrice*>(rsrc.data());
			
			if (mbp.TickHeader.ExchangeTimestamp < ExchangeTimestamp)
				return false;

			ExchangeTimestamp = mbp.TickHeader.ExchangeTimestamp;
			SendingTimestamp = mbp.TickHeader.SendingTimestamp;
			NicTimestamp = mbp.TickHeader.NicTimestamp;

			if (mbp.TickHeader.TickType == TickType::MarketByPriceSnapshot)
				Clear();

			int32_t dummy;
			
			if (mbp.TickHeader.TickType == TickType::MarketByPriceDelta)
			{
				for (const Level& level : mbp.BidsAsSpan(rsrc))
					Bids.TrySetQuantity(level.Ticks, level.Quantity + Bids.GetQuantity(level.Ticks), dummy);
					
				for (const Level& level : mbp.AsksAsSpan(rsrc))
					Asks.TrySetQuantity(level.Ticks, level.Quantity + Asks.GetQuantity(level.Ticks), dummy);
			}
			else
			{
				for (const Level& level : mbp.BidsAsSpan(rsrc))
					Bids.TrySetQuantity(level.Ticks, level.Quantity, dummy);
					
				for (const Level& level : mbp.AsksAsSpan(rsrc))
					Asks.TrySetQuantity(level.Ticks, level.Quantity, dummy);
			}

			return true;
		}

		Level BestBid() const
		{
			return Level(Bids._bestTicks, Bids.GetQuantity(Bids._bestTicks));
		}
		
		Level BestAsk() const
		{
			return Level(Asks._bestTicks, Asks.GetQuantity(Asks._bestTicks));
		}
		
		int32_t BidsCount() const
		{
			return Bids.Count();
		}
		
		int32_t AsksCount() const
		{
			return Asks.Count();
		}
		
		bool IsEmpty() const
		{
			return Bids.IsEmpty() && Asks.IsEmpty();
		}
		
		Tools::Bitset64 BidsBitset() const
		{
			return Bids.Bitset();
		}
		
		Tools::Bitset64 AsksBitset() const
		{
			return Asks.Bitset();
		}

		bool TrySetBidQuantity(int32_t ticks, int32_t quantity, int32_t& delta)
		{
			return Bids.TrySetQuantity(ticks, quantity, delta);
		}
		
		bool TrySetAskQuantity(int32_t ticks, int32_t quantity, int32_t& delta)
		{
			return Asks.TrySetQuantity(ticks, quantity, delta);
		}
		
		int32_t GetBidQuantity(int32_t ticks) const
		{
			return Bids.GetQuantity(ticks);
		}
		
		int32_t GetAskQuantity(int32_t ticks) const
		{
			return Asks.GetQuantity(ticks);
		}
		
		void Clear()
		{
			Bids.Clear();
			Asks.Clear();
		}

		SideByPrice64::Enumerator EnumerateBids() const
		{
			return Bids.GetEnumerator();
		}
		
		SideByPrice64::Enumerator EnumerateAsks() const
		{
			return Asks.GetEnumerator();
		}
	};
}