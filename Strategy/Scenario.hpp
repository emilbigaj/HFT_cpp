//BEGIN_FILE HFT/Strategy/Scenario.hpp
#pragma once

#include "Client.hpp"
#include "Instrument.hpp"
#include "Strategy.hpp"
#include "Clock.hpp"
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

namespace Strategy
{
class Scenario
{
private:
    std::string _clientName;
    std::string _serverName;

    Provider::Client _client;
    std::unique_ptr<Strategy> _strategy;

public:
    Scenario(const std::string& clientName)
    : _clientName(Provider::ClientContext::GetDirectoryPath(clientName))
    , _serverName(Provider::ServerContext::GetDirectoryPath(clientName))
    , _client(_clientName, _serverName)
    {
        Tools::Clock::SetMode(Tools::ClockMode::Simulation);
    }

    Data::InstrumentHeader128 GetInstrumentHeader()
    {
        std::string exchange = "XCME";
        std::string root = "6E";
        Tools::Timestamp expiry = Tools::Timestamp(2024, 3, 10);


        for(int instrumentHeaderId = 0; instrumentHeaderId < _client.ClientContext.ServerHeader().GetReadonlyRef().InstrumentsCount; instrumentHeaderId++)
        {
            Data::InstrumentHeader128 header128 = _client.ClientContext.GetInstrumentHeader(instrumentHeaderId).Read();
            Data::InstrumentHeader& header = header128.AsInstrumentHeader();

            // A realtime context holds spreads and unfilled slots too; AsFuture on those is a bad cast.
            if (header.InstrumentType != Data::InstrumentType::Future)
                continue;

            Data::FutureHeader& future = header128.AsFuture();

            if(header.Exchange == "XCME" && header.Root == "6E")
            {
                if (future.MaturityDate >= expiry)
                {
                    return header128;
                }
            }
        }
        throw std::runtime_error("No future instrument found");
    }


    void BuildStrategy()
    {
        Data::InstrumentHeader128 instrumentHeader128 = GetInstrumentHeader();

        Data::Instrument& instrument = _client.GetInstrument(instrumentHeader128.AsInstrumentHeader().InstrumentHeaderId);

        _strategy = std::make_unique<Strategy>(_client, instrument);
    }

    bool _isRunning = false;
    
    void Start()
    {
        if (_isRunning)
            return;

        _isRunning = true;

        if (Tools::Clock::Mode() == Tools::ClockMode::Simulation)
        {
            // Clock::Stop is permanent (as in C#), so a simulation Scenario runs once.
            if (Tools::Clock::IsStopping())
            {
                _isRunning = false;
                throw std::logic_error("Clock has been stopped; a simulation Scenario can not be restarted.");
            }

            Tools::Clock::SetBegin(_client.ClientContext.ServerHeader().GetReadonlyRef().Timestamp);
            Tools::Clock::SetEnd(Tools::Timestamp::MaxValue);
            auto interject = Tools::Clock::Interject += [this](Tools::Timestamp)
            {
                // The header follows NIC timestamps written by several RX threads and can step back; OnInterject
                // ignores an earlier time, which would let the clock run straight to End.
                Tools::Clock::OnInterject(Tools::Timestamp::Max(_client.ClientContext.ServerHeader().GetReadonlyRef().Timestamp, Tools::Clock::UtcNow()));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            };
            auto tickTock = Tools::Clock::TickTock += [this](Tools::Timestamp) { _client.ReadSocket(); };
            // C# routes these to the AlertManager; without one here, the first failure ends the run and reaches main.
            std::exception_ptr clockException;
            auto exception = Tools::Clock::Exception += [&clockException](const std::exception& e)
            {
                if (!clockException)
                    clockException = std::make_exception_ptr(std::runtime_error(e.what()));
                Tools::Clock::Stop();
            };
            auto unsubscribe = [&]()
            {
                Tools::Clock::Interject -= interject;
                Tools::Clock::TickTock -= tickTock;
                Tools::Clock::Exception -= exception;
            };

            try
            {
                Tools::Clock::Start();
            }
            catch (...)
            {
                unsubscribe();
                _isRunning = false;
                throw;
            }

            // A signal handled on another thread runs the exit chain there and ends in std::exit; returning
            // would destroy the Client that its later exit actions still use.
            if (Tools::Application::IsExiting())
                while (true)
                    std::this_thread::sleep_for(std::chrono::seconds(1));

            unsubscribe();
            _isRunning = false;
            if (clockException)
                std::rethrow_exception(clockException);
            return;
        }

        while(_isRunning)
            _client.ReadSocket();
    }

    void Stop()
    {
        _isRunning = false;
        Tools::Clock::Stop();
    }

};

}
//END_FILE HFT/Strategy/Scenario.hpp