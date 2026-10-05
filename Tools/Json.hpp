#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <glaze/glaze.hpp>
#include <magic_enum.hpp>
#include "Tools.hpp"

namespace Tools
{
	// Text exactly as C#'s Tools.Json writes it (System.Text.Json with its custom converters).
	namespace JsonText
	{
		// C#'s DoubleJsonConverter writes value.ToString("0.#############################"): .NET rounds to
		// 15 significant digits (7 for float), then half-up on those digits to at most 29 decimals, and
		// never uses an exponent. A negative value that rounds to zero keeps its sign ("-0"). NaN and the
		// infinities are JSON strings. Returns the length written; 352 bytes always suffice.
		template <std::floating_point F>
		inline size_t FormatNumber(char* out, F value)
		{
			if (std::isnan(value))
			{
				std::memcpy(out, "\"NaN\"", 5);
				return 5;
			}
			if (std::isinf(value))
			{
				if (value > 0)
				{
					std::memcpy(out, "\"Infinity\"", 10);
					return 10;
				}
				std::memcpy(out, "\"-Infinity\"", 11);
				return 11;
			}

			constexpr int precision = std::same_as<F, float> ? 7 : 15;
			constexpr int maxDecimals = 29;

			// "d.dddde+XX": precision significant digits, correctly rounded (ties to even, as .NET).
			char scientific[48];
			const char* end = std::to_chars(scientific, scientific + sizeof(scientific), std::fabs(value), std::chars_format::scientific, precision - 1).ptr;
			char digits[precision];
			digits[0] = scientific[0];
			std::memcpy(digits + 1, scientific + 2, precision - 1);
			const char* e = scientific + precision + 1;
			int exponent = 0;
			std::from_chars(e + 2, end, exponent);
			if (e[1] == '-')
				exponent = -exponent;

			int scale = exponent + 1; // digits before the decimal point
			int count = precision;
			int position = scale + maxDecimals;
			if (position < count)
			{
				if (position >= 0 && digits[position] >= '5')
				{
					int i = position;
					while (i > 0 && digits[i - 1] == '9')
						i--;
					if (i > 0)
						digits[i - 1]++;
					else
					{
						scale++;
						digits[0] = '1';
						i = 1;
					}
					count = i;
				}
				else
					count = std::max(position, 0);
			}
			while (count > 0 && digits[count - 1] == '0')
				count--;
			if (count == 0)
				scale = 0;

			char* p = out;
			if (std::signbit(value))
				*p++ = '-';
			if (scale <= 0)
				*p++ = '0';
			for (int i = 0; i < scale; i++)
				*p++ = i < count ? digits[i] : '0';
			if (count > scale)
			{
				*p++ = '.';
				for (int i = scale; i < 0; i++)
					*p++ = '0';
				for (int i = std::max(scale, 0); i < count; i++)
					*p++ = digits[i];
			}
			return static_cast<size_t>(p - out);
		}

