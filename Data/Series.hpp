//BEGIN_FILE HFT/Data/Series.hpp
#pragma once

#include "Timestamp.hpp"
#include "Json.hpp"
#include <string>
#include <cstdint>

namespace Data
{
	enum class FileType : uint8_t
	{
		Log = 0,
		Audit = 1,
		Fill = 2,
		Position = 3,
		MarketByPrice = 4,
		Trade = 5,
		Point = 6,
		Pair = 7,
		Candle = 8,
		Histogram = 9,
		Alert = 10,
		Factor = 11,
		Mean = 12,
		StdDev = 13,
	};

	enum class Frequency : int32_t
	{
		Tick = 0,
		MS = 1,
		MS10 = 10,
		MS100 = 100,
		Second = 1000,
		Minute = 60000,
	};

	struct Point
	{
		Tools::Timestamp Timestamp;
		double Value;

		Point(Tools::Timestamp timestamp, double value) : Timestamp(timestamp), Value(value)
		{
		}

		Point() = default;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = Point;
			static constexpr auto value = glz::object(
				"Timestamp", &T::Timestamp,
				"Value", &T::Value
			);
		};
	};

	struct Pair
	{
		Tools::Timestamp Timestamp;
		double First;
		double Second;

		Pair(Tools::Timestamp timestamp, double first, double second) : Timestamp(timestamp), First(first), Second(second)
		{
		}

		Pair() = default;

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = Pair;
			static constexpr auto value = glz::object(
				"Timestamp", &T::Timestamp,
				"First", &T::First,
				"Second", &T::Second
			);
		};
	};
}