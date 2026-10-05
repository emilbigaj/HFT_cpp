#include <iostream>
#include <cassert>
#include "Timestamp.hpp"
#include "Bitset.hpp"
#include "Json.hpp"
#include "String.hpp"

int main()
{
    auto now = Tools::Timestamp::UtcNow();
    std::cout << "Time is: " << now.ToString() << std::endl;

    // Bitset64 is a bare JSON number, as in C#.
    if (Tools::Json::SerializeToLine(Tools::Bitset64(7)) != "7" || Tools::Json::Deserialize<Tools::Bitset64>("7").Raw() != 7)
    {
        std::cout << "FAIL: Bitset64 JSON" << std::endl;
        return 1;
    }

    // Short formats after the caller's format fails; whitespace trimmed.
    Tools::Timestamp full = Tools::Timestamp::FromString("2026-10-04 09:30:00.123_456_789");
    Tools::Timestamp minute = Tools::Timestamp::FromString(" 2026-10-04 09:30 ");
    Tools::Timestamp date = Tools::Timestamp::FromString("2026-10-04");
    if (full.ToString() != "2026-10-04 09:30:00.123_456_789" || minute.ToString() != "2026-10-04 09:30:00.000_000_000" || date.ToString() != "2026-10-04 00:00:00.000_000_000")
    {
        std::cout << "FAIL: Timestamp FromString " << full.ToString() << " | " << minute.ToString() << " | " << date.ToString() << std::endl;
        return 1;
    }
    bool threw = false;
    try { Tools::Timestamp::FromString("2026-10-04 junk"); } catch (const std::exception&) { threw = true; }
    if (!threw)
    {
        std::cout << "FAIL: Timestamp FromString accepted trailing junk" << std::endl;
        return 1;
    }

    // Comments and trailing commas, as the C# reader accepts.
    std::vector<int> values = Tools::Json::Deserialize<std::vector<int>>("[1, /* c, */ 2, // x\n 3, ]");
    if (values != std::vector<int>{1, 2, 3} || Tools::Json::Deserialize<std::string>("\"a, ]\"") != "a, ]")
    {
        std::cout << "FAIL: Json comments / trailing commas" << std::endl;
        return 1;
    }

    threw = false;
    try { Tools::String4 s("abcde"); } catch (const std::exception&) { threw = true; }
    if (!threw)
    {
        std::cout << "FAIL: String4 accepted 5 chars" << std::endl;
        return 1;
    }

    std::cout << "ToolsTests passed." << std::endl;
    return 0;
}