//BEGIN_FILE HFT/Server/Client.hpp
#pragma once

#include "Allocate.hpp"
#include "Bitset.hpp"
#include "Context.hpp"
#include "Instrument.hpp"
#include "Socket.hpp"
#include "Order.hpp"
#include "OrderIdAllocator.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <chrono>
#include <span>
#include <vector>
#include "RiskLayer.hpp"


namespace Provider
{

class Client
{
public:
    const std::filesystem::path ClientName;
    const std::filesystem::path ServerName;

    uint64_t States = 0;
    uint64_t Rejections = 0;
    uint64_t Targets = 0;
private:
    Socket::ClientSocket _socket;
    Tools::Bitset64 _isOrderActive;
    bool _isDisposed = false;
    Provider::RiskLayer _riskLayer;

    // CoreGroupIds of the instruments this client has allocated => which execution channels ReadSocket drains.
    Tools::Bitset64 _coreGroupIds;

    // Blocking read of the admin channel (0) until a message arrives. Used for the connect handshake
    // (AllocateClient) and the instrument-allocation reply (AllocateInstrument).
    std::span<const uint8_t> ReadAdmin()
    {
        std::span<const uint8_t> rdst;
        while (_socket.TryRead(Socket::SocketChannel::Admin, rdst) != Socket::ReadStatus::New)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return rdst;
    }

    int32_t OnClientAllocated(const Provider::AllocateClient& allocate)
    {
        return allocate.ClientId;
    }

    int32_t Connect()
    {
        _socket.Connect();
        std::span<const uint8_t> rdst = ReadAdmin();
        return OnClientAllocated(*reinterpret_cast<const Provider::AllocateClient*>(rdst.data()));
    }

    // Peek the server's published ServerHeader (shared memory) for the CoreGroup layout, so we can
    // size our channels BEFORE _socket is built. The server publishes it as a LetterBox named
    // "<serverName>ServerHeader" (see ServerContext::Connect / Context). Returns the channel-length
    // array (identical for both directions). Blocks until the server is up.
    static std::vector<int32_t> ReadServerChannelLengths(const std::filesystem::path& serverName)
    {
        Socket::LetterBox<Provider::ServerHeader> serverHeaderBox(serverName.string() + "ServerHeader", Tools::Access::Read);
        Provider::ServerHeader serverHeader;
        while (!serverHeaderBox.TryPeek(serverHeader))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Socket::SocketChannel::BuildChannelLengths(serverHeader.CoreGroupIds);
    }

public:

    const int32_t ClientId;
    Provider::ClientContext ClientContext;

private:
    // Read-only view of the server's authoritative book (serverName-keyed), used to seed each
    // instrument's replica once on subscription. The strategy process does not init ContextManager,
    // so we open the server's book directly here rather than via ContextManager::ServerContextInstance.
    Socket::SharedArray<Data::MarketByPrice64> _serverMarketsByPrice;

    // Per-instrument broadcast ring readers (live deltas + trades), indexed by instrumentId.
    std::vector<std::unique_ptr<Socket::ReadOnlySocket>> _instrumentData;

public:
    std::function<void(const Execution::OrderState&)> OrderState;
    std::function<void(const Execution::OrderRejected&)> OrderRejected;
    std::function<void(const Execution::Fill&)> Fill;
    std::function<void(const Execution::PositionHeader&)> Position;
    // C++-only market-data surface (the C# strategy applies these via Instrument methods instead).
    std::function<void(const Data::Trade&)> Trade;
    std::function<void(const Data::MarketByPrice&, std::span<uint8_t>)> MarketByPrice;

    Client(const std::string& clientName, const std::string& serverName)
        : ClientName(Provider::ClientContext::GetDirectoryPath(clientName)),
          ServerName(Provider::ServerContext::GetDirectoryPath(serverName)),
          // Channel 0 is admin; channels 1..7 are per-CoreGroupId execution channels, sized from the
          // server's declared CoreGroupIds (read before connecting).
          _socket(ClientName, ServerName, ReadServerChannelLengths(ServerName), ReadServerChannelLengths(ServerName)),
          _isOrderActive(0ULL),
          _riskLayer(ServerName, Execution::OrderRejectedSource::Client),
          ClientId(Connect()),
          ClientContext(ClientName, ServerName, Tools::Access::Write),
          _serverMarketsByPrice(ServerName.string() + "MarketsByPrice", ClientContext.ServerHeader().GetReadonlyRef().InstrumentIds.Length(), Tools::Access::Read)
    {
        _instrumentData.resize(static_cast<size_t>(ClientContext.ServerHeader().GetReadonlyRef().InstrumentIds.Length()));
    }

