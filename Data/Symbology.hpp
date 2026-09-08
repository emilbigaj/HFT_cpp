#include <string>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <chrono>

#include "Timestamp.hpp"

namespace Data
{

// MaturityType is DELETED (C# 2026-09-08): the type letter in symbols broke lexical-order ==
// maturity-order (every M-file sorted before any Q-file), and MaturityDate alone identifies a
// contract - verified against all 187,087 catalog files with zero collisions.

enum class InstrumentType : uint8_t
{
    Instrument = 50,
    Future = 51,
    Option = 52,
    Swap = 53,
    Stock = 54,
    Spread = 55,
    Forex = 56,
};

static bool IsStringNullOrWhiteSpace(const std::string& str)
{
	if (str.empty())
		return true;

	for (char c : str)
	{
		if (!std::isspace(static_cast<unsigned char>(c)))
			return false;
	}

	return true;
}

[[maybe_unused]] static std::string TrimString(const std::string& text)
{
	std::size_t first = text.find_first_not_of(" \t\n\r");
	
	if (first == std::string::npos)
		return "";

	std::size_t last = text.find_last_not_of(" \t\n\r");
	
	return text.substr(first, (last - first + 1));
}

static std::string InstrumentTypeToString(InstrumentType type)
{
	switch (type)
	{
		case InstrumentType::Future:
			return "Future";
		case InstrumentType::Option:
			return "Option";
		case InstrumentType::Swap:
			return "Swap";
		case InstrumentType::Stock:
			return "Stock";
		case InstrumentType::Spread:
			return "Spread";
		case InstrumentType::Forex:
			return "Forex";
		default:
			return "Unknown";
	}
}

static InstrumentType ParseInstrumentType(const std::string& text)
{
	std::string lowerText = text;
	std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c) -> unsigned char { return static_cast<unsigned char>(std::tolower(c)); });

	if (lowerText == "future")
		return InstrumentType::Future;

	if (lowerText == "option")
		return InstrumentType::Option;

	if (lowerText == "swap")
		return InstrumentType::Swap;

	if (lowerText == "stock")
		return InstrumentType::Stock;

	if (lowerText == "spread")
		return InstrumentType::Spread;

	if (lowerText == "forex")
		return InstrumentType::Forex;

	throw std::invalid_argument("Invalid InstrumentType");
}

// Abbreviated "<Mon><yy>" for a contract maturity, e.g. "Jun25". Locale-free (fixed table) so it
// matches the C# ShortSymbol exactly (InvariantCulture abbreviated month + Year % 100).
static std::string ShortMonthYear(Tools::Timestamp date)
{
	using namespace std::chrono;
	year_month_day ymd{floor<days>(sys_time<nanoseconds>{nanoseconds(date.NanosSinceEpoch)})};
	static constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	unsigned month = static_cast<unsigned>(ymd.month());
	int year = static_cast<int>(ymd.year());
	return std::string(kMonths[month - 1]) + std::to_string(year % 100);
}

class Symbology
{
private:
	Data::InstrumentType _instrumentType;
	std::string _exchange;
	std::string _root;
	std::string _ticker;
	std::string _symbol;   // unique, used for lookups: "{Type} {Exchange} {Ticker}"
	std::string _product;  // not unique: "{Type} {Exchange} {Root}"

protected:
	std::string _shortSymbol; // not unique; default = ticker, overridden by Future/Spread

	Symbology(Data::InstrumentType instrumentType, const std::string& exchange, const std::string& root, const std::string& ticker) : _instrumentType(instrumentType), _exchange(exchange), _root(root), _ticker(ticker), _symbol(InstrumentTypeToString(instrumentType) + " " + exchange + " " + ticker), _product(InstrumentTypeToString(instrumentType) + " " + exchange + " " + root), _shortSymbol(ticker)
	{
		if (IsStringNullOrWhiteSpace(exchange))
			throw std::invalid_argument("exchange is required");

		if (IsStringNullOrWhiteSpace(root))
			throw std::invalid_argument("root is required");

		if (IsStringNullOrWhiteSpace(ticker))
			throw std::invalid_argument("ticker is required");
	}

public:
	virtual ~Symbology() = default;

	Data::InstrumentType InstrumentType() const
	{
		return _instrumentType;
	}

	std::string Exchange() const
	{
		return _exchange;
	}

	std::string Root() const
	{
		return _root;
	}