		// BMP code points (from U+0080) that .NET 10's UnsafeRelaxedJsonEscaping still escapes: C1
		// controls, U+00A0, and every code point its Unicode tables treat as unassigned, formatting
		// or separator. Generated from the encoder itself; every supplementary code point is escaped too.
		inline constexpr std::array<std::array<uint16_t, 2>, 343> EscapedRanges = {{
			{0x0080, 0x00A0}, {0x0378, 0x0379}, {0x0380, 0x0383}, {0x038B, 0x038B}, {0x038D, 0x038D}, {0x03A2, 0x03A2}, {0x0530, 0x0530}, {0x0557, 0x0558},
			{0x058B, 0x058C}, {0x0590, 0x0590}, {0x05C8, 0x05CF}, {0x05EB, 0x05EE}, {0x05F5, 0x05FF}, {0x070E, 0x070E}, {0x074B, 0x074C}, {0x07B2, 0x07BF},
			{0x07FB, 0x07FC}, {0x082E, 0x082F}, {0x083F, 0x083F}, {0x085C, 0x085D}, {0x085F, 0x085F}, {0x086B, 0x086F}, {0x088F, 0x088F}, {0x0892, 0x0896},
			{0x0984, 0x0984}, {0x098D, 0x098E}, {0x0991, 0x0992}, {0x09A9, 0x09A9}, {0x09B1, 0x09B1}, {0x09B3, 0x09B5}, {0x09BA, 0x09BB}, {0x09C5, 0x09C6},
			{0x09C9, 0x09CA}, {0x09CF, 0x09D6}, {0x09D8, 0x09DB}, {0x09DE, 0x09DE}, {0x09E4, 0x09E5}, {0x09FF, 0x0A00}, {0x0A04, 0x0A04}, {0x0A0B, 0x0A0E},
			{0x0A11, 0x0A12}, {0x0A29, 0x0A29}, {0x0A31, 0x0A31}, {0x0A34, 0x0A34}, {0x0A37, 0x0A37}, {0x0A3A, 0x0A3B}, {0x0A3D, 0x0A3D}, {0x0A43, 0x0A46},
			{0x0A49, 0x0A4A}, {0x0A4E, 0x0A50}, {0x0A52, 0x0A58}, {0x0A5D, 0x0A5D}, {0x0A5F, 0x0A65}, {0x0A77, 0x0A80}, {0x0A84, 0x0A84}, {0x0A8E, 0x0A8E},
			{0x0A92, 0x0A92}, {0x0AA9, 0x0AA9}, {0x0AB1, 0x0AB1}, {0x0AB4, 0x0AB4}, {0x0ABA, 0x0ABB}, {0x0AC6, 0x0AC6}, {0x0ACA, 0x0ACA}, {0x0ACE, 0x0ACF},
			{0x0AD1, 0x0ADF}, {0x0AE4, 0x0AE5}, {0x0AF2, 0x0AF8}, {0x0B00, 0x0B00}, {0x0B04, 0x0B04}, {0x0B0D, 0x0B0E}, {0x0B11, 0x0B12}, {0x0B29, 0x0B29},
			{0x0B31, 0x0B31}, {0x0B34, 0x0B34}, {0x0B3A, 0x0B3B}, {0x0B45, 0x0B46}, {0x0B49, 0x0B4A}, {0x0B4E, 0x0B54}, {0x0B58, 0x0B5B}, {0x0B5E, 0x0B5E},
			{0x0B64, 0x0B65}, {0x0B78, 0x0B81}, {0x0B84, 0x0B84}, {0x0B8B, 0x0B8D}, {0x0B91, 0x0B91}, {0x0B96, 0x0B98}, {0x0B9B, 0x0B9B}, {0x0B9D, 0x0B9D},
			{0x0BA0, 0x0BA2}, {0x0BA5, 0x0BA7}, {0x0BAB, 0x0BAD}, {0x0BBA, 0x0BBD}, {0x0BC3, 0x0BC5}, {0x0BC9, 0x0BC9}, {0x0BCE, 0x0BCF}, {0x0BD1, 0x0BD6},
			{0x0BD8, 0x0BE5}, {0x0BFB, 0x0BFF}, {0x0C0D, 0x0C0D}, {0x0C11, 0x0C11}, {0x0C29, 0x0C29}, {0x0C3A, 0x0C3B}, {0x0C45, 0x0C45}, {0x0C49, 0x0C49},
			{0x0C4E, 0x0C54}, {0x0C57, 0x0C57}, {0x0C5B, 0x0C5C}, {0x0C5E, 0x0C5F}, {0x0C64, 0x0C65}, {0x0C70, 0x0C76}, {0x0C8D, 0x0C8D}, {0x0C91, 0x0C91},
			{0x0CA9, 0x0CA9}, {0x0CB4, 0x0CB4}, {0x0CBA, 0x0CBB}, {0x0CC5, 0x0CC5}, {0x0CC9, 0x0CC9}, {0x0CCE, 0x0CD4}, {0x0CD7, 0x0CDC}, {0x0CDF, 0x0CDF},
			{0x0CE4, 0x0CE5}, {0x0CF0, 0x0CF0}, {0x0CF4, 0x0CFF}, {0x0D0D, 0x0D0D}, {0x0D11, 0x0D11}, {0x0D45, 0x0D45}, {0x0D49, 0x0D49}, {0x0D50, 0x0D53},
			{0x0D64, 0x0D65}, {0x0D80, 0x0D80}, {0x0D84, 0x0D84}, {0x0D97, 0x0D99}, {0x0DB2, 0x0DB2}, {0x0DBC, 0x0DBC}, {0x0DBE, 0x0DBF}, {0x0DC7, 0x0DC9},
			{0x0DCB, 0x0DCE}, {0x0DD5, 0x0DD5}, {0x0DD7, 0x0DD7}, {0x0DE0, 0x0DE5}, {0x0DF0, 0x0DF1}, {0x0DF5, 0x0E00}, {0x0E3B, 0x0E3E}, {0x0E5C, 0x0E80},
			{0x0E83, 0x0E83}, {0x0E85, 0x0E85}, {0x0E8B, 0x0E8B}, {0x0EA4, 0x0EA4}, {0x0EA6, 0x0EA6}, {0x0EBE, 0x0EBF}, {0x0EC5, 0x0EC5}, {0x0EC7, 0x0EC7},
			{0x0ECF, 0x0ECF}, {0x0EDA, 0x0EDB}, {0x0EE0, 0x0EFF}, {0x0F48, 0x0F48}, {0x0F6D, 0x0F70}, {0x0F98, 0x0F98}, {0x0FBD, 0x0FBD}, {0x0FCD, 0x0FCD},
			{0x0FDB, 0x0FFF}, {0x10C6, 0x10C6}, {0x10C8, 0x10CC}, {0x10CE, 0x10CF}, {0x1249, 0x1249}, {0x124E, 0x124F}, {0x1257, 0x1257}, {0x1259, 0x1259},
			{0x125E, 0x125F}, {0x1289, 0x1289}, {0x128E, 0x128F}, {0x12B1, 0x12B1}, {0x12B6, 0x12B7}, {0x12BF, 0x12BF}, {0x12C1, 0x12C1}, {0x12C6, 0x12C7},
			{0x12D7, 0x12D7}, {0x1311, 0x1311}, {0x1316, 0x1317}, {0x135B, 0x135C}, {0x137D, 0x137F}, {0x139A, 0x139F}, {0x13F6, 0x13F7}, {0x13FE, 0x13FF},
			{0x1680, 0x1680}, {0x169D, 0x169F}, {0x16F9, 0x16FF}, {0x1716, 0x171E}, {0x1737, 0x173F}, {0x1754, 0x175F}, {0x176D, 0x176D}, {0x1771, 0x1771},
			{0x1774, 0x177F}, {0x17DE, 0x17DF}, {0x17EA, 0x17EF}, {0x17FA, 0x17FF}, {0x181A, 0x181F}, {0x1879, 0x187F}, {0x18AB, 0x18AF}, {0x18F6, 0x18FF},
			{0x191F, 0x191F}, {0x192C, 0x192F}, {0x193C, 0x193F}, {0x1941, 0x1943}, {0x196E, 0x196F}, {0x1975, 0x197F}, {0x19AC, 0x19AF}, {0x19CA, 0x19CF},
			{0x19DB, 0x19DD}, {0x1A1C, 0x1A1D}, {0x1A5F, 0x1A5F}, {0x1A7D, 0x1A7E}, {0x1A8A, 0x1A8F}, {0x1A9A, 0x1A9F}, {0x1AAE, 0x1AAF}, {0x1ACF, 0x1AFF},
			{0x1B4D, 0x1B4D}, {0x1BF4, 0x1BFB}, {0x1C38, 0x1C3A}, {0x1C4A, 0x1C4C}, {0x1C8B, 0x1C8F}, {0x1CBB, 0x1CBC}, {0x1CC8, 0x1CCF}, {0x1CFB, 0x1CFF},
			{0x1F16, 0x1F17}, {0x1F1E, 0x1F1F}, {0x1F46, 0x1F47}, {0x1F4E, 0x1F4F}, {0x1F58, 0x1F58}, {0x1F5A, 0x1F5A}, {0x1F5C, 0x1F5C}, {0x1F5E, 0x1F5E},
			{0x1F7E, 0x1F7F}, {0x1FB5, 0x1FB5}, {0x1FC5, 0x1FC5}, {0x1FD4, 0x1FD5}, {0x1FDC, 0x1FDC}, {0x1FF0, 0x1FF1}, {0x1FF5, 0x1FF5}, {0x1FFF, 0x200A},
			{0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x2065, 0x2065}, {0x2072, 0x2073}, {0x208F, 0x208F}, {0x209D, 0x209F}, {0x20C1, 0x20CF},
			{0x20F1, 0x20FF}, {0x218C, 0x218F}, {0x242A, 0x243F}, {0x244B, 0x245F}, {0x2B74, 0x2B75}, {0x2B96, 0x2B96}, {0x2CF4, 0x2CF8}, {0x2D26, 0x2D26},
			{0x2D28, 0x2D2C}, {0x2D2E, 0x2D2F}, {0x2D68, 0x2D6E}, {0x2D71, 0x2D7E}, {0x2D97, 0x2D9F}, {0x2DA7, 0x2DA7}, {0x2DAF, 0x2DAF}, {0x2DB7, 0x2DB7},
			{0x2DBF, 0x2DBF}, {0x2DC7, 0x2DC7}, {0x2DCF, 0x2DCF}, {0x2DD7, 0x2DD7}, {0x2DDF, 0x2DDF}, {0x2E5E, 0x2E7F}, {0x2E9A, 0x2E9A}, {0x2EF4, 0x2EFF},
			{0x2FD6, 0x2FEF}, {0x3000, 0x3000}, {0x3040, 0x3040}, {0x3097, 0x3098}, {0x3100, 0x3104}, {0x3130, 0x3130}, {0x318F, 0x318F}, {0x31E6, 0x31EE},
			{0x321F, 0x321F}, {0xA48D, 0xA48F}, {0xA4C7, 0xA4CF}, {0xA62C, 0xA63F}, {0xA6F8, 0xA6FF}, {0xA7CE, 0xA7CF}, {0xA7D2, 0xA7D2}, {0xA7D4, 0xA7D4},
			{0xA7DD, 0xA7F1}, {0xA82D, 0xA82F}, {0xA83A, 0xA83F}, {0xA878, 0xA87F}, {0xA8C6, 0xA8CD}, {0xA8DA, 0xA8DF}, {0xA954, 0xA95E}, {0xA97D, 0xA97F},
			{0xA9CE, 0xA9CE}, {0xA9DA, 0xA9DD}, {0xA9FF, 0xA9FF}, {0xAA37, 0xAA3F}, {0xAA4E, 0xAA4F}, {0xAA5A, 0xAA5B}, {0xAAC3, 0xAADA}, {0xAAF7, 0xAB00},
			{0xAB07, 0xAB08}, {0xAB0F, 0xAB10}, {0xAB17, 0xAB1F}, {0xAB27, 0xAB27}, {0xAB2F, 0xAB2F}, {0xAB6C, 0xAB6F}, {0xABEE, 0xABEF}, {0xABFA, 0xABFF},
			{0xD7A4, 0xD7AF}, {0xD7C7, 0xD7CA}, {0xD7FC, 0xD7FF}, {0xE000, 0xF8FF}, {0xFA6E, 0xFA6F}, {0xFADA, 0xFAFF}, {0xFB07, 0xFB12}, {0xFB18, 0xFB1C},
			{0xFB37, 0xFB37}, {0xFB3D, 0xFB3D}, {0xFB3F, 0xFB3F}, {0xFB42, 0xFB42}, {0xFB45, 0xFB45}, {0xFBC3, 0xFBD2}, {0xFD90, 0xFD91}, {0xFDC8, 0xFDCE},
			{0xFDD0, 0xFDEF}, {0xFE1A, 0xFE1F}, {0xFE53, 0xFE53}, {0xFE67, 0xFE67}, {0xFE6C, 0xFE6F}, {0xFE75, 0xFE75}, {0xFEFD, 0xFF00}, {0xFFBF, 0xFFC1},
			{0xFFC8, 0xFFC9}, {0xFFD0, 0xFFD1}, {0xFFD8, 0xFFD9}, {0xFFDD, 0xFFDF}, {0xFFE7, 0xFFE7}, {0xFFEF, 0xFFF8}, {0xFFFE, 0xFFFF},
		}};

