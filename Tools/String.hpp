#pragma once

#include "Tools.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <glaze/glaze.hpp>
#include <string>

namespace Tools
{
// High-performance fixed-size string wrapper for shared memory.
// Guaranteed to be trivially copyable and standard layout.
template <size_t N>
struct alignas(1) StringN
{
	uint8_t Chars[N];
	static constexpr size_t Capacity = N;



	// Default constructor: Zero-init
	StringN()
	{
		std::memset(Chars, 0, N);
	}

	// Construct from C++ string
	StringN(const std::string& s)
	{
		Set(s);
	}

	// Construct from C-string
	StringN(const char* s)
	{
		Set(s);
	}

	// Set content safely
	void Set(const std::string& s)
	{
		if (s.length() > N)
			throw std::runtime_error("String<" + std::to_string(N) + ">.Set can not fit"+ s);

		std::memset(Chars, 0, N);
		size_t len =
			std::min(s.length(), N); // If exact fit, no null terminator at end?
		// C# implementation: "s.Length > capacity ? capacity : s.Length"
		// And then it zeros the rest.
		// Note: If s.length() == N, the C# impl fills the WHOLE buffer with data.
		// It does NOT enforce a null terminator if it fills the buffer.
		// My previous C++ impl forced N-1.
		// I will match the C# behavior: can fill completely.
		std::memcpy(Chars, s.data(), len);
	}

	void Set(const char* s)
	{
		std::memset(Chars, 0, N);
		if (s)
		{
			size_t len = std::strlen(s);
			if (len > N)
				len = N;
			std::memcpy(Chars, s, len);
		}
	}

	// Conversion to std::string
	std::string ToString() const
	{
		// Find length up to N or first null
		size_t len = 0;
		while (len < N && Chars[len] != 0)
			len++;
		return std::string(reinterpret_cast<const char*>(Chars), len);
	}

	// Equality
	bool operator==(const StringN& other) const
	{
		return std::memcmp(Chars, other.Chars, N) == 0;
	}

	bool operator!=(const StringN& other) const
	{
		return !(*this == other);
	}

	// Implicit string conversion for convenience
	operator std::string() const
	{
		return ToString();
	}


    struct glaze
    {
        using T = StringN<N>;
        using SetMethodPtr = void (T::*)(const std::string&);
        
        static constexpr decltype(glz::custom<static_cast<SetMethodPtr>(&T::Set), &T::ToString>) value = 
            glz::custom<static_cast<SetMethodPtr>(&T::Set), &T::ToString>;
    };
};

static_assert(Tools::PlainOldData<StringN<4>>);

// Common Sizes matching C# definitions
using String4 = StringN<4>;
using String8 = StringN<8>;
using String16 = StringN<16>;
using String32 = StringN<32>;
using String64 = StringN<64>;
using String128 = StringN<128>;
using String256 = StringN<256>;
using String512 = StringN<512>;
} // namespace Tools
