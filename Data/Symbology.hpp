#include <string>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <chrono>

#include "Timestamp.hpp"

namespace Data
{

enum MaturityType : char
{
	Day = 'D',
	Week = 'W',
	Month = 'M',
	Quarter = 'Q',
	Year = 'Y'
};

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

static std::string TrimString(const std::string& text)
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

// Abbreviated "<Mon> <Year>" for a contract maturity, e.g. "Jun 2025". Locale-free (fixed table)
// so it matches the C# ShortSymbol exactly (CultureInfo.InvariantCulture abbreviated month).
static std::string ShortMonthYear(Tools::Timestamp date)
{
	using namespace std::chrono;
	year_month_day ymd{floor<days>(sys_time<nanoseconds>{nanoseconds(date.NanosSinceEpoch)})};
	static constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	unsigned month = static_cast<unsigned>(ymd.month());
	int year = static_cast<int>(ymd.year());
	return std::string(kMonths[month - 1]) + " " + std::to_string(year);
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
	static void ParseMaturityToken(const std::string& token, MaturityType& maturityType, Tools::Timestamp& maturityDate)
	{
		if (IsStringNullOrWhiteSpace(token) || token.length() < 2)
			throw std::invalid_argument("Maturity token must start with a letter and include a date, e.g., M20251215.");

		char typeChar = token[0];
		maturityType = static_cast<MaturityType>(typeChar);

		std::string dateText = token.substr(1);

		try
		{
			maturityDate = Tools::Timestamp::FromString(dateText, "%Y-%m-%d");
		}
		catch (const std::exception& ex)
		{
			throw std::invalid_argument("Invalid maturity date: \"" + dateText + "\".");
		}
	}
};

class FutureSymbology : public Symbology
{
protected:
	Data::MaturityType _maturityType;
	Tools::Timestamp _maturityDate;

public:
	FutureSymbology(const std::string& exchange, const std::string& root, MaturityType maturityType, Tools::Timestamp maturityDate) : FutureSymbology(Data::InstrumentType::Future, exchange, root, root + " " + static_cast<char>(maturityType) + maturityDate.ToDateString(), maturityType, maturityDate)
	{
	}

protected:
	FutureSymbology(Data::InstrumentType instrumentType, const std::string& exchange, const std::string& root, const std::string& ticker, MaturityType maturityType, Tools::Timestamp maturityDate) : Symbology(instrumentType, exchange, root, ticker), _maturityType(maturityType), _maturityDate(maturityDate)
	{
		_shortSymbol = root + " " + ShortMonthYear(maturityDate);
	}

public:
	Data::MaturityType MaturityType() const
	{
		return _maturityType;
	}

	Tools::Timestamp MaturityDate() const
	{
		return _maturityDate;
	}
};

class SpreadSymbology final : public FutureSymbology
{
private:
	std::unique_ptr<FutureSymbology> _longSymbology;
	std::unique_ptr<FutureSymbology> _shortSymbology;

public:
	SpreadSymbology(const std::string& exchange, const std::string& root, Data::MaturityType longMaturityType, Tools::Timestamp longMaturityDate, Data::MaturityType shortMaturityType, Tools::Timestamp shortMaturityDate) : FutureSymbology(Data::InstrumentType::Spread, exchange, root, root + " " + static_cast<char>(longMaturityType) + longMaturityDate.ToDateString() + " - " + static_cast<char>(shortMaturityType) + shortMaturityDate.ToDateString(), (longMaturityDate <= shortMaturityDate) ? longMaturityType : shortMaturityType, (longMaturityDate <= shortMaturityDate) ? longMaturityDate : shortMaturityDate), _longSymbology(std::make_unique<FutureSymbology>(exchange, root, longMaturityType, longMaturityDate)), _shortSymbology(std::make_unique<FutureSymbology>(exchange, root, shortMaturityType, shortMaturityDate))
	{
		_shortSymbol = root + " " + ShortMonthYear(longMaturityDate) + " - " + ShortMonthYear(shortMaturityDate);
	}

	FutureSymbology& LongSymbology() const
	{
		return *_longSymbology;
	}

	FutureSymbology& ShortSymbology() const
	{
		return *_shortSymbology;
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
		Data::MaturityType maturityType;
		Tools::Timestamp maturityDate;
		
		ParseMaturityToken(remainder, maturityType, maturityDate);

		return std::make_unique<FutureSymbology>(exchange, root, maturityType, maturityDate);
	}
	else if (instrumentType == Data::InstrumentType::Spread)
	{
		std::size_t separatorPos = remainder.find(" - ");
		
		if (separatorPos == std::string::npos)
			throw std::invalid_argument("Spread ticker must be in the form \"<E><Date> - <E><Date>\".");

		std::string longLeg = TrimString(remainder.substr(0, separatorPos));
		std::string shortLeg = TrimString(remainder.substr(separatorPos + 3));

		Data::MaturityType longMaturityType;
		Tools::Timestamp longMaturityDate;
		
		ParseMaturityToken(longLeg, longMaturityType, longMaturityDate);

		Data::MaturityType shortMaturityType;
		Tools::Timestamp shortMaturityDate;
		
		ParseMaturityToken(shortLeg, shortMaturityType, shortMaturityDate);

		return std::make_unique<SpreadSymbology>(exchange, root, longMaturityType, longMaturityDate, shortMaturityType, shortMaturityDate);
	}

	throw std::logic_error("FromString does not yet support this InstrumentType.");
}

}