    void ReadSocket()
    {
        std::span<const uint8_t> rdst;
        // Drain each execution channel this client uses (channel index == CoreGroupId).
        Tools::Bitset64 coreGroupIds = _coreGroupIds;
        int32_t coreGroupId = 0;
        while (coreGroupIds.TryPopLowest(coreGroupId))
        {
            while (_socket.TryRead(coreGroupId, rdst) == Socket::ReadStatus::New)
                OnSocketMessage(rdst);
        }

        // Pump each subscribed instrument's broadcast ring (deltas + trades) into our own replica book.
        Tools::Bitset64 instrumentIds = ClientContext.InstrumentIds();
        int32_t instrumentId = 0;
        while (instrumentIds.TryPopLowest(instrumentId))
        {
            PumpInstrumentData(instrumentId);
        }
    }

    Data::Instrument& GetInstrument(int32_t instrumentHeaderId)
    {
        int32_t instrumentId = -1;
        if (ClientContext.TryGetInstrumentId(instrumentHeaderId, instrumentId))
            return ClientContext.GetInstrument(instrumentId);

        const Data::InstrumentHeader128& header128 = ClientContext.GetInstrumentHeader(instrumentHeaderId).GetReadonlyRef();

        Provider::AllocateInstrument allocateInstrument
        {
            .Header = Data::Header<Provider::AllocateType>(Provider::AllocateType::Instrument),
            .ClientId = ClientId,
            .InstrumentHeaderId = instrumentHeaderId,
            .InstrumentId = -1,
            .Symbol = header128.Symbology()->ToString()
        };

        _socket.Write(Socket::SocketChannel::Admin, allocateInstrument);

        std::span<const uint8_t> rdst = ReadAdmin();
        return OnInstrumentAllocated(*reinterpret_cast<const Provider::AllocateInstrument*>(rdst.data()));
    }

    Data::Instrument& OnInstrumentAllocated(const Provider::AllocateInstrument& allocated)
    {
        int32_t instrumentId = allocated.InstrumentId;
        Data::Instrument& instrument = ClientContext.GetInstrument(instrumentId);
        ClientContext.GetPosition(instrumentId);   // ensure the position is created
        OpenInstrumentDataSocket(instrumentId, instrument.Symbol());
        _coreGroupIds.Set(instrument.Header().CoreGroupId);
        return instrument;
    }

    // Strategy: open the per-instrument ring, drain stale frames (protect against lapping), and seed
    // the replica from the server's authoritative book once (idempotent).
    virtual void OpenInstrumentDataSocket(int32_t instrumentId, const std::string& symbol)
    {
        if (_instrumentData[static_cast<size_t>(instrumentId)])
            return; // already attached + seeded

        std::string name = Socket::SocketChannel::GetInstrumentDataName(ServerName.string(), symbol);
        _instrumentData[static_cast<size_t>(instrumentId)] = std::make_unique<Socket::ReadOnlySocket>(name, Socket::SharedMemory::CreateOrOpen(name, Socket::SocketChannel::InstrumentDataChannelLength));

        // Drain to protect against lapping.
        Socket::ReadOnlySocket* reader = _instrumentData[static_cast<size_t>(instrumentId)].get();
        std::span<const uint8_t> bytes;
        while (reader->TryRead(bytes) == Socket::ReadStatus::New) { }

        // Seed our replica with a one-shot torn-safe read of the server's authoritative book. Live ring
        // deltas then apply on top (the ExchangeTimestamp guard in TrySet drops anything <= the seed).
        Data::MarketByPrice64 snapshot = _serverMarketsByPrice[instrumentId].Read();
        ClientContext.GetMarketByPrice64(instrumentId).Write(snapshot);
    }

    void PumpInstrumentData(int32_t instrumentId)
    {
        Socket::ReadOnlySocket* reader = _instrumentData[static_cast<size_t>(instrumentId)].get();
        if (reader == nullptr)
            return;

        std::span<const uint8_t> bytes;
        while (reader->TryRead(bytes) == Socket::ReadStatus::New)
        {
            OnInstrumentData(instrumentId, bytes);
        }
    }

    void OnInstrumentData(int32_t instrumentId, std::span<const uint8_t> bytes)
    {
        if (bytes.empty())
            return;

        uint8_t type = bytes[0];
        switch (type)
        {
            case static_cast<uint8_t>(Data::TickType::MarketByPriceDelta):
            case static_cast<uint8_t>(Data::TickType::MarketByPriceSnapshot):
            case static_cast<uint8_t>(Data::TickType::MarketByPriceUpdate):
            {
                ApplyMarketByPrice(instrumentId, bytes);
                break;
            }
            case static_cast<uint8_t>(Data::TickType::Trade):
            {
                if (Trade)
                    Trade(*reinterpret_cast<const Data::Trade*>(bytes.data()));
                break;
            }
            default:
                break; // ignore market-data frames this client doesn't consume (e.g. settlements)
        }
    }