		inline bool IsEscaped(uint32_t codePoint)
		{
			if (codePoint < 0x80)
				return codePoint < 0x20 || codePoint == '"' || codePoint == '\\' || codePoint == 0x7F;
			if (codePoint > 0xFFFF)
				return true;
			auto it = std::upper_bound(EscapedRanges.begin(), EscapedRanges.end(), codePoint,
				[](uint32_t value, const std::array<uint16_t, 2>& range) { return value < range[0]; });
			return it != EscapedRanges.begin() && codePoint <= (*(it - 1))[1];
		}

		inline char* WriteUnicodeEscape(char* p, uint32_t unit)
		{
			static constexpr char hex[] = "0123456789ABCDEF";
			*p++ = '\\';
			*p++ = 'u';
			*p++ = hex[(unit >> 12) & 0xF];
			*p++ = hex[(unit >> 8) & 0xF];
			*p++ = hex[(unit >> 4) & 0xF];
			*p++ = hex[unit & 0xF];
			return p;
		}

		// Length of the well-formed UTF-8 sequence at s (Unicode Table 3-7), or 0 with `length` set to
		// its maximal subpart, which C# decodes to a single U+FFFD.
		inline size_t DecodeUtf8(const unsigned char* s, size_t n, uint32_t& codePoint, size_t& length)
		{
			unsigned char b0 = s[0];
			size_t need;
			unsigned char lo = 0x80, hi = 0xBF;
			if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; codePoint = b0 & 0x1F; }
			else if (b0 >= 0xE0 && b0 <= 0xEF) { need = 2; codePoint = b0 & 0x0F; lo = b0 == 0xE0 ? 0xA0 : 0x80; hi = b0 == 0xED ? 0x9F : 0xBF; }
			else if (b0 >= 0xF0 && b0 <= 0xF4) { need = 3; codePoint = b0 & 0x07; lo = b0 == 0xF0 ? 0x90 : 0x80; hi = b0 == 0xF4 ? 0x8F : 0xBF; }
			else { length = 1; return 0; }

