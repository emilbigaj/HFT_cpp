//BEGIN_FILE HFT/Execution/RateLimit.hpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Json.hpp"
#include "Timestamp.hpp"

namespace Execution
{
#pragma pack(push, 1)
	struct RateLimit
	{
		Tools::Duration Duration;
		int32_t Limit;

		RateLimit() = default;
		RateLimit(Tools::Duration duration, int32_t limit) : Duration(duration), Limit(limit) {}

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}

		struct glaze
		{
			using T = RateLimit;
			static constexpr auto value = glz::object(
				"Duration", &T::Duration,
				"Limit", &T::Limit
			);
		};
	};
#pragma pack(pop)

	class SessionRateLimit
	{
	private:
		int32_t _limit;
		int32_t _ordersSentToday;

	public:
		explicit SessionRateLimit(int32_t limit) : _limit(limit), _ordersSentToday(0) {}

		bool CanSendOrder(Tools::Timestamp /*timestamp*/) const
		{
			return _ordersSentToday < _limit;
		}

		bool TrySendOrder(Tools::Timestamp timestamp)
		{
			if (!CanSendOrder(timestamp))
				return false;

			_ordersSentToday++;
			return true;
		}

		void Reset()
		{
			_ordersSentToday = 0;
		}
	};

	class RollingRateLimit
	{
	private:
		RateLimit _rateLimit;
		std::vector<Tools::Timestamp> _timestamps;
		int32_t _current;

	public:
		explicit RollingRateLimit(RateLimit rateLimit)
			: _rateLimit(rateLimit),
			  _timestamps(static_cast<size_t>(rateLimit.Limit)),
			  _current(0)
		{
		}

		bool CanSendOrder(Tools::Timestamp timestamp) const
		{
			Tools::Timestamp oldestTimestamp = _timestamps[static_cast<size_t>(_current)];
			return (timestamp - oldestTimestamp) >= _rateLimit.Duration;
		}

		bool TrySendOrder(Tools::Timestamp timestamp)
		{
			if (!CanSendOrder(timestamp))
				return false;

			_timestamps[static_cast<size_t>(_current)] = timestamp;

			int32_t next = _current + 1;
			_current = next < _rateLimit.Limit ? next : 0;

			return true;
		}
	};
}
//END_FILE HFT/Execution/RateLimit.hpp
