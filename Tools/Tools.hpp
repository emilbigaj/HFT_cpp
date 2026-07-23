//BEGIN_FILE HFT/Tools/Tools.hpp
#pragma once

#include <cmath>
#include <csignal>
#include <future>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>
#include <fstream>
#include <optional> // Required for std::optional in C++20
#include <algorithm>

#include <typeinfo>
#include <cxxabi.h>
#include <cstdlib>
#include <unistd.h>

#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define NEVER_INLINE __attribute__((noinline))
#define COLD __attribute__((cold))
#define NORETURN __attribute__((noreturn))

namespace Tools
{
    inline void Join(std::thread& thread, std::chrono::milliseconds timeout)
    {
        if (thread.joinable())
        {
            auto future = std::async(std::launch::async, [&thread]() { thread.join(); });
            if (future.wait_for(timeout) == std::future_status::timeout)
            {
                thread.detach();
            }
        }
    }

    inline static std::string Sanitize(const std::string& input)
	{
		std::string result = input;
		for (char& c : result)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
				continue;
			c = '_';
		}
		return result;
	}



    inline std::string GetTypeName(const std::type_info& info)
    {
        int status = -1;
        const char* mangledName = info.name();

        char* demangled = abi::__cxa_demangle(mangledName, nullptr, nullptr, &status);

        if (status == 0 && demangled != nullptr)
        {
            std::string fullName(demangled);
            std::free(demangled);

            size_t lastScope = fullName.find_last_of("::");
            if (lastScope != std::string::npos)
            {
                return fullName.substr(lastScope + 1);
            }

            return fullName;
        }

        return std::string(mangledName);
    }

    template <typename T>
    std::string GetTypeName()
    {
        return GetTypeName(typeid(T));
    }


    struct Nanouble {
        double value;
        
        Nanouble() : value(0.0) {}
        Nanouble(double v) : value(v) {} // Allows transparent assignment
        
        operator double() const { return value; } // Allows transparent reading/math
        Nanouble& operator=(double v) { value = v; return *this; }
    };


	inline bool IsProcessAlive(int32_t pid)
	{
		if (pid <= 0)
			return false;
		if (kill(pid, 0) == 0)
			return true;
		return errno == EPERM;
	}

	enum class Access
	{
		Read,
		Write
	};

	template<typename T>
	concept ByteEnum = std::is_enum_v<T> && sizeof(std::underlying_type_t<T>) == 1;

	template<typename T>
	concept IntEnum = std::is_enum_v<T> && sizeof(std::underlying_type_t<T>) == 4;

	template <typename T>
	concept PlainOldData = std::is_trivially_copyable_v<T> && 
						   std::is_standard_layout_v<T> &&
						   !std::is_pointer_v<std::decay_t<T>> &&
						   !requires(std::decay_t<T>& t) { []<typename E, std::size_t S>(std::span<E, S>&){}(t); } &&
						   !requires(std::decay_t<T>& t) { []<typename C, typename Tr>(std::basic_string_view<C, Tr>&){}(t); };

	ALWAYS_INLINE void Print(const std::string &message)
	{
		std::cout << message;
	}

	ALWAYS_INLINE void PrintLine(const std::string &message)
	{
		std::cout << message << std::endl;
	}

	ALWAYS_INLINE static int RoundToInt(double value)
	{
		return static_cast<int>(value + (value >= 0.0 ? 0.5 : -0.5));
	}
	
	ALWAYS_INLINE static int FloorToInt(double value)
	{
		return static_cast<int>(std::floor(value));
	}
	
	ALWAYS_INLINE static int CeilingToInt(double value)
	{
		return static_cast<int>(std::ceil(value));
	}

	ALWAYS_INLINE static int32_t GetNumberOfDecimalPlaces(double number)
	{
		number = std::abs(number);
		number -= std::trunc(number);
		int32_t decimalPlaces = 0;
		while (number > 1e-9 && decimalPlaces < 10) 
		{
			decimalPlaces++;
			number *= 10.0;
			number -= std::trunc(number);
		}
		return decimalPlaces;
	}

	ALWAYS_INLINE static std::optional<std::string> ReadLastLine(const std::string& filePath)
	{
		std::ifstream stream(filePath, std::ios::in | std::ios::binary | std::ios::ate);
		if (!stream.is_open())
			return std::nullopt;

		const std::streamoff length = stream.tellg();
		if (length <= 0)
			return std::nullopt;

		auto hasNonWhitespace = [](const std::string& s) -> bool
		{
			for (char c : s)
				if (!std::isspace(static_cast<unsigned char>(c)))
					return true;
			return false;
		};

		auto finalize = [](std::string& reversed) -> std::string
		{
			std::reverse(reversed.begin(), reversed.end());
			if (!reversed.empty() && reversed.back() == '\r')
				reversed.pop_back();
			return std::move(reversed);
		};

		constexpr std::streamsize CHUNK_SIZE = 4096;
		char buffer[CHUNK_SIZE];
		std::string candidate;          // accumulates current line in REVERSE file order
		std::streamoff pos = length;

		while (pos > 0)
		{
			const std::streamsize readSize =
				static_cast<std::streamsize>(std::min<std::streamoff>(CHUNK_SIZE, pos));
			pos -= readSize;

			stream.seekg(pos, std::ios::beg);
			stream.read(buffer, readSize);

			for (std::streamsize i = readSize - 1; i >= 0; --i)
			{
				const char c = buffer[i];
				if (c == '\n')
				{
					if (hasNonWhitespace(candidate))
						return finalize(candidate);
					candidate.clear();                    // blank line, keep scanning back
				}
				else
				{
					candidate.push_back(c);               // O(1) — reversed-order accumulation
				}
			}
		}

		// Reached start of file. `candidate` holds the first line (still reversed).
		if (hasNonWhitespace(candidate))
			return finalize(candidate);

		return std::nullopt;
	}

	// Host (machine) name (C# Platform.Name). Linux-only; resolved once and cached.
	inline const std::string& MachineName()
	{
		static const std::string name = []
		{
			char buffer[256] = {};
			return gethostname(buffer, sizeof(buffer) - 1) == 0 ? std::string(buffer) : std::string("unknown");
		}();
		return name;
	}
}
//END_FILE HFT/Tools/Tools.hpp