			length = 1;
			for (size_t i = 1; i <= need; i++)
			{
				if (i >= n || s[i] < lo || s[i] > hi)
					return 0;
				codePoint = (codePoint << 6) | (s[i] & 0x3F);
				lo = 0x80;
				hi = 0xBF;
				length = i + 1;
			}
			return length;
		}

		// Writes `text` as a quoted JSON string the way Utf8JsonWriter does with UnsafeRelaxedJsonEscaping.
		// Needs room for 2 + 12 * text.size() bytes at out. Invalid UTF-8 becomes U+FFFD, as C# decoding
		// the same bytes would produce.
		inline size_t WriteString(char* out, std::string_view text)
		{
			const auto* s = reinterpret_cast<const unsigned char*>(text.data());
			const size_t n = text.size();
			char* p = out;
			*p++ = '"';
			size_t i = 0;
			while (i < n)
			{
				unsigned char c = s[i];
				if (c >= 0x80)
				{
					uint32_t codePoint = 0;
					size_t length = 0;
					if (DecodeUtf8(s + i, n - i, codePoint, length) == 0)
					{
						std::memcpy(p, "\xEF\xBF\xBD", 3);
						p += 3;
					}
					else if (!IsEscaped(codePoint))
					{
						std::memcpy(p, s + i, length);
						p += length;
					}
					else if (codePoint > 0xFFFF)
					{
						p = WriteUnicodeEscape(p, 0xD800 + ((codePoint - 0x10000) >> 10));
						p = WriteUnicodeEscape(p, 0xDC00 + ((codePoint - 0x10000) & 0x3FF));
					}
					else
						p = WriteUnicodeEscape(p, codePoint);
					i += length;
					continue;
				}

				if (!IsEscaped(c))
					*p++ = static_cast<char>(c);
				else if (c == '"') { *p++ = '\\'; *p++ = '"'; }
				else if (c == '\\') { *p++ = '\\'; *p++ = '\\'; }
				else if (c == '\b') { *p++ = '\\'; *p++ = 'b'; }
				else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
				else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
				else if (c == '\f') { *p++ = '\\'; *p++ = 'f'; }
				else if (c == '\r') { *p++ = '\\'; *p++ = 'r'; }
				else
					p = WriteUnicodeEscape(p, c);
				i++;
			}
			*p++ = '"';
			return static_cast<size_t>(p - out);
		}
	}

	// A member C# declares nullable (string?, object?): written only when it holds something, as
	// C#'s WhenWritingNull omits a null, and read in place. Empty string / std::monostate stand for null.
	template <class V>
	struct JsonOmitEmpty
	{
		V& Value;

		explicit operator bool() const
		{
			if constexpr (requires { Value.empty(); })
				return !Value.empty();
			else
				return !std::holds_alternative<std::monostate>(Value);
		}

		V& operator*() const
		{
			return Value;
		}
	};

	template <auto Member>
	inline constexpr auto OmitEmpty = [](auto&& self) { return JsonOmitEmpty<std::remove_reference_t<decltype(self.*Member)>>{self.*Member}; };

	// A getter for a write-only member that may throw. glaze's write ops are noexcept, so the exception
	// would terminate; it writes null instead (C# would throw out of Serialize).
	template <auto Getter>
	inline constexpr auto NullIfThrows = [](const auto& self)
	{
		decltype(std::invoke(Getter, self)) result{};
		try
		{
			result = std::invoke(Getter, self);
		}
		catch (...)
		{
		}
		return result;
	};
}

