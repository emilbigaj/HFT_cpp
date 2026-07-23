//BEGIN_FILE HFT/Strategy/Scenario.hpp
#pragma once

#include "Client.hpp"
#include "Instrument.hpp"
#include "Strategy.hpp"
#include <string>

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
        Provider::Clock::Mode = Provider::ClockMode::Simulation;
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
            Data::FutureHeader& future = header128.AsFuture();

            if(header.Exchange == "XCME" && header.Root == "6E")
            {
                if (future.ExpiryDate >= expiry)
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
        
        while(_isRunning)
        {
            if (Provider::Clock::Mode == Provider::ClockMode::Simulation)
                Provider::Clock::SetUtcNow(_client.ClientContext.ServerHeader().GetReadonlyRef().Timestamp);
            _client.ReadSocket();
        }
    }

    void Stop()
    {
        _isRunning = false;
    }

};

}
//END_FILE HFT/Strategy/Scenario.hpp