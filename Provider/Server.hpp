//BEGIN_FILE HFT/Provider/Server.hpp
#pragma once

#include "Bitset.hpp"
#include "Context.hpp"
#include "Instrument.hpp"
#include "Loggable.hpp"
#include "OrderIdAllocator.hpp"
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

    bool _isDisposed = false;

public:
    // A silent default: in the dispatchers hid a real bug on the C# side for weeks — a zeroed
    // Header::Type made every risk-limit edit a no-op with no reject and no log line. Counted, not
    // swallowed.
    uint64_t UnknownAdminMessages = 0;
    uint64_t UnknownExecutionMessages = 0;


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
        .Persistance = true,
    };

    std::function<void(const Execution::OrderTarget&)> OrderTarget;
    std::function<void(const Execution::OrderState&)> OrderState;
    std::function<void(const Execution::OrderRejected&, const std::string&)> OrderRejected;
    std::function<void(const Execution::Fill&)> Fill;


    std::function<void(const AllocateInstrument&)> AllocateInstrument;
    std::function<void(const Socket::SocketHeader&)> AllocateClient;

    std::function<void(int32_t)> ClientOpened;
    std::function<void(int32_t)> ClientClosed;



    Server(const ServerHeader& serverHeader)
    : ServerName(serverHeader.ServerName.ToString()),
      _serverHeaderBox(ServerContext::Connect(serverHeader)),
      _serverSocket(ServerName.string(), serverHeader.ClientIds.Length()),
      _serverContext(ServerName, Tools::Access::Write),
      _loggingServer(ServerName.string() + ".server", _serverContext.LoggingServerName, {Socket::SocketChannel::AdminChannelLength}, {Socket::SocketChannel::AdminChannelLength}),
      _audit(ServerName.string() + ".audit", _serverContext.LoggingServerName, Socket::SocketChannel::BuildChannelLengths(serverHeader.CoreGroupIds), {Socket::SocketChannel::AdminChannelLength}),
      _riskLayer(_serverContext, Execution::OrderRejectedSource::Server),
      _loggableManager()
    {
        InitDirectories();

        // Before anything else: LoadClients() runs between construction and Connect(), and
        // CreateDetatchedClient() refuses to build a Detached socket while this is false.
        _serverSocket.Persistance = serverHeader.Persistance;

        _instrumentData.resize(static_cast<size_t>(serverHeader.InstrumentIds.Length()));
        // +1: channel index == CoreGroupId, so we need slots 0..HighestSet() (matches BuildChannelLengths).
        _clientIdsByCoreGroupId.resize(static_cast<size_t>(serverHeader.CoreGroupIds.HighestSet() + 1));

        // One OrderTarget injection queue per EXECUTION CoreGroup (admin carries no order targets).
        for (int32_t coreGroupId : serverHeader.CoreGroupIds)
            if (coreGroupId != Socket::SocketChannel::Admin)
                _orderTargetQueues[static_cast<size_t>(coreGroupId)] = std::make_unique<Tools::ByteQueue>(Tools::Memory::SmallPageLength);

        _serverSocket.AllocateClientId = [this](const Socket::SocketHeader& socketHeader) {
            return _serverContext.AllocateClientId(socketHeader); 
        };

        _serverSocket.DeallocateClient = [this](int32_t clientId) {
            return _serverContext.DeallocateClient(clientId);
        };

        _serverSocket.ClientAllocated = [this](const Socket::SocketHeader& socketHeader) { 
            this->OnClientAllocated(socketHeader);
        };

        // never called with Persistance
        _serverSocket.ClientDeallocated = [this](const Socket::SocketHeader& socketHeader) { 
            this->OnClientDeallocated(socketHeader);
        };

        
        _serverSocket.ClientOpened = [this](int32_t clientId) { 
            this->OnClientOpened(clientId);
        };
        
        _serverSocket.ClientClosed = [this](int32_t clientId) { 
            this->OnClientClosed(clientId);
        };
        
        

        _loggingServer.Connect();
        _audit.Connect();

    }

    // Starts the listen thread. Call after LoadClients()/LoadInstruments() so the poll thread
    // cannot race them for the same clientId. Persistance comes from the ServerHeader, not here.
    void Connect()
    {
        _serverSocket.Listen();
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
                        UnknownExecutionMessages++;
                        break;
                }
            }
        }
    }

    void OnRiskLimit(const Execution::RiskLimit& riskLimit)
    {
        // The sender read-modify-writes the whole struct, so the running working quantities in its
        // copy are as stale as the moment it opened the edit dialog. They are server-owned state, not
        // config — carry the live ones across or an operator editing a limit silently rewinds them.
        Execution::RiskLimit riskLimitCopy = riskLimit;
        const Execution::RiskLimit& existing = _serverContext.GetRiskLimit(riskLimit.InstrumentId).GetReadonlyRef();
        riskLimitCopy.WorstLongWorkingQuantity = existing.WorstLongWorkingQuantity;
        riskLimitCopy.WorstShortWorkingQuantity = existing.WorstShortWorkingQuantity;

        _serverContext.GetRiskLimit(riskLimit.InstrumentId).Write(riskLimitCopy);
        if (riskLimitCopy.StrategyId >= 0)
            WriteToExecution(riskLimitCopy.StrategyId, _serverContext.GetInstrument(riskLimit.InstrumentId).Header().CoreGroupId, riskLimitCopy);
        SaveRiskLimit(riskLimit.InstrumentId, riskLimitCopy);
    }

    void SaveRiskLimit(int32_t instrumentId, const Execution::RiskLimit& riskLimit)
    {
        std::string symbol = _serverContext.GetInstrument(instrumentId).Symbol();
        std::filesystem::path riskLimitFilePath = Context::GetRiskLimitsFilePath(_serverContext.DirectoryPath, symbol);
        std::string riskLimitLine = Tools::Json::SerializeToLine(riskLimit);
        std::cout << "ServerSimulator::SaveRiskLimit(" << riskLimitFilePath << "):" << std::endl << riskLimitLine << std::endl;
        std::ofstream outFile(riskLimitFilePath, std::ios::app);
        outFile << riskLimitLine << std::endl;
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
        // Context keys order rows by OrderId, so build a probe whose GlobalIndex is
        // (clientId, localIndex) — same row, no extra accessor. Matches C#.
        for (int32_t localIndex = 0; localIndex < Execution::OrderIdAllocator::OrdersPerClient; localIndex++)
        {
            Execution::OrderId probe = Execution::OrderId().ClientId(clientId).LocalIndex(localIndex);

            Socket::SharedArrayEntry<Execution::OrderTarget>& orderTargetEntry = _serverContext.GetOrderTarget(probe);
            if (orderTargetEntry.IsEmpty())
                continue;
            Execution::OrderTarget orderTarget = orderTargetEntry.GetReadonlyRef(); // dont lock because client may have crashed mid write
            const Execution::OrderState& orderState = _serverContext.GetOrderState(probe).GetReadonlyRef();
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
                    case static_cast<uint8_t>(Execution::OrderType::RiskLimit):
                    {
                        const Execution::RiskLimit& riskLimit = *reinterpret_cast<const Execution::RiskLimit*>(rdst.data());
                        OnRiskLimit(riskLimit);
                        break;
                    }
                    default:
                        UnknownAdminMessages++;
                        break;
                }
            }
        }
    }

    void OnInstrumentHeader(Data::InstrumentHeader128& instrumentHeader128)
    {
        _serverContext.OnInstrumentHeader(instrumentHeader128);
    }

    void OnQuantityAhead(Execution::OrderId clientOrderId, int32_t quantityAhead)
    {
        Execution::OrderState& orderState = _serverContext.GetOrderState(clientOrderId).GetRef();
        if (orderState.OrderHeader.OrderId == clientOrderId)
        {
            // Quick write, its atomic, do not lock, it would contend with OnOrderState
            orderState.QuantityAhead = quantityAhead;
        }
    }

    Execution::OrderState OnOrderState(Execution::OrderState& orderState)
    {
        Execution::OrderState& existingOrderState = WriteOrderState(orderState);
        WriteToExecution(existingOrderState);
        if (OrderState)
            OrderState(existingOrderState);
        return existingOrderState;
    }

    // Row write + risk ledger ONLY - safe inside OnFill's position-lock envelope. The forward and
    // callback live in the public OnOrderState/OnFill, after every lock is released; calling the
    // public method from OnFill would send the state twice, from inside the lock.
    Execution::OrderState& WriteOrderState(Execution::OrderState& orderState)
    {
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(orderState.OrderHeader.OrderId);
        Socket::SharedArrayEntry<Execution::OrderTarget>& orderTargetEntry = _serverContext.GetOrderTarget(orderState.OrderHeader.OrderId);
        Execution::OrderState& existingOrderState = orderStateEntry.GetRef();
        const Execution::OrderTarget& existingOrderTarget = orderTargetEntry.GetReadonlyRef();
        bool isSeqInOrder = existingOrderState.OrderStateStatus == Execution::OrderStateStatus::Active && (orderState.OrderHeader.Seq >= existingOrderState.OrderHeader.Seq || orderState.OrderStateStatus == Execution::OrderStateStatus::Done);
        // handle case where exchange cancels order
        bool isSafeToOverwrite = existingOrderTarget.OrderHeader.OrderId == orderState.OrderHeader.OrderId && isSeqInOrder;
        
        if (isSafeToOverwrite)
        {
            int32_t beforeAckedOrderQuantity = existingOrderState.OrderProfile.Quantity;
            int32_t quantityFilled = std::abs(existingOrderState.QuantityFilled) > std::abs(orderState.QuantityFilled) ? existingOrderState.QuantityFilled : orderState.QuantityFilled;
            orderStateEntry.AcquireLock();
            existingOrderState.OrderHeader.Seq = orderState.OrderHeader.Seq;
            existingOrderState.ExchangeOrderId = orderState.ExchangeOrderId;
            existingOrderState.OrderProfile = orderState.OrderProfile;
            existingOrderState.OrderStateStatus = orderState.OrderStateStatus;
            existingOrderState.OrderStateReason = orderState.OrderStateReason;
            existingOrderState.QuantityFilled = quantityFilled;
            existingOrderState.OrderHeader.ExchangeTimestamp = orderState.OrderHeader.ExchangeTimestamp;
            existingOrderState.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
            orderStateEntry.ReleaseLock();
            _riskLayer.OnOrderState(existingOrderState, beforeAckedOrderQuantity);
        }
        return existingOrderState;
    }

    static constexpr uint64_t _orderNotFound = 1ULL << static_cast<int32_t>(Execution::OrderRejectedReason::OrderNotFound);

    Execution::OrderRejected OnOrderRejected(Execution::OrderRejected& orderRejected, const std::string& message)
    {
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(orderRejected.OrderHeader.OrderId);
        Execution::OrderState& orderState = orderStateEntry.GetRef();

        // The exchange saying "no such order" for a slot we already know is finished is not news, and
        // it must not pause the algo. Restate it as StateIsDone, which OrderDiscarded covers.
        if (orderState.OrderStateStatus == Execution::OrderStateStatus::Done
            && orderRejected.OrderRejectedReasons.Raw() == _orderNotFound)
        {
            orderRejected.OrderRejectedReasons = Tools::Bitset64(1ULL << static_cast<int32_t>(Execution::OrderRejectedReason::StateIsDone));
        }

        if (orderState.OrderHeader.OrderId == orderRejected.OrderHeader.OrderId)
        {
            orderRejected.OrderHeader.NicTimestamp = Tools::Timestamp::UtcNow();
            _riskLayer.OnOrderRejected(orderRejected);
            Reject(orderRejected, message);
            return orderRejected;
        }
        else
        {
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
        Socket::SharedArrayEntry<Execution::OrderState>& orderStateEntry = _serverContext.GetOrderState(orderTarget.OrderHeader.OrderId);
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
                .OrderProfile = orderTarget.OrderProfile,
                .TimeInForce = orderTarget.TimeInForce,
                .OrderStateStatus = isValid ? Execution::OrderStateStatus::Active : Execution::OrderStateStatus::Done,
                // Seq 0 already means "not acked"; naming it lets the RiskLayer retire hooks tell
                // PendingNew from an ack without inferring it from the sequence.
                .OrderStateReason = isValid ? Execution::OrderStateReason::PendingNew : Execution::OrderStateReason::Rejected,
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
        allocateInstrument.ExchangeInstrumentId = header128.AsInstrumentHeader().ExchangeInstrumentId;

        if (_instrumentData[static_cast<size_t>(instrumentId)])
            return instrumentId; // already attached + seeded

        OpenInstrumentData(instrumentId, allocateInstrument.Symbol.ToString());

        WriteToAudit(Socket::SocketChannel::Admin, allocateInstrument);

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

        // Strategy 0 is the house book: it holds the union of every client's allocations, which is what
        // lets a server workspace trade anything anyone can see without a validator special-case. It gets
        // no core-group poll bit (no socket) and no admin echo (nobody listening). See Spec.md.
        if (clientId != Execution::OrderIdAllocator::ServerStrategyId)
            _serverContext.AllocateInstrument(Execution::OrderIdAllocator::ServerStrategyId, instrumentId);

        // Remember this client now trades the instrument's CoreGroup, so ReadExecution polls that channel.
        int32_t coreGroupId = _serverContext.GetInstrument(instrumentId).Header().CoreGroupId;
        _clientIdsByCoreGroupId[static_cast<size_t>(coreGroupId)].AtomicSet(clientId);

        WriteToAdmin(clientId, allocateInstrument);

        // After the work, not before: on entry InstrumentId is still -1 and Symbol is empty.
        std::cout << ServerName.string() << "::OnAllocateInstrument()\n" << allocateInstrument.ToString() << std::endl;

        if (AllocateInstrument)
            AllocateInstrument(allocateInstrument);
    }

    // The fill arrives PAIRED with the order state it produced (one ExecutionReport carries both):
    // routing them through two independent calls let a strategy tick read a fresh position but a
    // stale QuantityFilled, size an amend to the wrong total, and cancel its own order. The vendor
    // session must hand this method the pair.
    Execution::Fill OnFill(Execution::OrderState& orderState, Execution::Fill& fill)
    {
        const Execution::OrderState& existingOrderState = _serverContext.GetOrderState(fill.OrderHeader.OrderId).GetReadonlyRef();

        if (existingOrderState.OrderHeader.OrderId != fill.OrderHeader.OrderId)
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

        Socket::SharedArrayEntry<Execution::PositionHeader>& serverPositionHeaderEntry = _serverContext.GetPositionHeader(instrumentId);
        Execution::PositionHeader& serverPosition = serverPositionHeaderEntry.GetRef();

        Socket::SharedArrayEntry<Execution::PositionHeader>& localPositionHeaderEntry = _serverContext.GetPositionHeader(strategyId, instrumentId);
        Execution::PositionHeader& localPosition = localPositionHeaderEntry.GetRef();

        // The fill is atomic: order state, both position rows and the risk ledger move under ONE
        // envelope (server row acquired first, then local; released in reverse), so no reader can
        // see the position with a stale filled count. WriteOrderState, not OnOrderState - the
        // forward and callbacks run after release below.
        serverPositionHeaderEntry.AcquireLock();
        localPositionHeaderEntry.AcquireLock();

        WriteOrderState(orderState);
        serverPosition.OnFill(fill, tickSize, multiplier);
        localPosition.OnFill(fill, tickSize, multiplier);
        _riskLayer.OnFill(fill);

        localPositionHeaderEntry.ReleaseLock();
        serverPositionHeaderEntry.ReleaseLock();

        int32_t coreGroupId = instrument.Header().CoreGroupId;
        WriteToExecution(existingOrderState);
        WriteToExecution(strategyId, coreGroupId, fill);
        WriteToExecution(strategyId, coreGroupId, localPosition);

        WriteToAudit(coreGroupId, fill);
        WriteToAudit(coreGroupId, serverPosition);
        if (OrderState)
            OrderState(existingOrderState);
        if (Fill)
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
        _instrumentData[static_cast<size_t>(instrumentId)]->Recover();
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
        if (_isDisposed)
            return;
        _isDisposed = true;

        _serverSocket.Dispose();

        for (std::unique_ptr<Socket::WriteOnlySocket>& instrumentData : _instrumentData)
        {
            if (instrumentData)
                instrumentData->Dispose();
        }

        _audit.Dispose();
        _loggingServer.Dispose();
        // _serverContext and _serverHeaderBox are RAII here; C# disposes them explicitly.
    }

    void LoadClients(const Tools::Timestamp date)
    {
        std::filesystem::path clientsFilePath = _serverContext.GetClientsFilePath(date);
        if (!std::filesystem::exists(clientsFilePath))
            return;
        std::ifstream clientsFile(clientsFilePath);
        std::string line;
        while (std::getline(clientsFile, line))
        {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;

            Socket::SocketHeader socketHeader = Tools::Json::Deserialize<Socket::SocketHeader>(line);
            socketHeader.ClientId = _serverContext.AllocateClientId(socketHeader);
            _serverSocket.CreateDetatchedClient(socketHeader);
            OnClientAllocated(socketHeader);
        }
    }

    void LoadInstruments(const Tools::Timestamp date)
    {
        std::filesystem::path instrumentsFilePath = _serverContext.GetInstrumentsFilePath(date);
        if (!std::filesystem::exists(instrumentsFilePath))
            return;
        std::ifstream instrumentsFile(instrumentsFilePath);
        std::string line;
        while (std::getline(instrumentsFile, line))
        {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;

            Provider::AllocateInstrument allocateInstrument = Tools::Json::Deserialize<Provider::AllocateInstrument>(line);
            allocateInstrument.InstrumentHeaderId = _serverContext.GetInstrumentHeaderIdByExchangeInstrumentId(allocateInstrument.ExchangeInstrumentId);
            if (allocateInstrument.InstrumentHeaderId < 0)
            {
                std::cout << "Server::LoadInstruments: " << allocateInstrument.Symbol.ToString() << " (exchange instrument id "
                          << allocateInstrument.ExchangeInstrumentId << ") is no longer listed; not restored." << std::endl;
                continue;
            }
            OnAllocateInstrument(allocateInstrument.ClientId, allocateInstrument);
        }
    }

    void SaveClient(const Socket::SocketHeader& socketHeader, const Tools::Timestamp date)
    {
        std::filesystem::path clientsFilePath = _serverContext.GetClientsFilePath(date);
        std::string line = Tools::Json::SerializeToLine(socketHeader);
        std::ofstream clientsFile(clientsFilePath, std::ios::app);
        clientsFile << line << std::endl;
    }

    void SaveInstrument(const Provider::AllocateInstrument& allocateInstrument, const Tools::Timestamp date)
    {
        std::filesystem::path instrumentsFilePath = _serverContext.GetInstrumentsFilePath(date);
        std::string line = Tools::Json::SerializeToLine(allocateInstrument);
        std::ofstream instrumentsFile(instrumentsFilePath, std::ios::app);
        instrumentsFile << line << std::endl;
    }

private:

    static inline const std::array<const char*, 7> SubDirectories =
        { "Alerts", "Audit", "Fills", "Positions", "Series", "Clients", "Instruments" };

    // Outside Realtime the server's own output directories are emptied, so a backtest starts from a
    // clean slate rather than replaying yesterday's clients, instruments, fills and positions.
    void InitDirectories()
    {
        for (const char* subDirectory : SubDirectories)
        {
            std::filesystem::path subDirectoryPath = ServerName / subDirectory;
            std::filesystem::create_directories(subDirectoryPath);

            if (Clock::Mode == ClockMode::Realtime)
                continue;

            
            //Just in case we accidently run a simulation on live server
            throw std::runtime_error("Server::InitDirectories: Simulation on live server is not allowed. Please check the server name and run again.");
            std::error_code error;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(subDirectoryPath, error))
            {
                if (entry.is_regular_file(error))
                    std::filesystem::remove(entry.path(), error);
            }
            
        }
    }

    void OnClientAllocated(const Socket::SocketHeader& socketHeader)
    {
        std::cout << ServerName.string() << "::OnClientAllocated()\n" << socketHeader.ToString() << std::endl;

        // this is the open signal for the logger
        _loggingServer.Write(socketHeader);
        if(AllocateClient)
            AllocateClient(socketHeader);
    }

    void OnClientClosed(int32_t clientId)
    {
        CancelAllOrders(clientId);

        int32_t coreGroupsIdsCount = static_cast<int32_t>(_clientIdsByCoreGroupId.size());
        for(int32_t coreGroupId = 0; coreGroupId < coreGroupsIdsCount; coreGroupId++)
        {
            _clientIdsByCoreGroupId[static_cast<size_t>(coreGroupId)].AtomicClear(clientId);
        }

        if(ClientClosed)
            ClientClosed(clientId);
    }

    void OnClientOpened(int32_t clientId)
    {
        for (int32_t instrumentId : _serverContext.GetInstrumentIdsByClientId(clientId).GetReadonlyRef())
            _clientIdsByCoreGroupId[_serverContext.GetInstrument(instrumentId).Header().CoreGroupId].AtomicSet(clientId);
    }

    void OnClientDeallocated(const Socket::SocketHeader& socketHeader)
    {
        Socket::SocketHeader clientSocketHeaderCopy = socketHeader;
        clientSocketHeaderCopy.ClientToServerChannelCount = 0;
        clientSocketHeaderCopy.ServerToClientChannelCount = 0;
         // this is the close signal for logger
        _loggingServer.Write(clientSocketHeaderCopy);
    }
};

}
//END_FILE HFT/Provider/Server.hpp