namespace glz
{
	template <class T> requires std::is_enum_v<T>
	struct meta<T>
	{
		static constexpr auto value = []<std::size_t... Is>(std::index_sequence<Is...>)
		{
			return glz::enumerate(magic_enum::enum_values<T>()[Is]...);
		}(std::make_index_sequence<magic_enum::enum_count<T>()>());
	};

	// Read and written as a plain double: the double reader and writer below handle NaN.
	template <>
	struct meta<Tools::Nanouble>
	{
		static constexpr auto value = &Tools::Nanouble::value;
	};

	namespace detail
	{
		template <std::floating_point F>
		struct floating_point_to_json
		{
			template <auto Opts, class B>
			static void op(auto&& value, is_context auto&&, B&& b, auto&& ix) noexcept
			{
				char text[352];
				const size_t n = Tools::JsonText::FormatNumber<F>(text, value);
				if constexpr (resizable<B>)
				{
					if (ix + n > b.size())
						b.resize((std::max)(b.size() * 2, ix + n));
				}
				std::memcpy(data_ptr(b) + ix, text, n);
				ix += n;
			}
		};

		// Also accepts what C#'s DoubleJsonConverter reads ("NaN", "Infinity", "-Infinity", a quoted
		// number) and null, which older C++ output wrote for NaN.
		template <std::floating_point F>
		struct floating_point_from_json
		{
			template <auto Opts, class It>
			static void op(auto&& value, is_context auto&& ctx, It&& it, auto&& end) noexcept
			{
				if constexpr (!Opts.ws_handled)
				{
					GLZ_SKIP_WS;
				}

				if (*it == '"')
				{
					std::string text;
					read<json>::op<ws_handled<Opts>()>(text, ctx, it, end);
					if (bool(ctx.error)) [[unlikely]]
						return;
					if (text == "NaN")
						value = std::numeric_limits<F>::quiet_NaN();
					else if (text == "Infinity")
						value = std::numeric_limits<F>::infinity();
					else if (text == "-Infinity")
						value = -std::numeric_limits<F>::infinity();
					else
					{
						F parsed{};
						auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
						if (ec != std::errc() || ptr != text.data() + text.size()) [[unlikely]]
						{
							ctx.error = error_code::parse_number_failure;
							return;
						}
						value = parsed;
					}
				}
				else if (*it == 'n')
				{
					match<"null", Opts>(ctx, it, end);
					value = std::numeric_limits<F>::quiet_NaN();
				}
				else if (!parse_float<F, Opts.force_conformance>(value, it)) [[unlikely]]
					ctx.error = error_code::parse_number_failure;
			}
		};

