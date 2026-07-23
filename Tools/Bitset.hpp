#pragma once

#include <atomic>
#include <cstdint>
#include <bit>
#include <glaze/core/common.hpp>
#include <string>
#include <sstream>
#include <immintrin.h>

namespace Tools
{
    /// <summary>
    /// Fixed-width 64-bit bitset optimized for ultra-low-latency operations.
    /// </summary>
    class Bitset64
    {
    private:
        alignas(8) uint64_t _bits;

    public:

        // ─────────────────────────────────────────────────────────────────────────
        // Construction
        // ─────────────────────────────────────────────────────────────────────────

        Bitset64() : _bits(0ULL)
        {
        }

        Bitset64(uint64_t initial) : _bits(initial)
        {
        }

        uint64_t Raw() const
        {
            return _bits;
        }

        int32_t Length() const
        {
            return 64;
        }

        void Raw(uint64_t value)
        {
            _bits = value;
        }

        struct glaze
        {
            static constexpr auto value = glz::object(
                "Raw", &Bitset64::_bits
            );
        };

        // ─────────────────────────────────────────────────────────────────────────
        // Bit access
        // ─────────────────────────────────────────────────────────────────────────

        bool operator[](int32_t index) const
        {
            return (_bits & (1ULL << index)) != 0ULL;
        }

        // ─────────────────────────────────────────────────────────────────────────
        // Mutations
        // ─────────────────────────────────────────────────────────────────────────

        uint64_t AtomicLoad() const
        {
            return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(_bits)).load(std::memory_order_acquire);
        }

        void AtomicSet(int32_t index)
        {
            uint64_t mask = 1ULL << index;
            std::atomic_ref<uint64_t>(_bits).fetch_or(mask, std::memory_order_release);
        }

        void AtomicClear(int32_t index)
        {
            uint64_t mask = ~(1ULL << index);
            std::atomic_ref<uint64_t>(_bits).fetch_and(mask, std::memory_order_release);
        }

        void Set(int32_t index)
        {
            _bits |= (1ULL << index);
        }

        void Clear(int32_t index)
        {
            _bits &= ~(1ULL << index);
        }

        void Toggle(int32_t index)
        {
            _bits ^= (1ULL << index);
        }

        void SetAll()
        {
            _bits = ~0ULL;
        }

        void ClearAll()
        {
            _bits = 0ULL;
        }

        void Fill(bool value)
        {
            _bits = value ? ~0ULL : 0ULL;
        }

        // ─────────────────────────────────────────────────────────────────────────
        // Properties
        // ─────────────────────────────────────────────────────────────────────────

        int32_t Count() const
        {
            return static_cast<int32_t>(std::popcount(_bits));
        }

        bool IsEmpty() const
        {
            return _bits == 0ULL;
        }

        bool IsFull() const
        {
            return _bits == ~0ULL;
        }

        int32_t HighestClear() const
        {
            uint64_t inverted = ~_bits;
            
            if (inverted == 0ULL) 
                return -1;
                
            return static_cast<int32_t>(63 - std::countl_zero(inverted));
        }

        int32_t LowestClear() const
        {
            uint64_t inverted = ~_bits;
            
            if (inverted == 0ULL) 
                return -1;
                
            return static_cast<int32_t>(std::countr_zero(inverted));
        }

        int32_t GetNthSetBit(int32_t n) const
        {
            uint64_t result = _pdep_u64(1ULL << n, _bits);
            return static_cast<int32_t>(std::countr_zero(result));
        }

        /// <summary>Lowest set bit index or Bitset64::-1 if empty.</summary>
        int32_t LowestSet() const
        {
            return _bits == 0ULL ? -1 : static_cast<int32_t>(std::countr_zero(_bits));
        }

        /// <summary>Highest set bit index or Bitset64::-1 if empty.</summary>
        int32_t HighestSet() const
        {
            return _bits == 0ULL ? -1 : static_cast<int32_t>(63 - std::countl_zero(_bits));
        }

        // ─────────────────────────────────────────────────────────────────────────
        // Scanning / selection
        // ─────────────────────────────────────────────────────────────────────────

        void ClearAbove(int32_t maxIndexInclusive)
        {
            if (maxIndexInclusive >= 63)
                return;

            if (maxIndexInclusive < 0)
            {
                _bits = 0ULL;
                return;
            }

            uint64_t keepMask = (1ULL << (maxIndexInclusive + 1)) - 1ULL;
            _bits &= keepMask;
        }

