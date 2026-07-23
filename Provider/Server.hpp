//BEGIN_FILE HFT/Provider/Server.hpp
#pragma once

#include "Bitset.hpp"
#include "Context.hpp"
#include "Instrument.hpp"
#include "Loggable.hpp"
#include "Protocol.hpp"
#include "RiskLayer.hpp"
#include "SharedArray.hpp"
#include "Socket.hpp"
#include "Allocate.hpp"
#include "Order.hpp"
#include "Tick.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"
#include "SeqLock.hpp"
#include "ByteQueue.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <array>
#include <atomic>

namespace Provider
{

class Server
{
public:
    const std::filesystem::path ServerName;

private:
    const Socket::LetterBox<ServerHeader> _serverHeaderBox;
    Socket::ServerSocket _serverSocket;
    Provider::ServerContext _serverContext;
    Socket::ClientSocket _loggingServer;
    Socket::ClientSocket _audit;
    RiskLayer _riskLayer;
    Provider::LoggableManager _loggableManager;
    std::vector<std::unique_ptr<Socket::WriteOnlySocket>> _instrumentData;

    // One spinlock per CoreGroup (index == CoreGroupId; 0 = admin). Fixed array (not a vector):
    // ExecutionLock holds a std::atomic, which is non-movable so it can't live in a resizable vector.
    struct alignas(64) ExecutionLock { std::atomic<bool> Flag{false}; };

    // Guards WriteToExecution (return channel, S->C). Multi-writer per segment (RX fills/states +
    // send create-acks) but same CCD, so the lock line stays CCD-resident.
    std::array<ExecutionLock, 8> _recvFromExchangeLocks;

    // Guards the PRODUCER end of the injection queue below. Only writers contend (hub + vendor RX);
    // the ReadExecution(cg) thread is the sole reader and takes no lock (ByteQueue SPSC read side).
    std::array<ExecutionLock, 8> _sendToExchangeLocks;

    // Per-CoreGroup OrderTarget injection queue: hub + vendor RX EnqueueOrderTarget() here, the
    // ReadExecution(cg) thread drains and sends, so it stays the sole order sender. unique_ptr: non-movable.
    std::array<std::unique_ptr<Tools::ByteQueue>, 8> _orderTargetQueues;

    // Per CoreGroup: which clients trade it (= the clients ReadExecution(coreGroupId) polls). Set on the
    // admin thread at instrument allocation, read on the exec threads => atomic ops. Indexed by
    // CoreGroupId; a set bit is a clientId. Sized to highest CoreGroupId + 1 in the ctor.
    std::vector<Tools::Bitset64> _clientIdsByCoreGroupId;
public:

    Provider::ServerContext& Context()
    {
        return _serverContext;
    }
    Provider::LoggableManager& LoggableManager()
    {
        return _loggableManager;
    }

    static inline const Provider::ServerHeader DefaultServerHeader =
    {
        .ServerName = Tools::String128("ServerName"),
        .Timestamp = Tools::Timestamp(0),
        .InstrumentsCapacity = 4096,
        .InstrumentsCount = 0,
        .InstrumentIds = Tools::Bitset64(),
        .ClientIds = Tools::Bitset64(),
        .CoreGroupIds = Tools::Bitset64(),
        .OrdersPerClient = 64,
    };

    std::function<void(const Execution::OrderTarget&)> OrderTarget;
    std::function<void(const Execution::OrderState&)> OrderState;
    std::function<void(const Execution::OrderRejected&, const std::string&)> OrderRejected;
    std::function<void(const Execution::Fill&)> Fill;


    std::function<void(const AllocateInstrument&)> AllocateInstrument;