    // Apply a delta/snapshot to our own replica under the seqlock, then fire the MarketByPrice callback
    // with the delta (a mutable copy, matching the callback signature).
    void ApplyMarketByPrice(int32_t instrumentId, std::span<const uint8_t> bytes)
    {
        Socket::SharedArrayEntry<Data::MarketByPrice64>& entry = ClientContext.GetMarketByPrice64(instrumentId);
        entry.AcquireLock();
        entry.GetRef().TrySet(bytes);
        entry.ReleaseLock();

        if (MarketByPrice)
        {
            alignas(Data::MarketByPrice) uint8_t buffer[Socket::ReadOnlySocket::BufferSize];
            std::memcpy(buffer, bytes.data(), bytes.size());
            Data::MarketByPrice& mbp = *reinterpret_cast<Data::MarketByPrice*>(buffer);
            MarketByPrice(mbp, std::span<uint8_t>(buffer, bytes.size()));
        }
    }

    bool OnOrderTarget(Execution::OrderTarget& orderTarget)
    {
        orderTarget.OrderHeader.NicTimestamp = Clock::GetUtcNow();

        if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create)
        {
            bool sent = Create(orderTarget);
            Targets += sent ? 1ULL : 0ULL;
            return sent;
        }
        else
        {
            bool sent = Amend(orderTarget);
            Targets += sent ? 1ULL : 0ULL;
            return sent;
        }
    }

    bool Create(Execution::OrderTarget& orderTarget)
    {
        orderTarget.OrderHeader.Seq = 1;
        orderTarget.OrderTargetAction = Execution::OrderTargetAction::Create;

        // Sender-authority rule: anything created by this client carries this client's ids,
        // regardless of what the template claimed - spoof-proof by construction. For a legitimately
        // recovered own id the stamp is a no-op. (C++ clients are their own strategy.)
        Execution::OrderId& orderId = orderTarget.OrderHeader.OrderId;
        orderId.ClientId(ClientId);
        orderId.StrategyId(ClientId);

        if (!Execution::OrderIdAllocator::TryAllocate(_isOrderActive, orderId))
        {
            Tools::Bitset64 orderRejectedReasons;
            orderRejectedReasons.Set((int)Execution::OrderRejectedReason::CantAllocateClientOrderId);
            Reject(orderTarget, orderRejectedReasons, Execution::OrderRejectedSource::Client);
            return false;
        }
        int32_t localOrderIndex = orderTarget.OrderHeader.OrderId.LocalIndex();

        if (Send(orderTarget))
        {
            ClientContext.GetPosition(orderTarget.OrderHeader.OrderId.InstrumentId()).OnOrderActive(localOrderIndex);
            return true;
        }
        else
        {
            Execution::OrderIdAllocator::Free(_isOrderActive, orderTarget.OrderHeader.OrderId);
            return false;
        }
    }

    bool Validate(Execution::OrderTarget& orderTarget, Tools::Bitset64& orderRejectedReasons)
    {
        bool isValid = _riskLayer.ValidateOrder(orderTarget, orderRejectedReasons);
        orderTarget.OrderTargetStatus = isValid ? Execution::OrderStateStatus::Active : Execution::OrderStateStatus::Done;
        return isValid;
    }

    virtual bool Send(Execution::OrderTarget& orderTarget)
    {
        Tools::Bitset64 orderRejectedReasons;
        if (Validate(orderTarget, orderRejectedReasons))
        {
            int localOrderIndex = orderTarget.OrderHeader.OrderId.LocalIndex();
            ClientContext.GetOrderTarget(localOrderIndex).Write(orderTarget);
            _socket.Write(ClientContext.GetInstrument(orderTarget.OrderHeader.OrderId.InstrumentId()).Header().CoreGroupId, orderTarget);
            return true;
        }
        else
        {
            Reject(orderTarget, orderRejectedReasons, Execution::OrderRejectedSource::Client);
            return false;
        }
    }

    virtual bool Amend(Execution::OrderTarget& orderTarget)
    {
        int localOrderIndex = orderTarget.OrderHeader.OrderId.LocalIndex();
        const Execution::OrderTarget existingOrderTarget = ClientContext.GetOrderTarget(localOrderIndex).GetReadonlyRef();

        if (existingOrderTarget.OrderHeader.OrderId != orderTarget.OrderHeader.OrderId) // ensure that orderTarget has not been re-allocated already
        {
            return false;
        }
        orderTarget.OrderHeader.Seq = std::max(existingOrderTarget.OrderHeader.Seq + 1, orderTarget.OrderHeader.Seq);

        return Send(orderTarget);
    }