		template <>
		struct to_json<double> : floating_point_to_json<double> {};
		template <>
		struct to_json<float> : floating_point_to_json<float> {};
		template <>
		struct from_json<double> : floating_point_from_json<double> {};
		template <>
		struct from_json<float> : floating_point_from_json<float> {};

		template <>
		struct to_json<std::string>
		{
			template <auto Opts, class B>
			static void op(auto&& value, is_context auto&&, B&& b, auto&& ix) noexcept
			{
				if constexpr (resizable<B>)
				{
					if (const size_t k = ix + 2 + 12 * value.size(); k > b.size())
						b.resize((std::max)(b.size() * 2, k));
				}
				ix += Tools::JsonText::WriteString(data_ptr(b) + ix, value);
			}
		};

		template <class V>
		struct to_json<Tools::JsonOmitEmpty<V>>
		{
			template <auto Opts>
			static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
			{
				write<json>::op<opt_false<Opts, &opts::write_unchecked>>(*value, ctx, args...);
			}
		};

		template <class V>
		struct from_json<Tools::JsonOmitEmpty<V>>
		{
			template <auto Opts>
			static void op(auto&& value, is_context auto&& ctx, auto&& it, auto&& end) noexcept
			{
				read<json>::op<Opts>(*value, ctx, it, end);
			}
		};
	}
}

