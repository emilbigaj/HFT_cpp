#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <glaze/glaze.hpp>

#include "Timestamp.hpp"

namespace Tools
{
    // Wire-compatible with .NET System.TimeSpan: 8 bytes of 100 ns ticks. Only what C++ uses of it is here.
    // JSON is "hh:mm:ss", as the C# TimeSpanJsonConverter writes and reads it.
    struct TimeSpan
    {
        static constexpr int64_t TicksPerSecond = 10'000'000;
        static constexpr int64_t TicksPerMinute = 60 * TicksPerSecond;
        static constexpr int64_t TicksPerHour = 60 * TicksPerMinute;
        static constexpr int64_t TicksPerDay = 24 * TicksPerHour;

        int64_t Ticks = 0;

        static TimeSpan FromHours(int64_t hours) { return TimeSpan{ hours * TicksPerHour }; }

        std::string ToString() const
        {
            int64_t ticks = Ticks < 0 ? -Ticks : Ticks;
            char text[16];
            std::snprintf(text, sizeof(text), "%02d:%02d:%02d", static_cast<int>((ticks / TicksPerHour) % 24), static_cast<int>((ticks / TicksPerMinute) % 60), static_cast<int>((ticks / TicksPerSecond) % 60));
            return text;
        }

        // .NET TimeSpan.ParseExact(text, @"hh\:mm\:ss"): two digits each, hours 0-23, minutes and seconds 0-59.
        static TimeSpan FromString(const std::string& text)
        {
            auto digit = [&](size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
            auto twoDigits = [&](size_t i) { return (text[i] - '0') * 10 + (text[i + 1] - '0'); };
            if (text.size() != 8 || !digit(0) || !digit(1) || text[2] != ':' || !digit(3) || !digit(4) || text[5] != ':' || !digit(6) || !digit(7))
                throw std::runtime_error("TimeSpan parse error: '" + text + "' does not match format 'hh:mm:ss'");
            int hours = twoDigits(0), minutes = twoDigits(3), seconds = twoDigits(6);
            if (hours > 23 || minutes > 59 || seconds > 59)
                throw std::runtime_error("TimeSpan parse error: '" + text + "' is out of range");
            return TimeSpan{ hours * TicksPerHour + minutes * TicksPerMinute + seconds * TicksPerSecond };
        }

        void Set(const std::string& text) { *this = FromString(text); }

        friend bool operator==(TimeSpan a, TimeSpan b) { return a.Ticks == b.Ticks; }

        struct glaze
        {
            static constexpr auto value = glz::custom<&TimeSpan::Set, &TimeSpan::ToString>;
        };
    };
    static_assert(sizeof(TimeSpan) == 8);

    enum class DateTimeKind : uint8_t
    {
        Unspecified = 0,
        Utc = 1,
        Local = 2,
    };

    // Wire-compatible with .NET System.DateTime: 8 bytes, 100 ns ticks since 0001-01-01 in the low 62 bits, the Kind in
    // the top 2. Comparisons use the ticks only, as .NET does. Only what C++ uses of it is here.
    // JSON is System.Text.Json's ISO 8601 form: "yyyy-MM-ddTHH:mm:ss", ".fffffff" trimmed of trailing zeros when there is
    // a fraction, "Z" for Utc. A Local value (an offset suffix) is refused: the C# side writes none to the files C++ reads.
    struct DateTime
    {
        static constexpr uint64_t TicksMask = 0x3FFFFFFFFFFFFFFFULL;
        static constexpr int32_t KindShift = 62;
        static constexpr int64_t UnixEpochTicks = 621'355'968'000'000'000LL;

        uint64_t DateData = 0;

        static DateTime MinValue() { return DateTime{}; }

        static DateTime FromTicks(int64_t ticks, DateTimeKind kind = DateTimeKind::Unspecified)
        {
            return DateTime{ static_cast<uint64_t>(ticks) | (static_cast<uint64_t>(kind) << KindShift) };
        }

        // C# Timestamp.ToDateTime: DateTime.UnixEpoch (Utc) plus the timestamp's whole ticks.
        static DateTime FromTimestamp(Timestamp timestamp)
        {
            return FromTicks(UnixEpochTicks + timestamp.NanosSinceEpoch / Nanoseconds::PerTick, DateTimeKind::Utc);
        }

        int64_t Ticks() const { return static_cast<int64_t>(DateData & TicksMask); }
        DateTimeKind Kind() const { return static_cast<DateTimeKind>(DateData >> KindShift); }