    Server(const ServerHeader& serverHeader)
    : ServerName(serverHeader.ServerName.ToString()),
      _serverHeaderBox(ServerContext::Connect(serverHeader)),
      _serverSocket(ServerName.string(), serverHeader.ClientIds.Length()),
      _serverContext(ServerName, Tools::Access::Write),
      _loggingServer(ServerName.string() + ".server", _serverContext.LoggingServerName, {Socket::SocketChannel::AdminChannelLength}, {Socket::SocketChannel::AdminChannelLength}),
      _audit(ServerName.string() + ".audit", _serverContext.LoggingServerName, Socket::SocketChannel::BuildChannelLengths(serverHeader.CoreGroupIds), {Socket::SocketChannel::AdminChannelLength}),
      _riskLayer(ServerName, Execution::OrderRejectedSource::Server),
      _loggableManager()
    {
        _instrumentData.resize(static_cast<size_t>(serverHeader.InstrumentIds.Length()));
        // +1: channel index == CoreGroupId, so we need slots 0..HighestSet() (matches BuildChannelLengths).
        _clientIdsByCoreGroupId.resize(static_cast<size_t>(serverHeader.CoreGroupIds.HighestSet() + 1));

        // One OrderTarget injection queue per EXECUTION CoreGroup (admin carries no order targets).
        for (int32_t coreGroupId : serverHeader.CoreGroupIds)
            if (coreGroupId != Socket::SocketChannel::Admin)
                _orderTargetQueues[static_cast<size_t>(coreGroupId)] = std::make_unique<Tools::ByteQueue>(Tools::Memory::SmallPageLength);

        _serverSocket.AllocateClientId = [this](const Socket::SocketHeader& header) {
            return _serverContext.AllocateClientId(header); 
        };

        _serverSocket.DeallocateClient = [this](int32_t clientId) {
            return _serverContext.DeallocateClient(clientId);
        };

        _serverSocket.ClientAllocated = [this](const Socket::SocketHeader& header) { 
            this->OnClientAllocated(header); 
        };

        _serverSocket.ClientDeallocated = [this](const Socket::SocketHeader& header) { 
            this->OnClientDeallocated(header);
        };

        _serverSocket.Listen();
        _loggingServer.Connect();
        _audit.Connect();
    }

    template <typename T> requires Tools::PlainOldData<T>
    inline static std::filesystem::path GetSeriesFilePath(const std::filesystem::path& seriesDirectoryPath, const std::string& name)
    {
        std::string fileName = name + "." + Tools::GetTypeName<T>();
        return seriesDirectoryPath / fileName;
    }

    template <typename T> requires Tools::PlainOldData<T>
    Series<T>& NewSeries(const std::string& name)
    {
        std::filesystem::path filePath = GetSeriesFilePath<T>(_serverContext.SeriesDirectoryPath, name);
        std::unique_ptr<Series<T>> series = std::make_unique<Series<T>>(filePath.string(), _serverContext.LoggingServerName);

        if (std::filesystem::exists(filePath) && Clock::Mode == ClockMode::Realtime)
        {
            std::optional<std::string> json = Tools::ReadLastLine(filePath.string());
            if (json.has_value())
            {
                series->Value = Tools::Json::Deserialize<T>(json.value());
            }
        }

        Series<T>& seriesRef = *series;
        _loggableManager.OnLogging(std::move(series));

        return seriesRef;
    }


    // Disconnect housekeeping — run by ONE thread (the hub), NOT per-segment: it owns _clientIds and
    // CancelAllOrders is cross-segment. Cancels a dropped client's working orders and clears it from
    // each CoreGroup's client set so ReadExecution stops polling it (AtomicClear => race-free vs readers).
    Tools::Bitset64 _clientIds = {};
    void PollDisconnects()
    {
        Tools::Bitset64 closedClientIds;
        {
            Tools::Bitset64 clientIds = _serverSocket.ClientIds();
            closedClientIds = _clientIds & ~clientIds;
            _clientIds = clientIds;
            for(int32_t clientId : closedClientIds)
                CancelAllOrders(clientId);
        }
        
        {
            int32_t coreGroupsIdsCount = static_cast<int32_t>(_clientIdsByCoreGroupId.size());
            for(int32_t coreGroupId = 0; coreGroupId < coreGroupsIdsCount; coreGroupId++)
            {
                Tools::Bitset64& clientIds = _clientIdsByCoreGroupId[static_cast<size_t>(coreGroupId)];
                for(int32_t clientId : closedClientIds)
                    clientIds.AtomicClear(clientId);    
            }
        }
    }

