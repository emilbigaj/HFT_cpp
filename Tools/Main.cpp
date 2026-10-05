#include <iostream>
#include <cassert>
#include "Timestamp.hpp"
#include "Bitset.hpp"
#include "Json.hpp"
#include "String.hpp"
#include "Clock.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Clock is process-global and Stop() is permanent (C# never clears IsStopping), so the cases run as one
// sequence with the handlers registered once and the Stop() case last.
static bool TestClock()
{
    using Tools::Clock;
    using Tools::ClockMode;
    using Tools::Duration;
    using Tools::Reminder;
    using Tools::Timestamp;

    std::vector<std::string> fired;
    std::vector<std::string> exceptions;
    std::vector<Timestamp> tickTocks;
    int started = 0;
    int stopped = 0;
    std::function<void(Timestamp)> onInterject;

    Clock::Exception += [&](const std::exception& exception) { exceptions.push_back(exception.what()); };
    Clock::TickTock += [&](Timestamp now) { tickTocks.push_back(now); };
    Clock::Started += [&](Timestamp) { started++; };
    Clock::Stopped += [&](Timestamp) { stopped++; };
    Clock::Interject += [&](Timestamp next) { if (onInterject) onInterject(next); };

    auto reset = [&]()
    {
        fired.clear();
        exceptions.clear();
        tickTocks.clear();
        started = 0;
        stopped = 0;
        onInterject = nullptr;
    };
    auto fail = [](const std::string& what)
    {
        std::cout << "FAIL: Clock " << what << std::endl;
        return false;
    };

    const Timestamp begin = Timestamp::FromString("2026-10-05 09:00:00");
    const Duration second = Duration::FromSeconds(int64_t{ 1 });

    Clock::SetMode(ClockMode::Simulation);
    if (Clock::Mode() != ClockMode::Simulation || Clock::IsRunning())
        return fail("initial state");

    // Order by timestamp, FIFO among equal timestamps; UtcNow() is the reminder's time inside its callback.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second * 10);
        if (Clock::UtcNow() != begin)
            return fail("SetBegin did not move UtcNow");

        auto add = [&](const std::string& name, Timestamp at)
        {
            Clock::AddReminder(Reminder(at, [&, name](Timestamp timestamp)
            {
                fired.push_back(name);
                if (Clock::UtcNow() != timestamp)
                    fired.push_back("UtcNow mismatch at " + name);
            }, name));
        };
        add("A", begin + second * 2);
        add("B", begin + second * 1);
        add("C", begin + second * 2);
        add("D", begin + second * 1);
        add("E", begin);
        if (Clock::RemindersQueued() != 5)
            return fail("RemindersQueued after AddReminder");

        Clock::Start();

        if (fired != std::vector<std::string>{ "E", "B", "D", "A", "C" })
        {
            std::string order;
            for (const std::string& f : fired)
                order += f + " ";
            return fail("reminder order: " + order);
        }
        if (started != 1 || stopped != 1)
            return fail("Started/Stopped count " + std::to_string(started) + "/" + std::to_string(stopped));
        if (tickTocks != std::vector<Timestamp>{ begin + second, begin + second * 2, begin + second * 10 })
            return fail("TickTock sequence");
        if (Clock::IsRunning() || Clock::UtcNow() != begin + second * 10 || Clock::RemindersQueued() != 0)
            return fail("state after simulation");
    }

    // TryRemoveReminder matches on timestamp and callback identity only.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second * 10);

        auto record = [&](const std::string& name) { return [&, name](Timestamp) { fired.push_back(name); }; };
        Reminder first(begin + second, record("first"));
        Reminder second_(begin + second, record("second"));
        Reminder later = first;
        later.Timestamp = begin + second * 2;
        later.Name = "later";
        Clock::AddReminder(first);
        Clock::AddReminder(second_);
        Clock::AddReminder(later);

        Clock::TryRemoveReminder(Reminder(begin + second, record("first")));
        if (Clock::RemindersQueued() != 3)
            return fail("TryRemoveReminder removed a reminder with a different callback");
        Clock::TryRemoveReminder(first);
        if (Clock::RemindersQueued() != 2)
            return fail("TryRemoveReminder did not remove the matching reminder");

        Clock::Start();

        if (fired != std::vector<std::string>{ "second", "first" })
            return fail("TryRemoveReminder removed the wrong reminder");

        // Reminders built from one held callback are equal, like C# Reminders built from one stored delegate.
        auto held = std::make_shared<const std::function<void(Timestamp)>>(record("held"));
        Clock::AddReminder(Reminder(begin + second, held));
        Clock::TryRemoveReminder(Reminder(begin + second, held));
        if (Clock::RemindersQueued() != 0)
            return fail("TryRemoveReminder did not match a Reminder built from the same held callback");
    }

    // A handler removed with -= no longer runs; a throwing Started handler still leaves the clock stopped.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second);
        auto throwing = Clock::Started += [](Timestamp) { throw std::runtime_error("started boom"); };
        Clock::Start();
        Clock::Started -= throwing;
        if (Clock::IsRunning() || exceptions != std::vector<std::string>{ "started boom" } || stopped != 1)
            return fail("a throwing Started handler left the clock running");

        reset();
        Clock::Start();
        if (!exceptions.empty() || started != 1)
            return fail("Started -= did not remove the handler");
    }

    // An Interject handler's OnInterject brings the next stop forward.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second * 10);
        const Timestamp interjected = begin + Duration::FromMilliseconds(int64_t{ 1500 });
        bool once = false;
        onInterject = [&](Timestamp next)
        {
            if (once)
                return;
            once = true;
            if (next != begin + second * 10)
                fired.push_back("unexpected next stop");
            Clock::OnInterject(interjected);
            Clock::OnInterject(begin - second);
        };

        Clock::Start();

        if (!fired.empty() || tickTocks.empty() || tickTocks.front() != interjected)
            return fail("OnInterject did not stop the clock at the interjected time");
        if (tickTocks.back() != begin + second * 10)
            return fail("simulation did not resume to End after the interjection");
    }

    // A throwing reminder reaches the Exception event and later reminders still run.
    // SetMode and Start while running throw.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second * 10);
        Clock::AddReminder(Reminder(begin + second, [](Timestamp) { throw std::runtime_error("reminder boom"); }, "throws"));
        Clock::AddReminder(Reminder(begin + second * 2, [&](Timestamp)
        {
            fired.push_back("after");
            try
            {
                Clock::SetMode(ClockMode::Realtime);
                fired.push_back("SetMode did not throw");
            }
            catch (const std::logic_error&)
            {
                fired.push_back("SetMode threw");
            }
            try
            {
                Clock::Start();
                fired.push_back("Start did not throw");
            }
            catch (const std::logic_error&)
            {
                fired.push_back("Start threw");
            }
            try
            {
                Clock::SetBegin(begin);
                fired.push_back("SetBegin did not throw");
            }
            catch (const std::logic_error&)
            {
                fired.push_back("SetBegin threw");
            }
        }, "after"));

        Clock::Start();

        if (exceptions != std::vector<std::string>{ "reminder boom" })
            return fail("reminder exception did not reach the Exception event");
        if (fired != std::vector<std::string>{ "after", "SetMode threw", "Start threw", "SetBegin threw" })
            return fail("guards while running");
        if (Clock::Mode() != ClockMode::Simulation || started != 1 || stopped != 1)
            return fail("state after guarded calls");
    }

    // Speed 1000 paces a 1 s simulated gap to about 1 ms of wall time; double max does not wait.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second);
        Clock::SetSimulationSpeed(1000.0);
        auto wallBegin = std::chrono::steady_clock::now();
        Clock::Start();
        double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallBegin).count();
        if (Clock::SimulationSpeed() != 1000.0)
            return fail("SimulationSpeed not applied");
        if (wallMs < 0.9 || wallMs > 500.0)
            return fail("speed 1000 took " + std::to_string(wallMs) + " ms for 1 simulated second");

        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + Duration::FromHours(int64_t{ 1 }));
        Clock::SetSimulationSpeed(std::numeric_limits<double>::max());
        wallBegin = std::chrono::steady_clock::now();
        Clock::Start();
        wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallBegin).count();
        if (Clock::SimulationSpeed() != std::numeric_limits<double>::max())
            return fail("SimulationSpeed max not applied");
        if (wallMs > 100.0)
            return fail("unpaced simulation of 1 h took " + std::to_string(wallMs) + " ms");
    }

    // Realtime UtcNow() is the wall clock, whatever the simulated time.
    {
        Clock::SetMode(ClockMode::Realtime);
        Timestamp before = Timestamp::UtcNow();
        Timestamp now = Clock::UtcNow();
        Timestamp after = Timestamp::UtcNow();
        Clock::SetMode(ClockMode::Simulation);
        if (now < before || now > after)
            return fail("Realtime UtcNow is not wall time");
    }

    // Stop() from a callback ends Start() before End. As in C#, the iteration in progress still advances to
    // the next stop and the final drain runs the reminders due there. Last because IsStopping is never cleared.
    {
        reset();
        Clock::SetBegin(begin);
        Clock::SetEnd(begin + second * 10);
        Clock::AddReminder(Reminder(begin + second, [&](Timestamp) { fired.push_back("stop"); Clock::Stop(); }, "stop"));
        Clock::AddReminder(Reminder(begin + second * 2, [&](Timestamp) { fired.push_back("next"); }, "next"));
        Clock::AddReminder(Reminder(begin + second * 5, [&](Timestamp) { fired.push_back("never"); }, "never"));

        Clock::Start();

        if (fired != std::vector<std::string>{ "stop", "next" } || !Clock::IsStopping() || Clock::IsRunning())
            return fail("Stop() from a callback did not end Start()");
        if (Clock::UtcNow() != begin + second * 2 || Clock::RemindersQueued() != 1 || started != 1 || stopped != 1)
            return fail("state after Stop()");
    }

    return true;
}

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

    if (!TestClock())
        return 1;

    std::cout << "ToolsTests passed." << std::endl;
    return 0;
}