        // In Bitset64
        static uint64_t MaskBetween(int32_t from, int32_t to)
        {
            if (from < 0)
                from = 0;

            if (to >= 63)
                to = 63;

            uint64_t upToTo = (to == 63) ? ~0ULL : ((1ULL << (to + 1)) - 1ULL);
            uint64_t fromOn = (from == 0) ? ~0ULL : ~((1ULL << from) - 1ULL);

            return (from <= to) ? (upToTo & fromOn) : (fromOn | upToTo);
        }

        void ClearOutside(int32_t from, int32_t to)
        {
            uint64_t keepMask = MaskBetween(from, to);
            _bits &= keepMask;
        }

        void ClearBelow(int32_t minIndexInclusive)
        {
            if (minIndexInclusive <= 0)
                return;

            if (minIndexInclusive >= 64)
            {
                _bits = 0ULL;
                return;
            }

            _bits &= ~((1ULL << minIndexInclusive) - 1ULL);
        }

        int32_t FirstSet(int32_t from) const
        {
            if (from < 0 || from >= 64) return -1;
            uint64_t shifted = _bits >> from;
            return shifted == 0ULL ? -1 : from + static_cast<int32_t>(std::countr_zero(shifted));
        }

        Bitset64 RotateRight(int32_t count) const
        {
            return Bitset64(std::rotr(_bits, count));
        }

        bool TryPopLowest(int32_t& index)
        {
            uint64_t b = _bits;
            if (b == 0ULL) { index = -1; return false; }
            
            index = static_cast<int32_t>(std::countr_zero(b));
            _bits = b & (b - 1ULL);
            return true;
        }

        bool TryPopHighest(int32_t& index)
        {
            uint64_t b = _bits;
            if (b == 0ULL) { index = -1; return false; }
            
            index = static_cast<int32_t>(63 - std::countl_zero(b));
            _bits &= ~(1ULL << index);
            return true;
        }

        // ─────────────────────────────────────────────────────────────────────────
        // Relations and operators
        // ─────────────────────────────────────────────────────────────────────────

        bool Overlaps(Bitset64 other) const { return (_bits & other._bits) != 0ULL; }
        bool IsSubsetOf(Bitset64 other) const { return (_bits & ~other._bits) == 0ULL; }
        bool IsSupersetOf(Bitset64 other) const { return other.IsSubsetOf(*this); }

        Bitset64 operator&(Bitset64 b) const { return Bitset64(_bits & b._bits); }
        Bitset64 operator|(Bitset64 b) const { return Bitset64(_bits | b._bits); }
        Bitset64 operator^(Bitset64 b) const { return Bitset64(_bits ^ b._bits); }
        Bitset64 operator~() const { return Bitset64(~_bits); }
        bool operator==(Bitset64 b) const { return _bits == b._bits; }
        bool operator!=(Bitset64 b) const { return _bits != b._bits; }

        // ─────────────────────────────────────────────────────────────────────────
        // Enumeration
        // ─────────────────────────────────────────────────────────────────────────

        class Enumerator
        {
        private:
            uint64_t _remaining;
            int32_t _current;

        public:
            Enumerator(uint64_t bits) : _remaining(bits), _current(-1)
            {
                if (_remaining != 0ULL)
                {
                    _current = static_cast<int32_t>(std::countr_zero(_remaining));
                }
            }

            // End iterator state
            Enumerator() : _remaining(0ULL), _current(-1)
            {
            }

            int32_t operator*() const { return _current; }

            Enumerator& operator++()
            {
                if (_remaining != 0ULL)
                {
                    // Clear the bit we just visited
                    _remaining &= _remaining - 1ULL;

                    if (_remaining != 0ULL)
                    {
                        _current = static_cast<int32_t>(std::countr_zero(_remaining));
                    }
                    else
                    {
                        _current = -1;
                    }
                }
                return *this;
            }

            bool operator!=(const Enumerator& other) const
            {
                return _current != other._current;
            }
        };

        Enumerator begin() const { return Enumerator(_bits); }
        Enumerator end() const { return Enumerator(); }

        // ─────────────────────────────────────────────────────────────────────────
        // ToString
        // ─────────────────────────────────────────────────────────────────────────

        std::string ToString() const
        {
            uint64_t b = _bits;
            if (b == 0ULL) return "[]";

            std::stringstream sb;
            sb << "[ ";

            bool first = true;
            while (b != 0ULL)
            {
                int32_t i = static_cast<int32_t>(std::countr_zero(b));
                b &= b - 1ULL;

                if (!first) sb << ", ";
                sb << i;
                first = false;
            }

            sb << " ]";
            return sb.str();
        }

        std::string ToBitString() const
        {
            std::stringstream sb;
            sb << '|';
            for (int32_t i = 0; i < 64; i++)
            {
                sb << ' ';
                sb << (((_bits >> i) & 1ULL) != 0ULL ? '1' : '0');
                sb << " |";
            }
            return sb.str();
        }
    };
}