	std::string Ticker() const
	{
		return _ticker;
	}

	std::string Symbol() const
	{
		return _symbol;
	}

	std::string Product() const
	{
		return _product;
	}

	std::string ShortSymbol() const
	{
		return _shortSymbol;
	}

	static std::unique_ptr<Symbology> FromString(const std::string& symbol);

	virtual std::string ToString() const
	{
		return _symbol;
	}

protected:
	// Token is a bare date, "2025-12-15", parsed blindly: anything else - a legacy maturity-type
	// letter included - fails the date parse loudly on its own. No tolerance, migrate the catalog.
	static Tools::Timestamp ParseMaturityToken(const std::string& token)
	{
		return Tools::Timestamp::FromString(token, "%Y-%m-%d");
	}
};

class FutureSymbology : public Symbology
{
protected:
	Tools::Timestamp _maturityDate;

public:
	// The ticker leads with the bare ISO date so names sort lexically == chronologically.
	FutureSymbology(const std::string& exchange, const std::string& root, Tools::Timestamp maturityDate) : FutureSymbology(Data::InstrumentType::Future, exchange, root, root + " " + maturityDate.ToDateString(), maturityDate)
	{
	}

protected:
	FutureSymbology(Data::InstrumentType instrumentType, const std::string& exchange, const std::string& root, const std::string& ticker, Tools::Timestamp maturityDate) : Symbology(instrumentType, exchange, root, ticker), _maturityDate(maturityDate)
	{
		_shortSymbol = root + " " + ShortMonthYear(maturityDate);
	}

public:
	Tools::Timestamp MaturityDate() const
	{
		return _maturityDate;
	}
};

// Legged ticker: the root appears once, up front; leg tokens carry only sign+maturity, e.g.
// "ES +M2025-12-15 -M2026-03-15". This string names shared-memory rings and .risklimit files,
// so it must match the C# LeggedSymbology byte-for-byte.
class LeggedSymbology : public Symbology
{
protected:
	std::vector<std::unique_ptr<Symbology>> _symbologies;
	std::vector<int32_t> _weights;

	struct SymbolLeg { std::string Symbol; int32_t Weight; };

	// Weight rendered as the leg-token sign: "+", "-", "+2"; negative weights carry their own '-'.
	static std::string GetSignedWeight(int32_t weight)
	{
		if (weight == 1) return "+";
		if (weight == -1) return "-";
		if (weight > 1) return "+" + std::to_string(weight);
		return std::to_string(weight); // 0 renders as "0"
	}

	static std::string GetLegsTicker(const std::string& root, const std::vector<SymbolLeg>& legs)
	{
		std::string result = root + " ";
		for (size_t i = 0; i < legs.size(); i++)
		{
			result += GetSignedWeight(legs[i].Weight) + legs[i].Symbol;
			if (i + 1 < legs.size())
				result += " ";
		}
		return result;
	}

	// C#'s l.Ticker.Replace(l.Root + " ", ""): strip the leg's own root prefix from a leg string.
	static std::string StripRoot(const std::string& text, const std::string& root)
	{
		std::string prefix = root + " ";
		std::string result = text;
		size_t pos;
		while ((pos = result.find(prefix)) != std::string::npos)
			result.erase(pos, prefix.length());
		return result;
	}

	static std::vector<SymbolLeg> TickerLegs(const std::vector<std::unique_ptr<Symbology>>& symbologies, const std::vector<int32_t>& weights)
	{
		std::vector<SymbolLeg> legs;
		legs.reserve(symbologies.size());
		for (size_t i = 0; i < symbologies.size(); i++)
			legs.push_back({ StripRoot(symbologies[i]->Ticker(), symbologies[i]->Root()), weights[i] });
		return legs;
	}

	static std::vector<SymbolLeg> ShortSymbolLegs(const std::vector<std::unique_ptr<Symbology>>& symbologies, const std::vector<int32_t>& weights)
	{
		std::vector<SymbolLeg> legs;
		legs.reserve(symbologies.size());
		for (size_t i = 0; i < symbologies.size(); i++)
			legs.push_back({ StripRoot(symbologies[i]->ShortSymbol(), symbologies[i]->Root()), weights[i] });
		return legs;
	}

public:
	LeggedSymbology(Data::InstrumentType instrumentType, const std::string& exchange, const std::string& root, std::vector<std::unique_ptr<Symbology>> symbologies, std::vector<int32_t> weights)
	: Symbology(instrumentType, exchange, root, GetLegsTicker(root, TickerLegs(symbologies, weights)))
	{
		_shortSymbol = GetLegsTicker(root, ShortSymbolLegs(symbologies, weights));
		_symbologies = std::move(symbologies);
		_weights = std::move(weights);
	}