        DateTime Date() const { return FromTicks(Ticks() - Ticks() % TimeSpan::TicksPerDay, Kind()); }
        DateTime AddDays(int32_t days) const { return FromTicks(Ticks() + days * TimeSpan::TicksPerDay, Kind()); }
        DateTime Add(TimeSpan timeSpan) const { return FromTicks(Ticks() + timeSpan.Ticks, Kind()); }

        friend bool operator==(DateTime a, DateTime b) { return a.Ticks() == b.Ticks(); }
        friend auto operator<=>(DateTime a, DateTime b) { return a.Ticks() <=> b.Ticks(); }

        std::string ToString() const
        {
            using namespace std::chrono;
            int64_t ticks = Ticks();
            sys_days days{ std::chrono::days{ (ticks - UnixEpochTicks) / TimeSpan::TicksPerDay - ((ticks - UnixEpochTicks) % TimeSpan::TicksPerDay < 0 ? 1 : 0) } };
            year_month_day date{ days };
            int64_t ticksOfDay = ticks % TimeSpan::TicksPerDay;
            char text[48];
            int length = std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02d:%02d:%02d", static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                static_cast<int>(ticksOfDay / TimeSpan::TicksPerHour), static_cast<int>((ticksOfDay / TimeSpan::TicksPerMinute) % 60), static_cast<int>((ticksOfDay / TimeSpan::TicksPerSecond) % 60));
            std::string result(text, static_cast<size_t>(length));
            int64_t fraction = ticksOfDay % TimeSpan::TicksPerSecond;
            if (fraction != 0)
            {
                std::snprintf(text, sizeof(text), ".%07lld", static_cast<long long>(fraction));
                std::string fractionText(text);
                fractionText.erase(fractionText.find_last_not_of('0') + 1);
                result += fractionText;
            }
            if (Kind() == DateTimeKind::Utc)
                result += 'Z';
            return result;
        }

        static DateTime FromString(const std::string& text)
        {
            using namespace std::chrono;
            auto fail = [&]() -> DateTime { throw std::runtime_error("DateTime parse error: '" + text + "' is not yyyy-MM-ddTHH:mm:ss[.fffffff][Z]"); };
            auto digits = [&](size_t i, size_t count, int& value)
            {
                value = 0;
                for (size_t j = i; j < i + count; j++)
                {
                    if (j >= text.size() || text[j] < '0' || text[j] > '9')
                        return false;
                    value = value * 10 + (text[j] - '0');
                }
                return true;
            };
            int yearValue, monthValue, dayValue, hours, minutes, seconds;
            if (!digits(0, 4, yearValue) || text.size() < 19 || text[4] != '-' || !digits(5, 2, monthValue) || text[7] != '-' || !digits(8, 2, dayValue)
                || text[10] != 'T' || !digits(11, 2, hours) || text[13] != ':' || !digits(14, 2, minutes) || text[16] != ':' || !digits(17, 2, seconds))
                return fail();
            year_month_day date{ year{ yearValue }, month{ static_cast<unsigned>(monthValue) }, day{ static_cast<unsigned>(dayValue) } };
            if (!date.ok() || hours > 23 || minutes > 59 || seconds > 59)
                return fail();
            size_t i = 19;
            int64_t fraction = 0;
            if (i < text.size() && text[i] == '.')
            {
                int64_t scale = TimeSpan::TicksPerSecond;
                for (i++; i < text.size() && text[i] >= '0' && text[i] <= '9'; i++)
                {
                    if (scale > 1)
                    {
                        scale /= 10;
                        fraction += (text[i] - '0') * scale;
                    }
                }
            }
            DateTimeKind kind = DateTimeKind::Unspecified;
            if (i < text.size() && text[i] == 'Z')
            {
                kind = DateTimeKind::Utc;
                i++;
            }
            if (i != text.size())
                return fail();
            int64_t ticks = UnixEpochTicks + static_cast<int64_t>(sys_days{ date }.time_since_epoch().count()) * TimeSpan::TicksPerDay
                + hours * TimeSpan::TicksPerHour + minutes * TimeSpan::TicksPerMinute + seconds * TimeSpan::TicksPerSecond + fraction;
            return FromTicks(ticks, kind);
        }

        void Set(const std::string& text) { *this = FromString(text); }

        struct glaze
        {
            static constexpr auto value = glz::custom<&DateTime::Set, &DateTime::ToString>;
        };
    };
    static_assert(sizeof(DateTime) == 8);
}
