#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <ctime>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <format>

#include <cstdio>
#include <stdexcept>
#include <ctime>
#include <time.h>
#include <glaze/glaze.hpp>

namespace Tools
{
    // =============================== Constants ===============================

    struct Nanoseconds
    {
        static constexpr int64_t PerTick = 100;
        static constexpr int64_t PerMicrosecond = 1'000;
        static constexpr int64_t PerMillisecond = 1'000'000;
        static constexpr int64_t PerSecond = 1'000'000'000;
        static constexpr int64_t PerMinute = 60 * PerSecond;
        static constexpr int64_t PerHour = 60 * PerMinute;
        static constexpr int64_t PerDay = 24 * PerHour;
    };

    // =============================== Duration (ns-precision) ===============================

    struct Duration
    {
    public:
        int64_t TotalNanoseconds;

        static const Duration Zero;

        // Constructors
        Duration() : TotalNanoseconds(0) { }
        explicit Duration(int64_t nanoseconds) : TotalNanoseconds(nanoseconds) { }

        // Factories (Integer)
        [[nodiscard]] static Duration FromNanoseconds(int64_t ns) { return Duration(ns); }
        [[nodiscard]] static Duration FromMicroseconds(int64_t us) { return Duration(us * Nanoseconds::PerMicrosecond); }
        [[nodiscard]] static Duration FromMilliseconds(int64_t ms) { return Duration(ms * Nanoseconds::PerMillisecond); }
        [[nodiscard]] static Duration FromSeconds(int64_t s) { return Duration(s * Nanoseconds::PerSecond); }
        [[nodiscard]] static Duration FromMinutes(int64_t m) { return Duration(m * Nanoseconds::PerMinute); }
        [[nodiscard]] static Duration FromHours(int64_t h) { return Duration(h * Nanoseconds::PerHour); }
        [[nodiscard]] static Duration FromDays(int64_t d) { return Duration(d * Nanoseconds::PerDay); }

        // Factories (Floating Point)
        [[nodiscard]] static Duration FromMilliseconds(double ms) { return Duration(static_cast<int64_t>(ms * Nanoseconds::PerMillisecond)); }
        [[nodiscard]] static Duration FromSeconds(double s) { return Duration(static_cast<int64_t>(s * Nanoseconds::PerSecond)); }

