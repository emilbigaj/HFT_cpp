#pragma once

#include <chrono>
#include <string>

#include "DateTime.hpp"
#include "Timestamp.hpp"

namespace Tools
{
    // The part of C# Tools/Session.cs that C++ uses: a session's time zone, to turn a UTC timestamp into the
    // session's local time. C# names the CME zone by its Windows id "Central Standard Time"; it is America/Chicago.
    struct Session
    {
        std::string Name;
        const std::chrono::time_zone* TimeZone;

        static const Session& CME()
        {
            static const Session s_cme{ "CME", std::chrono::locate_zone("America/Chicago") };
            return s_cme;
        }

        // C# TimeZoneInfo.ConvertTime(timestamp.ToDateTime, Utc, TimeZone): local wall-clock time, Kind Unspecified.
        DateTime ConvertToLocal(Timestamp timestamp) const
        {
            std::chrono::sys_time<std::chrono::nanoseconds> utc{ std::chrono::nanoseconds{ timestamp.NanosSinceEpoch } };
            int64_t localNanoseconds = TimeZone->to_local(utc).time_since_epoch().count();
            return DateTime::FromTicks(DateTime::UnixEpochTicks + localNanoseconds / Nanoseconds::PerTick);
        }
    };
}