namespace Tools
{
	class Json
	{
	public:
		// ---------------------------------------------------------------------
		// Serialization
		// ---------------------------------------------------------------------
		// Utf8JsonWriter's indented layout: 2 spaces, "key": value, one array element per line.
		template <typename T>
		static std::string Serialize(const T& obj)
		{
			static constexpr glz::opts writerOpts = glz::opts{
				.prettify = true,
				.indentation_width = 2,
			};

			std::string buffer;
			glz::error_ctx ec = glz::write<writerOpts>(obj, buffer);

			if (ec)
			{
				throw std::runtime_error("Serialization failed");
			}

			return buffer;
		}

		template <typename T>
		static std::string SerializeToLine(const T& obj)
		{
			static constexpr glz::opts writerOpts = glz::opts{
				.prettify = false,
			};

			std::string buffer;
			glz::error_ctx ec = glz::write<writerOpts>(obj, buffer);

			if (ec)
			{
				throw std::runtime_error("Serialization failed");
			}

			return buffer;
		}

		// ---------------------------------------------------------------------
		// Deserialization
		// ---------------------------------------------------------------------
		// Accepts // and /* */ comments and trailing commas, as the C# reader does.
		template <typename T>
		static T Deserialize(std::string_view jsonString)
		{
			T obj{};

			static constexpr glz::opts readerOpts = glz::opts{
				.comments = true,
				.error_on_unknown_keys = false,
			};

			std::string json = RemoveTrailingCommas(jsonString);
			glz::error_ctx ec = glz::read<readerOpts>(obj, json);

			if (ec)
			{
				throw std::runtime_error("Deserialization failed: " + glz::format_error(ec, json));
			}

			return obj;
		}

	private:
		// Index of the first character at or after i that is neither whitespace nor inside a comment.
		static size_t SkipWhitespaceAndComments(std::string_view json, size_t i)
		{
			while (i < json.size())
			{
				char c = json[i];
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
					i++;
				else if (c == '/' && i + 1 < json.size() && json[i + 1] == '/')
				{
					size_t end = json.find('\n', i + 2);
					i = end == std::string_view::npos ? json.size() : end + 1;
				}
				else if (c == '/' && i + 1 < json.size() && json[i + 1] == '*')
				{
					size_t end = json.find("*/", i + 2);
					i = end == std::string_view::npos ? json.size() : end + 2;
				}
				else
					break;
			}
			return i;
		}

		static std::string RemoveTrailingCommas(std::string_view json)
		{
			std::string result;
			result.reserve(json.size());
			size_t i = 0;
			while (i < json.size())
			{
				char c = json[i];
				if (c == '"')
				{
					size_t start = i++;
					while (i < json.size() && json[i] != '"')
						i += json[i] == '\\' ? 2 : 1;
					i = std::min(i + 1, json.size());
					result.append(json.substr(start, i - start));
				}
				else if (c == '/' && i + 1 < json.size() && (json[i + 1] == '/' || json[i + 1] == '*'))
				{
					size_t end = SkipWhitespaceAndComments(json, i);
					result.append(json.substr(i, end - i));
					i = end;
				}
				else
				{
					if (c == ',')
					{
						size_t next = SkipWhitespaceAndComments(json, i + 1);
						if (next < json.size() && (json[next] == '}' || json[next] == ']'))
						{
							i++;
							continue;
						}
					}
					result.push_back(c);
					i++;
				}
			}
			return result;
		}
	};
}