        // Totals
        [[nodiscard]] double GetTotalMicroseconds() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerMicrosecond; }
        [[nodiscard]] double GetTotalMilliseconds() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerMillisecond; }
        [[nodiscard]] double GetTotalSeconds() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerSecond; }
        [[nodiscard]] double GetTotalMinutes() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerMinute; }
        [[nodiscard]] double GetTotalHours() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerHour; }
        [[nodiscard]] double GetTotalDays() const { return static_cast<double>(TotalNanoseconds) / Nanoseconds::PerDay; }

        // Component decomposition (Signed, because Duration can be negative)
        [[nodiscard]] int64_t Days() const { return TotalNanoseconds / Nanoseconds::PerDay; }
        [[nodiscard]] int32_t Hours() const
        {
            int64_t rem = TotalNanoseconds % Nanoseconds::PerDay;
            return static_cast<int32_t>(rem / Nanoseconds::PerHour);
        }
        [[nodiscard]] int32_t Minutes() const
        {
            int64_t rem = TotalNanoseconds % Nanoseconds::PerHour;
            return static_cast<int32_t>(rem / Nanoseconds::PerMinute);
        }
        [[nodiscard]] int32_t Seconds() const
        {
            int64_t rem = TotalNanoseconds % Nanoseconds::PerMinute;
            return static_cast<int32_t>(rem / Nanoseconds::PerSecond);
        }
        [[nodiscard]] int32_t Milliseconds() const
        {
            int64_t rem = TotalNanoseconds % Nanoseconds::PerSecond;
            return static_cast<int32_t>(rem / Nanoseconds::PerMillisecond);
        }
        [[nodiscard]] int32_t Microseconds() const
        {
            int64_t rem = TotalNanoseconds % Nanoseconds::PerMillisecond;
            return static_cast<int32_t>(rem / Nanoseconds::PerMicrosecond);
        }

        // Math
        [[nodiscard]] Duration Add(Duration other) const { return Duration(TotalNanoseconds + other.TotalNanoseconds); }
        [[nodiscard]] Duration Subtract(Duration other) const { return Duration(TotalNanoseconds - other.TotalNanoseconds); }
        [[nodiscard]] Duration Negate() const { return Duration(-TotalNanoseconds); }
        [[nodiscard]] Duration Abs() const { return Duration(TotalNanoseconds >= 0 ? TotalNanoseconds : -TotalNanoseconds); }

        // Scale
        [[nodiscard]] Duration Multiply(double factor) const { return Duration(static_cast<int64_t>(static_cast<double>(TotalNanoseconds) * factor)); }
        [[nodiscard]] Duration Divide(double divisor) const { return Duration(static_cast<int64_t>(static_cast<double>(TotalNanoseconds) / divisor)); }
        [[nodiscard]] double Divide(Duration other) const { return static_cast<double>(TotalNanoseconds) / static_cast<double>(other.TotalNanoseconds); }

        // Rounding
        [[nodiscard]] Duration RoundUp(int64_t quantumNanos) const
        {
            if (quantumNanos <= 0) return *this;
            int64_t r = TotalNanoseconds % quantumNanos;
            if (r == 0) return *this;
            if (r < 0) r += quantumNanos;
            return Duration(TotalNanoseconds + (quantumNanos - r));
        }

        // Operators
        friend bool operator==(Duration a, Duration b) { return a.TotalNanoseconds == b.TotalNanoseconds; }
        friend bool operator!=(Duration a, Duration b) { return a.TotalNanoseconds != b.TotalNanoseconds; }
        friend bool operator<(Duration a, Duration b) { return a.TotalNanoseconds < b.TotalNanoseconds; }
        friend bool operator<=(Duration a, Duration b) { return a.TotalNanoseconds <= b.TotalNanoseconds; }
        friend bool operator>(Duration a, Duration b) { return a.TotalNanoseconds > b.TotalNanoseconds; }
        friend bool operator>=(Duration a, Duration b) { return a.TotalNanoseconds >= b.TotalNanoseconds; }

        friend Duration operator+(Duration a, Duration b) { return Duration(a.TotalNanoseconds + b.TotalNanoseconds); }
        friend Duration operator-(Duration a, Duration b) { return Duration(a.TotalNanoseconds - b.TotalNanoseconds); }
        friend Duration operator-(Duration a) { return Duration(-a.TotalNanoseconds); }
        friend Duration operator*(Duration a, double f) { return a.Multiply(f); }
        friend Duration operator*(double f, Duration a) { return a.Multiply(f); }
        friend Duration operator/(Duration a, double d) { return a.Divide(d); }
        friend double operator/(Duration a, Duration b) { return a.Divide(b); }

        // Canonical form: "[-]d.HH:MM:SS.nnn_uuu_mmm"
        std::string ToString() const
        {
            using namespace std::chrono;
            nanoseconds total{TotalNanoseconds};
            bool negative = total.count() < 0;
            nanoseconds abs = negative ? -total : total;

            auto d = floor<days>(abs);
            hh_mm_ss hms{abs - d};
            int64_t sub = hms.subseconds().count();

            return std::format("{}{}.{:02}:{:02}:{:02}.{:03}_{:03}_{:03}",
                negative ? "-" : "",
                d.count(),
                hms.hours().count(),
                hms.minutes().count(),
                hms.seconds().count(),
                sub / 1'000'000,
                (sub / 1'000) % 1'000,
                sub % 1'000);
        }

        // Parse a duration. Format uses chrono specifiers plus a `%d` days-prefix extension:
        //   %d  days  (only valid as "%d." prefix; consumes digits up to the first '.')
        //   %H  hours, %M  minutes, %S  seconds (fractional auto-handled to target precision)
        // A leading '-' in the format is the optional-sign placeholder; a leading '-' in the
        // input is a literal sign that negates the result. Underscores in input are stripped
        // (canonical grouping); leading/trailing whitespace is trimmed.
        static Duration FromString(const std::string& input, const std::string& format = "-%d.%H:%M:%S")
        {
            using namespace std::chrono;

            std::string s = input;
            std::string f = format;
            s.erase(std::remove(s.begin(), s.end(), '_'), s.end());

            constexpr const char* ws = " \t\n\r\f\v";
            s.erase(0, s.find_first_not_of(ws));
            if (size_t p = s.find_last_not_of(ws); p != std::string::npos) s.erase(p + 1);

            bool negative = !s.empty() && s.front() == '-';
            if (negative) s.erase(0, 1);
            if (!f.empty() && f.front() == '-') f.erase(0, 1);

            int64_t totalDays = 0;
            if (f.starts_with("%d."))
            {
                size_t dot = s.find('.');
                if (dot == std::string::npos)
                    throw std::runtime_error("Duration parse error: '" + input + "' missing days prefix");
                totalDays = std::stoll(s.substr(0, dot));
                s.erase(0, dot + 1);
                f.erase(0, 3);
            }

            nanoseconds time{0};
            if (!f.empty())
            {
                std::istringstream iss(s);
                iss >> parse(f, time);
                if (iss.fail())
                    throw std::runtime_error("Duration parse error: '" + input + "' does not match format '" + format + "'");
            }

            int64_t total = totalDays * Nanoseconds::PerDay + time.count();
            return Duration(negative ? -total : total);
        }

        void Set(const std::string& input) { *this = FromString(input); }

        struct glaze
        {
            static constexpr auto value = glz::custom<&Duration::Set, &Duration::ToString>;
        };
    };

    inline const Duration Duration::Zero = Duration(0);

    // =============================== Timestamp ===============================

    struct Timestamp
    {
    public:
        // Strictly number of nanoseconds since 1970-01-01 00:00:00 UTC. Must be >= 0.
        int64_t NanosSinceEpoch;

        static const Timestamp MaxValue;
        static const Timestamp MinValue;

    private:
        static int64_t PositiveMod(int64_t value, int64_t mod)
        {
            int64_t r = value % mod;
            return r < 0 ? r + mod : r;
        }

        static int64_t ToNanoseconds(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second, int32_t milliseconds, int32_t microseconds, int32_t nanoseconds)
        {
            if (year < 1970)
                throw std::runtime_error("Timestamp error: Date must be after 1970.");

            std::tm tm_val{};
            tm_val.tm_year = year - 1900;
            tm_val.tm_mon = month - 1;
            tm_val.tm_mday = day;
            tm_val.tm_hour = hour;
            tm_val.tm_min = minute;
            tm_val.tm_sec = second;

            time_t seconds = timegm(&tm_val);
            if (seconds < 0)
                throw std::runtime_error("Timestamp error: Invalid or pre-epoch date.");

            return static_cast<int64_t>(seconds) * Nanoseconds::PerSecond
                 + static_cast<int64_t>(milliseconds) * Nanoseconds::PerMillisecond
                 + static_cast<int64_t>(microseconds) * Nanoseconds::PerMicrosecond
                 + nanoseconds;
        }

    public:
        Timestamp() : NanosSinceEpoch(0) { }
        explicit Timestamp(int64_t nanosSinceEpoch) : NanosSinceEpoch(nanosSinceEpoch) 
        {
            if (nanosSinceEpoch < 0)
            {
                throw std::runtime_error("Timestamp error: Value cannot be before 1970 (negative).");
            }
        }

        // Date Parts Logic
        [[nodiscard]] Timestamp Date() const
        {
            int64_t r = NanosSinceEpoch % Nanoseconds::PerDay;
            return Timestamp(NanosSinceEpoch - r);
        }

        // Constructor 1: Date Only
        Timestamp(int32_t year, int32_t month, int32_t day)
            : NanosSinceEpoch(ToNanoseconds(year, month, day, 0, 0, 0, 0, 0, 0))
        {
        }

        // Constructor 2: Date + Time
        Timestamp(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second)
            : NanosSinceEpoch(ToNanoseconds(year, month, day, hour, minute, second, 0, 0, 0))
        {
        }

        // Constructor 3: Full Precision
        Timestamp(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond, int32_t nanosecond)
            : NanosSinceEpoch(ToNanoseconds(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond))
        {
        }

        // Arithmetic
        [[nodiscard]] Timestamp AddDuration(Duration duration) const { return Timestamp(NanosSinceEpoch + duration.TotalNanoseconds); }
        [[nodiscard]] Timestamp AddTicks(int64_t ticks) const { return Timestamp(NanosSinceEpoch + ticks * Nanoseconds::PerTick); }
        [[nodiscard]] Timestamp AddNanoseconds(int64_t ns) const { return Timestamp(NanosSinceEpoch + ns); }
        [[nodiscard]] Timestamp AddMicroseconds(int64_t us) const { return Timestamp(NanosSinceEpoch + us * Nanoseconds::PerMicrosecond); }
        [[nodiscard]] Timestamp AddMilliseconds(int64_t ms) const { return Timestamp(NanosSinceEpoch + ms * Nanoseconds::PerMillisecond); }
        [[nodiscard]] Timestamp AddSeconds(int64_t s) const { return Timestamp(NanosSinceEpoch + s * Nanoseconds::PerSecond); }
        [[nodiscard]] Timestamp AddMinutes(int64_t m) const { return Timestamp(NanosSinceEpoch + m * Nanoseconds::PerMinute); }
        [[nodiscard]] Timestamp AddHours(int64_t h) const { return Timestamp(NanosSinceEpoch + h * Nanoseconds::PerHour); }
        [[nodiscard]] Timestamp AddDays(int64_t d) const { return Timestamp(NanosSinceEpoch + d * Nanoseconds::PerDay); }

        // Rounding
        [[nodiscard]] Timestamp RoundUp(int64_t quantumNanos) const
        {
            if (quantumNanos <= 0) return *this;
            int64_t r = NanosSinceEpoch % quantumNanos;
            return r == 0 ? *this : Timestamp(NanosSinceEpoch + (quantumNanos - r));
        }

        [[nodiscard]] Timestamp RoundUpMilliseconds(int32_t milliseconds) const { return RoundUp(milliseconds * Nanoseconds::PerMillisecond); }
        [[nodiscard]] Timestamp RoundUpSeconds(int32_t seconds) const { return RoundUp(seconds * Nanoseconds::PerSecond); }
        [[nodiscard]] Timestamp RoundUpMinutes(int32_t minutes) const { return RoundUp(minutes * Nanoseconds::PerMinute); }

        // Operators
        friend bool operator==(Timestamp a, Timestamp b) { return a.NanosSinceEpoch == b.NanosSinceEpoch; }
        friend bool operator!=(Timestamp a, Timestamp b) { return a.NanosSinceEpoch != b.NanosSinceEpoch; }
        friend bool operator<(Timestamp a, Timestamp b) { return a.NanosSinceEpoch < b.NanosSinceEpoch; }
        friend bool operator<=(Timestamp a, Timestamp b) { return a.NanosSinceEpoch <= b.NanosSinceEpoch; }
        friend bool operator>(Timestamp a, Timestamp b) { return a.NanosSinceEpoch > b.NanosSinceEpoch; }
        friend bool operator>=(Timestamp a, Timestamp b) { return a.NanosSinceEpoch >= b.NanosSinceEpoch; }

        friend Duration operator-(Timestamp a, Timestamp b) { return Duration::FromNanoseconds(a.NanosSinceEpoch - b.NanosSinceEpoch); }
        friend Timestamp operator+(Timestamp t, Duration d) { return Timestamp(t.NanosSinceEpoch + d.TotalNanoseconds); }
        friend Timestamp operator-(Timestamp t, Duration d) { return Timestamp(t.NanosSinceEpoch - d.TotalNanoseconds); }

        // Factories

        // PTP/GPS-aligned wall-clock time, read directly from CLOCK_REALTIME.
        //
        // With sfptpd (or ptp4l + phc2sys) running, the daemon steers the system
        // clock to the Solarflare PHC, which is itself disciplined to the roof GPS
        // grandmaster. CLOCK_REALTIME is UTC (leap-second offset already applied),
        // so it lines up with this 1970-UTC epoch. Do NOT use CLOCK_TAI here — the
        // PHC typically runs in TAI and would be off by the current leap offset (37s).
        //
        // Resolved via the vDSO (no syscall) on a TSC clocksource, so this is the
        // lean hot-path read.
        static Timestamp UtcNow()
        {
            struct timespec ts;
            ::clock_gettime(CLOCK_REALTIME, &ts);
            int64_t nanos = static_cast<int64_t>(ts.tv_sec) * Nanoseconds::PerSecond + ts.tv_nsec;
            if (nanos < 0) throw std::runtime_error("Timestamp error: System clock before 1970.");
            return Timestamp(nanos);
        }

        [[nodiscard]] static Timestamp Min(Timestamp a, Timestamp b) { return a.NanosSinceEpoch <= b.NanosSinceEpoch ? a : b; }
        [[nodiscard]] static Timestamp Max(Timestamp a, Timestamp b) { return a.NanosSinceEpoch >= b.NanosSinceEpoch ? a : b; }

        // Formatting
        std::string ToString() const
        {
            using namespace std::chrono;
            sys_time<nanoseconds> tp{nanoseconds(NanosSinceEpoch)};
            auto dp = floor<days>(tp);
            year_month_day ymd{dp};
            hh_mm_ss hms{tp - dp};
            int64_t sub = hms.subseconds().count();

            return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}_{:03}_{:03}",
                static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()),
                hms.hours().count(),
                hms.minutes().count(),
                hms.seconds().count(),
                sub / 1'000'000,
                (sub / 1'000) % 1'000,
                sub % 1'000);
        }

        std::string ToDateString() const
        {
            using namespace std::chrono;
            year_month_day ymd{floor<days>(sys_time<nanoseconds>{nanoseconds(NanosSinceEpoch)})};
            return std::format("{:04}-{:02}-{:02}",
                static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()));
        }

        static Timestamp FromString(const std::string& input, const std::string& format = "%Y-%m-%d %H:%M:%S")
        {
            std::string cleaned = input;
            cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '_'), cleaned.end());

            std::chrono::sys_time<std::chrono::nanoseconds> tp;
            std::istringstream iss(cleaned);
            iss >> std::chrono::parse(format, tp);
            if (iss.fail())
                throw std::runtime_error("Timestamp parse error: '" + input + "' does not match format '" + format + "'");

            int64_t ns = tp.time_since_epoch().count();
            if (ns < 0)
                throw std::runtime_error("Timestamp parse error: Pre-epoch timestamp '" + input + "'");

            return Timestamp(ns);
        }

        void Set(const std::string& input) { *this = FromString(input); }

        struct glaze
        {
            static constexpr auto value = glz::custom<&Timestamp::Set, &Timestamp::ToString>;
        };
    };

    inline const Timestamp Timestamp::MaxValue = Timestamp(INT64_MAX);
    inline const Timestamp Timestamp::MinValue = Timestamp(0);
}