    void Reject(const Execution::OrderTarget& orderTarget, Tools::Bitset64 orderRejectedReasons, Execution::OrderRejectedSource orderRejectedSource)
    {
        Execution::OrderRejected orderRejected
        {
            .OrderHeader = orderTarget.OrderHeader,
            .OrderTargetAction = orderTarget.OrderTargetAction,
            .OrderRejectedSource = orderRejectedSource,
            .OrderProfile = orderTarget.OrderProfile,
            .OrderRejectedReasons = orderRejectedReasons,
        };

        if (!orderRejectedReasons.IsEmpty() && orderRejectedReasons.IsSubsetOf(Execution::OrderRejected::OrderDiscarded))
            return;

        if (Clock::Mode == ClockMode::Simulation && orderRejectedReasons.Raw() == (1ULL << static_cast<int32_t>(Execution::OrderRejectedReason::TooManyOrdersPerSecond)))
            return;

        _socket.Write(ClientContext.GetInstrument(orderRejected.OrderHeader.OrderId.InstrumentId()).Header().CoreGroupId, orderRejected);
        if (OrderRejected) OrderRejected(orderRejected);
    }


private:

    void OnOrderRejected(const Execution::OrderRejected& orderRejected)
    {
        int32_t localOrderIndex = orderRejected.OrderHeader.OrderId.LocalIndex();
        Execution::OrderTarget& orderTarget = ClientContext.GetOrderTarget(localOrderIndex).GetRef();
        bool isTargetDone = orderRejected.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId && orderTarget.OrderHeader.Seq == orderRejected.OrderHeader.Seq;
        if (isTargetDone)
            orderTarget.OrderTargetStatus = Execution::OrderStateStatus::Done;
    }

    void OnOrderState(const Execution::OrderState& orderState)
    {
        int32_t localOrderIndex = orderState.OrderHeader.OrderId.LocalIndex();
        Execution::OrderTarget& orderTarget = ClientContext.GetOrderTarget(localOrderIndex).GetRef();
        if (orderState.OrderHeader.OrderId == orderTarget.OrderHeader.OrderId)
        {
            if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done)
            {
                orderTarget.OrderTargetStatus = Execution::OrderStateStatus::Done;
                ClientContext.GetPosition(orderState.OrderHeader.OrderId.InstrumentId()).OnOrderDone(localOrderIndex);
                Execution::OrderIdAllocator::Free(_isOrderActive, orderState.OrderHeader.OrderId);
            }
            else if (orderState.OrderHeader.Seq >= orderTarget.OrderHeader.Seq)
            {
                orderTarget.OrderTargetStatus = Execution::OrderStateStatus::Done;
            }
        }

        if (OrderState)
            OrderState(orderState);
    }

    void OnFill(std::span<const uint8_t> rsrc)
    {
        const Execution::Fill& fill = *reinterpret_cast<const Execution::Fill*>(rsrc.data());
        if (Fill)
            Fill(fill);
    }

    void OnPositionHeader(std::span<const uint8_t> rsrc)
    {
        const Execution::PositionHeader& positionHeader = *reinterpret_cast<const Execution::PositionHeader*>(rsrc.data());
        if (Position)
            Position(positionHeader);
    }

    void OnSocketMessage(std::span<const uint8_t> rsrc)
    {
        uint8_t type = rsrc[0];

        switch (type)
        {
            case static_cast<uint8_t>(Execution::OrderType::OrderState):
            {
                States += 1;
                OnOrderState(*reinterpret_cast<const Execution::OrderState*>(rsrc.data()));
                break;
            }
            case static_cast<uint8_t>(Execution::OrderType::OrderRejected):
            {
                Rejections += 1;
                OnOrderRejected(*reinterpret_cast<const Execution::OrderRejected*>(rsrc.data()));
                break;
            }
            case static_cast<uint8_t>(Execution::OrderType::Fill):
            {
                OnFill(rsrc);
                break;
            }
            case static_cast<uint8_t>(Execution::OrderType::Position):
            {
                OnPositionHeader(rsrc);
                break;
            }
            default:
                throw std::runtime_error("Unknown message type: " + std::to_string(type));
        }
    }

public:

    void Dispose()
    {
        if (_isDisposed)
            return;

        if (_isOrderActive.IsFull())
            throw std::runtime_error("Client::Dispose: orders still active");

        _isDisposed = true;

        for (std::unique_ptr<Socket::ReadOnlySocket>& reader : _instrumentData)
        {
            if (reader)
                reader->Dispose();
        }

        _socket.Close();
        _socket.Dispose();
    }
};

}

//END_FILE HFT/Server/Client.hpp