	const std::vector<std::unique_ptr<Symbology>>& Symbologies() const { return _symbologies; }
	const std::vector<int32_t>& Weights() const { return _weights; }
};

class SpreadSymbology final : public LeggedSymbology
{
public:
	SpreadSymbology(const std::string& exchange, const std::string& root, std::vector<std::unique_ptr<Symbology>> symbologies, std::vector<int32_t> weights)
	: LeggedSymbology(Data::InstrumentType::Spread, exchange, root, std::move(symbologies), std::move(weights))
	{
	}
};

inline std::unique_ptr<Symbology> Symbology::FromString(const std::string& symbol)
{
	if (IsStringNullOrWhiteSpace(symbol))
		throw std::invalid_argument("symbol is required");

	std::size_t firstSpace = symbol.find(' ');
	std::size_t secondSpace = (firstSpace == std::string::npos) ? std::string::npos : symbol.find(' ', firstSpace + 1);

	if (firstSpace == std::string::npos || secondSpace == std::string::npos)
		throw std::invalid_argument("Expected format: \"InstrumentType Exchange Ticker\".");

	std::string instrumentTypeText = symbol.substr(0, firstSpace);
	std::string exchange = symbol.substr(firstSpace + 1, secondSpace - firstSpace - 1);
	std::string ticker = symbol.substr(secondSpace + 1);

	Data::InstrumentType instrumentType = ParseInstrumentType(instrumentTypeText);

	std::size_t spaceAfterRoot = ticker.find(' ');
	
	if (spaceAfterRoot == std::string::npos)
		throw std::invalid_argument("Ticker must contain root and a maturity part.");

	std::string root = ticker.substr(0, spaceAfterRoot);
	std::string remainder = ticker.substr(spaceAfterRoot + 1);

	if (instrumentType == Data::InstrumentType::Future)
	{
		return std::make_unique<FutureSymbology>(exchange, root, ParseMaturityToken(remainder));
	}
	else if (instrumentType == Data::InstrumentType::Spread)
	{
		// Signed leg tokens "±[n]<Date>": root appears once, legs maturity-ascending.
		std::vector<std::unique_ptr<Symbology>> symbologies;
		std::vector<int32_t> weights;
		std::size_t tokenStart = 0;
		while (tokenStart < remainder.length())
		{
			std::size_t tokenEnd = remainder.find(' ', tokenStart);
			if (tokenEnd == std::string::npos) tokenEnd = remainder.length();
			std::string legToken = remainder.substr(tokenStart, tokenEnd - tokenStart);
			tokenStart = tokenEnd + 1;
			if (legToken.empty()) continue;

			int32_t sign = legToken[0] == '+' ? 1 : legToken[0] == '-' ? -1 : throw std::invalid_argument("Spread leg \"" + legToken + "\" must start with '+' or '-'.");

			// The ISO date is fixed-width (10) at the token's END; the digits between the sign
			// and the date are the optional weight magnitude ("+22026-07-31" = weight 2).
			// Fixed-width is what keeps the grammar unambiguous with no maturity letter
			// separating magnitude from date - a left-to-right digit scan eats the year as the
			// weight (this bug shipped in C# and was caught).
			if (legToken.length() < 11)
				throw std::invalid_argument("Spread leg \"" + legToken + "\" must end with a yyyy-MM-dd date.");
			std::string dateText = legToken.substr(legToken.length() - 10);
			int32_t magnitude = 0;
			for (size_t index = 1; index < legToken.length() - 10; index++)
			{
				if (legToken[index] >= '0' && legToken[index] <= '9')
					magnitude = magnitude * 10 + (legToken[index] - '0');
			}

			symbologies.push_back(std::make_unique<FutureSymbology>(exchange, root, ParseMaturityToken(dateText)));
			weights.push_back(sign * std::max(magnitude, 1));
		}
		return std::make_unique<SpreadSymbology>(exchange, root, std::move(symbologies), std::move(weights));
	}

	throw std::logic_error("FromString does not yet support this InstrumentType.");
}

}