    // Producer API for the injection queue (derives cg from the instrument). Hub + vendor RX call this
    // instead of sending; writers serialise on _sendToExchangeLocks, full queue spins (never drops).
    void EnqueueOrderTarget(const Execution::OrderTarget& orderTarget)
    {
        Data::Instrument& instrument = _serverContext.GetInstrument(orderTarget.OrderHeader.OrderId.InstrumentId());
        int32_t coreGroupId = instrument.Header().CoreGroupId;
        Tools::ByteQueue& queue = *_orderTargetQueues[static_cast<size_t>(coreGroupId)];
        Tools::RAIISpinLock lock(_sendToExchangeLocks[static_cast<size_t>(coreGroupId)].Flag);
        std::span<uint8_t> dst;
        while (!queue.TryEnqueue(static_cast<int32_t>(sizeof(Execution::OrderTarget)), dst))
            _mm_pause();
        std::memcpy(dst.data(), &orderTarget, sizeof(Execution::OrderTarget));
        queue.Commit();
    }

    // Per-CoreGroup hot poll: ONE thread per CoreGroup busy-polls this with its own coreGroupId,
    // reading every connected client's channel for THIS segment and dispatching its OrderTargets.
    // One reader thread per (client, channel) => SPSC-safe; different segments touch different
    // ReadOnlySockets. It also drains the injection queue first (hub cancels + RX replays).
    void ReadExecution(int32_t coreGroupId)
    {
        // Drain injected OrderTargets first (hub cancels + RX replays): sole reader, no lock. Copy out
        // and Dequeue before sending so the slot frees ahead of a slow SendOrder.
        if (Tools::ByteQueue* injected = _orderTargetQueues[static_cast<size_t>(coreGroupId)].get())
        {
            std::span<const uint8_t> qsrc;
            while (injected->TryPeek(qsrc))
            {
                Execution::OrderTarget orderTarget = *reinterpret_cast<const Execution::OrderTarget*>(qsrc.data());
                injected->Dequeue();
                OnOrderTarget(orderTarget);
            }
        }

        Tools::Bitset64 clientIds = _clientIdsByCoreGroupId[static_cast<size_t>(coreGroupId)].AtomicLoad();
        std::span<const uint8_t> rdst;
        for(int32_t clientId : clientIds)
        {
            while (_serverSocket.TryRead(clientId, coreGroupId, rdst) == Socket::ReadStatus::New)
            {
                if (rdst.empty())
                    continue;
                uint8_t msgType = rdst[0];
                switch (msgType)
                {
                    case static_cast<uint8_t>(Execution::OrderType::OrderTarget):
                    {
                        const Execution::OrderTarget& orderTarget = *reinterpret_cast<const Execution::OrderTarget*>(rdst.data());
                        OnOrderTarget(orderTarget);
                        break;
                    }
                    case static_cast<uint8_t>(Execution::OrderType::OrderRejected):
                    {
                        const Execution::OrderRejected& orderRejected = *reinterpret_cast<const Execution::OrderRejected*>(rdst.data());
                        OnControlAlgoStatus(orderRejected.OrderHeader.OrderId.StrategyId(), orderRejected.OrderHeader.OrderId.InstrumentId(), Execution::AlgoStatus::Paused);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
    
    void OnControlAlgoStatus(int32_t strategyId, int32_t instrumentId, Execution::AlgoStatus algoStatus)
    {
        Tools::Timestamp now = Tools::Timestamp::UtcNow();
        Socket::SharedArrayEntry<Execution::PositionHeader>& localPositionEntry = _serverContext.GetPositionHeader(strategyId, instrumentId);
        Execution::PositionHeader localPosition = localPositionEntry.GetReadonlyRef();
        localPosition.OrderHeader.ExchangeTimestamp = now;
        localPosition.OrderHeader.NicTimestamp = now;
        localPosition.AlgoStatus = algoStatus;
        localPositionEntry.Write(localPosition);
        int32_t coreGroupId = _serverContext.GetInstrument(instrumentId).Header().CoreGroupId;
        WriteToExecution(strategyId, coreGroupId, localPosition);
    }

    void CancelAllOrders(int32_t clientId)
    {
        int32_t firstGlobalOrderIndex = Execution::OrderIdAllocator::GetFirstGlobalIndex(clientId);
        int32_t lastGlobalOrderIndex = Execution::OrderIdAllocator::GetLastGlobalIndex(clientId);
        for(int globalOrderIndex = firstGlobalOrderIndex; globalOrderIndex <= lastGlobalOrderIndex; globalOrderIndex++)
        {
            Socket::SharedArrayEntry<Execution::OrderTarget>& orderTargetEntry = _serverContext.GetOrderTarget(globalOrderIndex);
            if (orderTargetEntry.IsEmpty())
                continue;
            Execution::OrderTarget orderTarget = orderTargetEntry.GetReadonlyRef(); // dont lock because client may have crashed mid write
            const Execution::OrderState& orderState = _serverContext.GetOrderState(globalOrderIndex).GetReadonlyRef();
            if (orderState.OrderStateStatus == Execution::OrderStateStatus::Active || orderTarget.OrderTargetStatus == Execution::OrderStateStatus::Active)
            {
                orderTarget.OrderTargetStatus = Execution::OrderStateStatus::Active;
                orderTarget.OrderTargetAction = Execution::OrderTargetAction::Cancel;
                orderTarget.OrderHeader.Seq += 1'000'000;
                orderTarget.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
                // Client process is dead, so the server is the slot's sole writer: stamp the cancel in
                // so the vendor's replay-on-ack cancels a still-PendingNew order.
                orderTargetEntry.RecoveryWrite(orderTarget);
                // Enqueue so an already-working order is cancelled now on its segment thread.
                EnqueueOrderTarget(orderTarget);
            }
        }
    }

    void ReadAdmin()
    {
        PollDisconnects();

        std::span<const uint8_t> rdst;
        for(int32_t clientId : _serverSocket.ClientIds())
        {
            while (_serverSocket.TryRead(clientId, Socket::SocketChannel::Admin, rdst) == Socket::ReadStatus::New)
            {
                if (rdst.empty())
                    continue;
                uint8_t msgType = rdst[0];
                switch (msgType)
                {
                    case static_cast<uint8_t>(AllocateType::Instrument):
                    {
                        Provider::AllocateInstrument allocateInstrument = *reinterpret_cast<const Provider::AllocateInstrument*>(rdst.data());
                        OnAllocateInstrument(clientId, allocateInstrument);
                        break;
                    }
                    case static_cast<uint8_t>(Provider::ControlType::AlgoStatus):
                    {
                        const Provider::ControlAlgoStatus& controlAlgoStatus = *reinterpret_cast<const Provider::ControlAlgoStatus*>(rdst.data());
                        OnControlAlgoStatus(controlAlgoStatus.StrategyId, controlAlgoStatus.InstrumentId, controlAlgoStatus.AlgoStatus);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    void OnInstrumentHeader(Data::InstrumentHeader128& instrumentHeader128)
    {
        _serverContext.OnInstrumentHeader(instrumentHeader128);
    }

    void OnQuantityAhead(uint64_t clientOrderId, int32_t quantityAhead)
    {
        int32_t globalOrderIndex = Execution::OrderIdAllocator::GetGlobalIndex(clientOrderId);
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(globalOrderIndex);
        Execution::OrderState& orderState = orderStateEntry.GetRef();
        if (orderState.OrderHeader.OrderId == clientOrderId)
        {
            orderStateEntry.AcquireLock();
            orderState.QuantityAhead = quantityAhead;
            orderStateEntry.ReleaseLock();
        }
    }

    Execution::OrderState OnOrderState(Execution::OrderState& orderState)
    {
        int32_t globalOrderIndex = orderState.OrderHeader.OrderId.GlobalIndex();
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(globalOrderIndex);
        Socket::SharedArrayEntry<Execution::OrderTarget>& orderTargetEntry = _serverContext.GetOrderTarget(globalOrderIndex);
        Execution::OrderState& existingOrderState = orderStateEntry.GetRef();
        const Execution::OrderTarget& existingOrderTarget = orderTargetEntry.GetReadonlyRef();
        bool isSeqInOrder = existingOrderState.OrderStateStatus == Execution::OrderStateStatus::Active && (orderState.OrderHeader.Seq >= existingOrderState.OrderHeader.Seq || orderState.OrderStateStatus == Execution::OrderStateStatus::Done);
        // handle case where exchange cancels order
        bool isSafeToOverwrite = existingOrderTarget.OrderHeader.OrderId == orderState.OrderHeader.OrderId && isSeqInOrder;
        
        if (isSafeToOverwrite)
        {
            orderStateEntry.AcquireLock();
            existingOrderState.OrderHeader.Seq = orderState.OrderHeader.Seq;
            existingOrderState.ExchangeOrderId = orderState.ExchangeOrderId;
            existingOrderState.OrderProfile = orderState.OrderProfile;
            existingOrderState.OrderStateStatus = orderState.OrderStateStatus;
            existingOrderState.QuantityFilled = orderState.QuantityFilled;
            existingOrderState.OrderHeader.ExchangeTimestamp = orderState.OrderHeader.ExchangeTimestamp;
            existingOrderState.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
            orderStateEntry.ReleaseLock();
        }
        WriteToExecution(existingOrderState);
            if (OrderState)
                OrderState(existingOrderState);
        return existingOrderState;
    }

    Execution::OrderRejected OnOrderRejected(Execution::OrderRejected& orderRejected, const std::string& message)
    {
        int32_t globalOrderIndex = Execution::OrderIdAllocator::GetGlobalIndex(orderRejected.OrderHeader.OrderId);
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(globalOrderIndex);
        Execution::OrderState& orderState = orderStateEntry.GetRef();
        if (orderState.OrderHeader.OrderId == orderRejected.OrderHeader.OrderId)
        {
            orderRejected.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
            Reject(orderRejected, message);
            return orderRejected;
        }
        else
        {
            std::cout << "Server::OnOrderRejected: unknown clientOrderId\n"
                      << orderRejected.ToString()
                      << std::endl;
            return Execution::OrderRejected{};
        }
    }

    void Reject(const Execution::OrderRejected& orderRejected, const std::string& message)
    { 
        WriteToExecution(orderRejected);
        if (!orderRejected.OrderRejectedReasons.IsEmpty() && orderRejected.OrderRejectedReasons.IsSubsetOf(Execution::OrderRejected::OrderDiscarded))
            return;
        OnControlAlgoStatus(orderRejected.OrderHeader.OrderId.StrategyId(), orderRejected.OrderHeader.OrderId.InstrumentId(), Execution::AlgoStatus::Paused);
        if (OrderRejected)
            OrderRejected(orderRejected, message);
    }

    // comes from client so parameter is consistent with framework
    void OnOrderTarget(const Execution::OrderTarget& orderTarget)
    {
        int32_t globalOrderIndex = orderTarget.OrderHeader.OrderId.GlobalIndex();
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(globalOrderIndex);
        Execution::OrderState& orderState = orderStateEntry.GetRef();

        Tools::Bitset64 orderRejectedReasons{};

        bool isValid = _riskLayer.ValidateOrder(orderTarget, orderRejectedReasons);
        if (orderTarget.OrderTargetAction == Execution::OrderTargetAction::Create)
        {
            orderStateEntry.AcquireLock();
            orderState = Execution::OrderState
            {
                .Header = Data::Header<Execution::OrderType>(Execution::OrderType::OrderState),
                .OrderHeader = orderTarget.OrderHeader,
                .OrderStateStatus = isValid ? Execution::OrderStateStatus::Active : Execution::OrderStateStatus::Done,
                .OrderProfile = orderTarget.OrderProfile,
                .QuantityFilled = 0,
                .QuantityAhead = 0,
            };
            orderState.OrderHeader.Seq = 0; // indicates new Order but that ordertarget is not acked by exchange
            orderState.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
            orderStateEntry.ReleaseLock();
            WriteToExecution(orderState);
        }
        if (isValid)
        {
            if(OrderTarget)
            {
                OrderTarget(orderTarget);
            }
        }
        else
		{
			Execution::OrderRejected orderRejected
			{
				.Header = Data::Header<Execution::OrderType>(Execution::OrderType::OrderRejected),
				.OrderHeader = orderTarget.OrderHeader,
				.OrderTargetAction = orderTarget.OrderTargetAction,
				.OrderRejectedSource = Execution::OrderRejectedSource::Server,
				.OrderProfile = orderTarget.OrderProfile,
				.OrderRejectedReasons = orderRejectedReasons,
			};
			Reject(orderRejected, "Rejected by Server Risk Layer");
		}
    }

    int32_t OnAllocateInstrument(Provider::AllocateInstrument& allocateInstrument)
    {
        ServerHeader& serverHeader = _serverContext.ServerHeader().GetRef();
        if (allocateInstrument.InstrumentHeaderId >= serverHeader.InstrumentsCount)
        {
            throw std::out_of_range("Server::AllocateInstrument: instrumentHeaderId out of range");
        }

        int32_t instrumentId = _serverContext.AllocateInstrument(allocateInstrument.InstrumentHeaderId);
        allocateInstrument.InstrumentId = instrumentId;

        Data::InstrumentHeader128& header128 = _serverContext.GetInstrumentHeader(allocateInstrument.InstrumentHeaderId).GetRef();
        allocateInstrument.Symbol = header128.Symbology()->ToString();

        OpenInstrumentData(instrumentId, allocateInstrument.Symbol.ToString());

        WriteToAudit(Socket::SocketChannel::Admin, allocateInstrument);

        if (AllocateInstrument)
            AllocateInstrument(allocateInstrument);


        return instrumentId;
    }

    void OnAllocateInstrument(int32_t clientId, Provider::AllocateInstrument& allocateInstrument)
    {   
        ServerHeader& serverHeader = _serverContext.ServerHeader().GetRef();
        if (clientId >= serverHeader.ClientIds.Length())
        {
            throw std::out_of_range("Server::OnInstrumentAllocated: clientId out of range");
        }

        int32_t instrumentId = OnAllocateInstrument(allocateInstrument);

        _serverContext.AllocateInstrument(clientId, instrumentId);

        // Remember this client now trades the instrument's CoreGroup, so ReadExecution polls that channel.
        int32_t coreGroupId = _serverContext.GetInstrument(instrumentId).Header().CoreGroupId;
        _clientIdsByCoreGroupId[static_cast<size_t>(coreGroupId)].AtomicSet(clientId);

        WriteToAdmin(clientId, allocateInstrument);
    }

    Execution::Fill OnFill(Execution::Fill& fill)
    {
        int32_t globalOrderIndex = Execution::OrderIdAllocator::GetGlobalIndex(fill.OrderHeader.OrderId);
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(globalOrderIndex);
        const Execution::OrderState& orderState = orderStateEntry.GetReadonlyRef();
        
        if (orderState.OrderHeader.OrderId != fill.OrderHeader.OrderId)
        {
            throw std::out_of_range("Server::OnFill: unknown clientOrderId");
        }
        // Identity (ClientId/StrategyId/InstrumentId) is packed inside ClientOrderId, and the equality
        // check above guarantees it matches the state's - no re-stamping needed.
        fill.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();

        int32_t strategyId = fill.OrderHeader.OrderId.StrategyId();
        int32_t instrumentId = fill.OrderHeader.OrderId.InstrumentId();

        Data::Instrument& instrument = _serverContext.GetInstrument(instrumentId);

        double multiplier = instrument.Multiplier();
        double tickSize = instrument.TickSize();
        
        // Global Update
        Socket::SharedArrayEntry<Execution::PositionHeader>& serverPositionHeaderEntry = _serverContext.GetPositionHeader(instrumentId);
        Execution::PositionHeader& serverPosition = serverPositionHeaderEntry.GetRef();
        serverPositionHeaderEntry.AcquireLock();
        serverPosition.OnFill(fill, tickSize, multiplier);
        serverPositionHeaderEntry.ReleaseLock();
    
        // Local Update
        Socket::SharedArrayEntry<Execution::PositionHeader>& localPositionHeaderEntry = _serverContext.GetPositionHeader(strategyId, instrumentId);
        Execution::PositionHeader& localPosition = localPositionHeaderEntry.GetRef();
        localPositionHeaderEntry.AcquireLock();
        localPosition.OnFill(fill, tickSize, multiplier);
        localPositionHeaderEntry.ReleaseLock();
        
        int32_t coreGroupId = instrument.Header().CoreGroupId;
        WriteToExecution(strategyId, coreGroupId, fill);
        WriteToExecution(strategyId, coreGroupId, localPosition);
        WriteToAudit(coreGroupId, fill);
        WriteToAudit(coreGroupId, serverPosition);
        if(Fill)
            Fill(fill);
        return fill;
    }

    void OnTrade(const Data::Trade& trade)
    {
        WriteToInstrumentData(trade);
    }

    // Opens (once) the per-instrument broadcast ring this server writes market data to.
    void OpenInstrumentData(int32_t instrumentId, const std::string& symbol)
    {
        if (_instrumentData[static_cast<size_t>(instrumentId)])
            return;
        std::string name = Socket::SocketChannel::GetInstrumentDataName(ServerName.string(), symbol);
        _instrumentData[static_cast<size_t>(instrumentId)] = std::make_unique<Socket::WriteOnlySocket>(name, Socket::SharedMemory::CreateOrOpen(name, Socket::SocketChannel::InstrumentDataChannelLength));
    }

    // Broadcast a trade/settlement tick verbatim to the instrument's ring.
    template <typename T> requires Tools::PlainOldData<T>
    void WriteToInstrumentData(const T& tick)
    {
        int32_t instrumentId = tick.TickHeader.InstrumentId;
        if (_instrumentData[static_cast<size_t>(instrumentId)])
            _instrumentData[static_cast<size_t>(instrumentId)]->Write(tick);
    }

    void OnMarketByPrice(const Data::MarketByPrice& marketByPrice, std::span<uint8_t> src)
    {
        int32_t instrumentId = marketByPrice.TickHeader.InstrumentId;
        Socket::SharedArrayEntry<Data::MarketByPrice64>& entry = _serverContext.GetMarketByPrice64(instrumentId);
        Data::MarketByPrice64& mbp64 = entry.GetRef();

        // Snapshot->update conversion writes here. Must outlive the Write() below, since deltaSpan
        // points into it on the snapshot path, so it lives at function scope (not inside the branch).
        // Worst case the diff touches book + snapshot levels = up to 128 per side.
        alignas(Data::MarketByPrice) uint8_t updateBuffer[Data::MarketByPrice::SizeOf(128, 128)];
        std::span<uint8_t> deltaSpan;
        bool isDeltas = false;

        if (marketByPrice.TickHeader.TickType == Data::TickType::MarketByPriceSnapshot)
        {
            alignas(Data::MarketByPrice) uint8_t pastBuffer[Data::MarketByPrice::SizeOf(64, 64)];
            std::span<uint8_t> pastSpan(pastBuffer, sizeof(pastBuffer));
            mbp64.CopyToSnapshot(instrumentId, pastSpan);

            std::span<uint8_t> futureSpan = src;

            deltaSpan = std::span<uint8_t>(updateBuffer, sizeof(updateBuffer));
            Data::MarketByPrice::SnapshotAsUpdate(pastSpan, futureSpan, deltaSpan); // shrinks deltaSpan to the update

            entry.AcquireLock();
            isDeltas = mbp64.TrySetAsDeltas(deltaSpan);
            entry.ReleaseLock();
        }
        else if (marketByPrice.TickHeader.TickType == Data::TickType::MarketByPriceUpdate)
        {
            deltaSpan = src;

            entry.AcquireLock();
            isDeltas = mbp64.TrySetAsDeltas(deltaSpan);
            entry.ReleaseLock();
        }
        else if (marketByPrice.TickHeader.TickType == Data::TickType::MarketByPriceDelta)
        {
            deltaSpan = src;
            entry.AcquireLock();
            isDeltas = mbp64.TrySet(deltaSpan);
            entry.ReleaseLock();
        }

        if (!isDeltas)
            return;

        _serverContext.ServerHeader().GetRef().Timestamp = marketByPrice.TickHeader.NicTimestamp;

        if (_instrumentData[static_cast<size_t>(instrumentId)])
            _instrumentData[static_cast<size_t>(instrumentId)]->Write(std::span<const uint8_t>(deltaSpan.data(), deltaSpan.size()));
    }

    // Canonical return-channel writer. EVERY write to a CoreGroup channel (fill, state, reject,
    // position) funnels through here so the per-CoreGroup spinlock serialises the segment's RX
    // thread (the near-constant holder) against the rare send/admin/cancel writer on the same CCD.
    // Without the lock on THIS (hot) path the rare concurrent reject would tear the SPSC ring.
    template <typename T> requires Tools::PlainOldData<T>
    void WriteToExecution(int32_t clientId, int32_t coreGroupId, const T& value)
    {
        Tools::RAIISpinLock lock(_recvFromExchangeLocks[static_cast<size_t>(coreGroupId)].Flag);
        _serverSocket.Write(clientId, coreGroupId, value);
    }

    // Convenience overload for the order/admin/reject paths: derive (clientId, CoreGroupId) from the
    // message's OrderHeader, then funnel through the locked writer above.
    template <typename T> requires Tools::PlainOldData<T>
    void WriteToExecution(const T& value)
    {
        const Execution::OrderHeader& orderHeader = value.OrderHeader;
        int32_t coreGroupId = _serverContext.GetInstrument(orderHeader.OrderId.InstrumentId()).Header().CoreGroupId;
        WriteToExecution(orderHeader.OrderId.ClientId(), coreGroupId, value);
    }

    // Per-CoreGroup audit: channelId == CoreGroupId (0 = admin). Each segment's RX thread owns its
    // own audit channel and admin owns channel 0, so every channel is single-writer => no lock and
    // no cross-CCD bounce on the fill path.
    template <typename T> requires Tools::PlainOldData<T>
    void WriteToAudit(int32_t channelId, const T& value)
    {
        _audit.Write(channelId, value);
    }

    template <typename T> requires Tools::PlainOldData<T>
    void WriteToAdmin(int32_t clientId, const T& value)
    {
        _serverSocket.Write(clientId, Socket::SocketChannel::Admin, value);
    }

    void Stop()
    {
        _serverSocket.Stop();
    }

    void Dispose()
    {
        _serverSocket.Dispose();
    }

private:

    void OnClientAllocated(const Socket::SocketHeader& socketHeader)
    {       
        _loggingServer.Write(socketHeader);

        AllocateClient allocClient
        {
            .Header = Data::Header<AllocateType>(AllocateType::Client),
            .ClientId = socketHeader.ClientId,
            .ClientName = socketHeader.ClientName
        };
        _serverSocket.Write(socketHeader.ClientId, Socket::SocketChannel::Admin, allocClient);
    }

    void OnClientDeallocated(const Socket::SocketHeader& socketHeader)
    {
        CancelAllOrders(socketHeader.ClientId);

        Socket::SocketHeader clientSocketHeaderCopy = socketHeader;
        clientSocketHeaderCopy.ClientToServerChannelCount = 0;
        clientSocketHeaderCopy.ServerToClientChannelCount = 0;
        _loggingServer.Write(clientSocketHeaderCopy); // this is the close signal for logger
    }
};

}
//END_FILE HFT/Provider/Server.hpp