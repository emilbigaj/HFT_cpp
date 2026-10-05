//BEGIN_FILE HFT/Execution/RateLimit.hpp
#pragma once

#include <cstdint>
#include <string>

#include "Json.hpp"
#include "Timestamp.hpp"

namespace Execution
{
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
}
//END_FILE HFT/Execution/RateLimit.hpp
