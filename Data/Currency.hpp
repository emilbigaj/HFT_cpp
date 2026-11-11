#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <magic_enum.hpp>

#include "Json.hpp"
#include "Timestamp.hpp"

namespace Data
{
	// What a contract pays in. The value goes into every instrument header, so it is part of the shared-memory contract:
	// rename freely, never renumber. USD is 0 so an unstamped header reads dollars, which is what every header held before
	// currencies existed, and so what an unported CmeServer keeps producing (see Spec.md).
	enum class Currency : uint8_t
	{
		USD = 0,
		USc = 1,   // US cents: the grains quote in them, so the rate is a flat 0.01 and never an FX rate
		JPY = 2,
		EUR = 3,
		CNH = 4,
		SEK = 5,
		NOK = 6,
	};

	// One day's dollars for one unit of a currency, a line in <server>/ExchangeRates/<currency>.exchangerate.
	struct ExchangeRate
	{
		Data::Currency Currency = Data::Currency::USD;
		Tools::Timestamp Timestamp = Tools::Timestamp::MinValue;
		double UsdRate = 0;

		std::string ToString() const
		{
			return Tools::Json::SerializeToLine(*this);
		}

		struct glaze
		{
			using T = ExchangeRate;
			static constexpr auto value = glz::object(
				"Currency", &T::Currency,
				"Timestamp", &T::Timestamp,
				"UsdRate", &T::UsdRate
			);
		};
	};

	// The Currency enum's shared-memory side: how many rate rows there are, and how a catalog string and a file become one.
	struct Currencies
	{
		// The rate array's size, not the enum's count: a currency added later must not change the Mirror's layout hash and
		// make every desk reinstall the same day (see Spec.md).
		static constexpr int32_t s_capacity = 16;

		// The catalog's Currency string, which is the enum's own name. Throws on an unknown one: falling back to USD would
		// misprice a whole product silently. Startup only.
		static Data::Currency FromString(const std::string& currency)
		{
			std::optional<Data::Currency> parsedCurrency = magic_enum::enum_cast<Data::Currency>(currency);
			if (!parsedCurrency)
				throw std::out_of_range("Currencies::FromString: '" + currency + "' is not a known currency; add it to the Currency enum and give it an .exchangerate file");
			return *parsedCurrency;
		}
	};
}
