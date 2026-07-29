
#include "Allocate.hpp"
#include "Server.hpp"
#include <chrono>
using namespace std::chrono;

// Returns the CME week open (Sunday 17:00 America/Chicago) at or before tp, as UTC.
template <class Dur>
sys_seconds CmeWeekOpen(sys_time<Dur> tp)
{
    static const time_zone* chi = locate_zone("America/Chicago");

    // Work in Chicago local time so DST is handled for us.
    local_time<Dur> lt = chi->to_local(tp);

    auto lday = floor<days>(lt);
    weekday wd{lday};
    auto delta = wd - Sunday;              // days since Sunday, [0, 6]
    auto sunday = lday - delta;
    local_seconds open = sunday + hours{17};

    // If tp is Sunday but before 17:00 local, the week open is the *previous* Sunday.
    if (lt < open)
        open -= days{7};

    return chi->to_sys(open);
}


int main()
{
    Tools::Timestamp week = Tools::Timestamp::FromChrono(CmeWeekOpen(system_clock::now()));

    Provider::Server server(Provider::Server::DefaultServerHeader);
    
    server.LoadClients(week);
    server.AllocateClient = [&server, week](const Socket::SocketHeader& socketHeader) {
        server.SaveClient(socketHeader, week);
    };

    server.LoadInstruments(week);
    server.AllocateInstrument = [&server, week](Provider::AllocateInstrument allocateInstrument) {
        server.SaveInstrument(allocateInstrument, week);
    };

    server.Connect();

}