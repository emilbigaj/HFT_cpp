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
		return weight == 1 ? "+" : weight == -1 ? "-" : weight > 1 ? "+" + std::to_string(weight) : std::to_string(weight);
	}

	static std::string GetLegsTicker(const std::string& root, const std::vector<SymbolLeg>& legs)
	{
		std::string result = root + " ";
		for (size_t i = 0; i < legs.size(); i++)
			result += GetSignedWeight(legs[i].Weight) + legs[i].Symbol + (i + 1 < legs.size() ? " " : "");
		return result;
	}

	// C#'s l.Ticker.Replace(l.Root + " ", ""): leg tokens carry only sign+maturity.
	static std::vector<SymbolLeg> ToLegs(const std::vector<std::unique_ptr<Symbology>>& symbologies, const std::vector<int32_t>& weights, bool shortForm)
	{
		std::vector<SymbolLeg> legs;
		for (size_t i = 0; i < symbologies.size(); i++)
		{
			std::string text = shortForm ? symbologies[i]->ShortSymbol() : symbologies[i]->Ticker();
			text.erase(text.find(symbologies[i]->Root() + " "), symbologies[i]->Root().length() + 1);
			legs.push_back({ text, weights[i] });
		}
		return legs;
	}

public:
	LeggedSymbology(Data::InstrumentType instrumentType, const std::string& exchange, const std::string& root, std::vector<std::unique_ptr<Symbology>> symbologies, std::vector<int32_t> weights)
	: Symbology(instrumentType, exchange, root, GetLegsTicker(root, ToLegs(symbologies, weights, false)))
	{
		_shortSymbol = GetLegsTicker(root, ToLegs(symbologies, weights, true));
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
		return std::make_unique<FutureSymbology>(exchange, root, Tools::Timestamp::FromString(remainder, "%Y-%m-%d"));
	}
	else if (instrumentType == Data::InstrumentType::Spread)
	{
		// Signed leg tokens "±[n]<Date>": root appears once, legs maturity-ascending. Parsed
		// blindly assuming the format is correct - the ISO date is the fixed-width (10) END of
		// the token, the digits between the sign and the date are the optional weight magnitude
		// ("+22026-07-31" = weight 2); anything malformed throws on its own.
		std::vector<std::unique_ptr<Symbology>> symbologies;
		std::vector<int32_t> weights;
		std::size_t tokenStart = 0;
		while (tokenStart < remainder.length())
		{
			std::size_t tokenEnd = std::min(remainder.find(' ', tokenStart), remainder.length());
			std::string legToken = remainder.substr(tokenStart, tokenEnd - tokenStart);
			tokenStart = tokenEnd + 1;
			if (legToken.empty()) continue;

			int32_t sign = legToken[0] == '-' ? -1 : 1;
			int32_t magnitude = 0;
			for (int32_t index = 1; index < static_cast<int32_t>(legToken.length()) - 10; index++)
				magnitude = magnitude * 10 + (legToken[static_cast<size_t>(index)] - '0');

			symbologies.push_back(std::make_unique<FutureSymbology>(exchange, root, Tools::Timestamp::FromString(legToken.substr(legToken.length() - 10), "%Y-%m-%d")));
			weights.push_back(sign * std::max(magnitude, 1));
		}
		return std::make_unique<SpreadSymbology>(exchange, root, std::move(symbologies), std::move(weights));
	}

	throw std::logic_error("FromString does not yet support this InstrumentType.");
}

}