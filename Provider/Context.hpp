//BEGIN_FILE HFT/Provider/Context.hpp
#pragma once

#include "OrderIdAllocator.hpp"
#include "Order.hpp"

#include "SeqLock.hpp"
#include "Socket.hpp"
#include "SharedArray.hpp"
#include "Instrument.hpp"
#include "Bitset.hpp"
#include "Position.hpp"
#include <filesystem>
#include "Allocate.hpp"
#include "Timestamp.hpp"
#include "Tools.hpp"
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unistd.h>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "MarketByPrice.hpp"

namespace Provider
{

enum ClockMode : uint8_t
{
	Simulation,
	Realtime
};

class Clock
{
	static inline Tools::Timestamp s_utcNow;
public:
	static inline ClockMode Mode = ClockMode::Simulation;
	static inline void SetUtcNow(Tools::Timestamp utcNow)
	{
		s_utcNow = utcNow;
	}
	static inline Tools::Timestamp GetUtcNow()
	{
		if (Mode == ClockMode::Realtime)
			return Tools::Timestamp::UtcNow();

		return s_utcNow;
	}
};

class ContextManager; // Forward declaration

class Context
{
public:
	// The one mount every durable path on this machine hangs off: servers, strategies, venue logs and
	// session state alike. Named here so no caller ever spells it out again.
	static inline const std::filesystem::path RootDirectoryPath = "/mnt/S";

	const std::filesystem::path ServerName;
	const std::filesystem::path LoggingServerName;
	const std::filesystem::path DirectoryPath;

	const Tools::Access ServerAccess;
	const Tools::Access ClientAccess;

	static std::filesystem::path GetAlertsFilePath(const std::filesystem::path& directoryPath, Tools::Timestamp date)
	{
		std::string fileName = date.ToDateString() + ".alert";
		return GetAlertsDirectoryPath(directoryPath) / fileName;
	}

	static std::filesystem::path GetAlertsDirectoryPath(const std::filesystem::path& directoryPath)
	{
		return directoryPath / "Alerts";
	}

	static std::filesystem::path GetPositionFilePath(const std::filesystem::path& directoryPath, const std::string& symbol)
	{
		return directoryPath / "Positions" / (symbol + ".position");
	}

	static std::filesystem::path GetFillsFilePath(const std::filesystem::path& directoryPath, const std::string& symbol)
	{
		return directoryPath / "Fills" / (symbol + ".fill");
	}

	static std::filesystem::path GetAuditFilePath(const std::filesystem::path& directoryPath, Tools::Timestamp date)
	{
		std::string fileName = date.ToDateString() + ".audit";
		return GetAuditDirectoryPath(directoryPath) / fileName;
	}

	static std::filesystem::path GetAuditDirectoryPath(const std::filesystem::path& directoryPath)
	{
		return directoryPath / "Audit";
	}

	static std::filesystem::path GetWorkspacesDirectoryPath(const std::filesystem::path& directoryPath)
	{
		return directoryPath / "Workspaces";
	}

	static std::filesystem::path GetWorkspaceFilePath(const std::filesystem::path& directoryPath, const std::string& workspaceName)
	{
		return GetWorkspacesDirectoryPath(directoryPath) / (workspaceName + ".workspace");
	}

	static std::filesystem::path GetRiskLimitsFilePath(const std::filesystem::path& directoryPath, const std::string& symbol)
	{
		return directoryPath / "RiskLimits" / (symbol + ".risklimit");
	}

	static std::filesystem::path GetLoggingServerDirectoryPath(const std::filesystem::path& directoryPath)
	{
		return directoryPath / "LoggingServer";
	}

	// The Strategies tree for the current clock mode. ClientContext::GetDirectoryPath is the public
	// spelling; it lives here because ServerContext is declared first and needs it for strategy 0.
	static std::filesystem::path GetStrategyDirectoryPath(const std::string& clientName)
	{
		std::string modeStr = Clock::Mode == ClockMode::Simulation ? "Simulation" : "Realtime";
		return RootDirectoryPath / "Strategies" / modeStr / clientName;
	}

	const std::filesystem::path FillsDirectoryPath;
	const std::filesystem::path PositionsDirectoryPath;
	const std::filesystem::path AlertsDirectoryPath;
	const std::filesystem::path RiskLimitsDirectoryPath;
	const std::filesystem::path AuditDirectoryPath;
	const std::filesystem::path SeriesDirectoryPath;
	const std::filesystem::path WorkspaceDirectoryPath;

protected:

	// server
	Socket::LetterBox<Provider::ServerHeader> _serverHeaderBox;
	Socket::SharedArray<Socket::SocketHeader> _clientSocketHeaders;

	// instruments
	Socket::SharedArray<Data::InstrumentHeader128> _instrumentHeaders;
	Socket::SharedArray<int32_t> _instrumentHeaderIdByInstrumentId;
	
	// subscriptions
	Socket::SharedArray<Tools::Bitset64> _instrumentIdsByClientId;
	Socket::SharedArray<Tools::Bitset64> _clientIdsByInstrumentId;
	Socket::SharedArray<Data::MarketByPrice64> _marketsByPrice;

	// execution
	Socket::SharedArray<Execution::RiskLimit> _riskLimits;
	Socket::SharedArray<Execution::OrderState> _orderStates;
	Socket::SharedArray<Execution::OrderTarget> _orderTargets;
	// Reserved exposure per order slot. Server-owned: the RiskLayer is the only writer, and it runs
	// server-side only. Keyed by OrderId::GlobalIndex, so a client and the server name the same row.
	Socket::SharedArray<Execution::OrderRisk> _orderRisks;
	
	// positions
	Socket::SharedArray<Execution::PositionHeader> _localPositionHeaders;

	std::vector<std::unique_ptr<Data::Instrument>> _instruments;
	std::vector<std::unique_ptr<Position>> _positions;

protected:
	Context(const std::filesystem::path& serverName, const std::filesystem::path& directoryPath, Tools::Access serverAccess, Tools::Access clientAccess)
    : 
    ServerName(serverName),
    LoggingServerName(GetLoggingServerDirectoryPath(ServerName)),
    DirectoryPath(directoryPath),
    ServerAccess(serverAccess),
    ClientAccess(clientAccess),
    FillsDirectoryPath(directoryPath / "Fills"),
    PositionsDirectoryPath(directoryPath / "Positions"),
    AlertsDirectoryPath(directoryPath / "Alerts"),
    RiskLimitsDirectoryPath(directoryPath / "RiskLimits"),
    AuditDirectoryPath(GetAuditDirectoryPath(directoryPath)),
    SeriesDirectoryPath(directoryPath / "Series"),
    WorkspaceDirectoryPath(directoryPath / "Workspaces"),
    _serverHeaderBox(serverName / "ServerHeader", ServerAccess),
    _clientSocketHeaders((EnsureConnected(), serverName / "ClientHeaders"), ServerHeader().GetReadonlyRef().ClientIds.Length(), ServerAccess),
    _instrumentHeaders(serverName / "InstrumentHeaders", ServerHeader().GetReadonlyRef().InstrumentsCapacity, ServerAccess),
    _instrumentHeaderIdByInstrumentId(serverName / "InstrumentHeaderIdByInstrumentId", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    _instrumentIdsByClientId(serverName / "InstrumentIdsByClientId", ServerHeader().GetReadonlyRef().ClientIds.Length(), ServerAccess),
    _clientIdsByInstrumentId(serverName / "ClientIdsByInstrumentId", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    _marketsByPrice(directoryPath / "MarketsByPrice", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), (directoryPath == serverName) ? serverAccess : clientAccess),
    _riskLimits(serverName / "RiskLimits", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    _orderStates(serverName / "OrderStates", ServerHeader().GetReadonlyRef().OrdersCapacity(), ServerAccess, false),
    _orderTargets(serverName / "OrderTargets", ServerHeader().GetReadonlyRef().OrdersCapacity(), ClientAccess, false),
    _orderRisks(serverName / "OrderRisks", ServerHeader().GetReadonlyRef().OrdersCapacity(), ServerAccess, false),
    _localPositionHeaders(serverName / "LocalPositionHeaders", ServerHeader().GetReadonlyRef().LocalPositionsCapacity(), ServerAccess, false)
	{
        std::cout << Tools::GetTypeName(typeid(*this)) << "(" << serverName << ", " << directoryPath.string() << ", "
                  << static_cast<int>(serverAccess) << ", " << static_cast<int>(clientAccess) << ")" << std::endl;

        std::filesystem::create_directories(FillsDirectoryPath);
        std::filesystem::create_directories(PositionsDirectoryPath);
        std::filesystem::create_directories(AlertsDirectoryPath);
        std::filesystem::create_directories(RiskLimitsDirectoryPath);
        std::filesystem::create_directories(AuditDirectoryPath);
        std::filesystem::create_directories(SeriesDirectoryPath);
        std::filesystem::create_directories(WorkspaceDirectoryPath);

		_instruments.resize(static_cast<size_t>(ServerHeader().GetReadonlyRef().InstrumentIds.Length()));
		_positions.resize(static_cast<size_t>(ServerHeader().GetReadonlyRef().InstrumentIds.Length()));
	}

	void EnsureConnected()
	{
		Provider::ServerHeader serverHeader;
		while (!_serverHeaderBox.TryPeek(serverHeader))
        {
            std::cout << "Context " << ServerName << " failed to connect to server " << ServerName << " ... will try again." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
	}

	ALWAYS_INLINE int32_t GetLocalPositionIndex(int32_t clientId, int32_t instrumentId)
	{
		ThrowIfClientIdOutOfRange(clientId);
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return (clientId * ServerHeader().GetReadonlyRef().InstrumentIds.Length()) + instrumentId;
	}

public:
	virtual ~Context() = default;

	virtual Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) = 0;
    virtual bool TryGetInstrumentId(int32_t instrumentIndex, int32_t& instrumentId) = 0;
	virtual Tools::Bitset64 InstrumentIds() = 0;

	// Order rows are ONE server-wide array (all three keyed by serverName), so every context is a
	// view over the same storage. These used to be virtual, with ClientContext reading the argument
	// as a local index and ServerContext as a global one - same signature, two meanings, so a caller
	// holding a base Context could not know which to pass. Keyed by the id there is only one
	// meaning: OrderId already carries clientId and localIndex, so GlobalIndex is exact and no
	// caller needs to know whose order it is. Matches C#.
	Socket::SharedArrayEntry<Execution::OrderState>& GetOrderState(Execution::OrderId orderId)
	{
		return _orderStates[orderId.GlobalIndex()];
	}

	Socket::SharedArrayEntry<Execution::OrderTarget>& GetOrderTarget(Execution::OrderId orderId)
	{
		return _orderTargets[orderId.GlobalIndex()];
	}

	Socket::SharedArrayEntry<Execution::OrderRisk>& GetOrderRisk(Execution::OrderId orderId)
	{
		return _orderRisks[orderId.GlobalIndex()];
	}

	// server level - shared over all clients
	Socket::SharedArrayEntry<Execution::RiskLimit>& GetRiskLimit(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _riskLimits[instrumentId];
	}

	Socket::SharedArrayEntry<Data::MarketByPrice64>& GetMarketByPrice64(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _marketsByPrice[instrumentId];
	}

	// Common shared memory accessors
	Socket::SharedArrayEntry<Data::InstrumentHeader128>& GetInstrumentHeader(int32_t instrumentHeaderId)
	{
		ThrowIfInstrumentHeaderIdOutOfRange(instrumentHeaderId);
		return _instrumentHeaders[instrumentHeaderId];
	}

	Socket::SharedArrayEntry<int32_t>& GetInstrumentHeaderIdByInstrumentId(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _instrumentHeaderIdByInstrumentId[instrumentId];
	}

	Socket::SharedArrayEntry<Tools::Bitset64>& GetInstrumentIdsByClientId(int32_t clientId)
	{
		ThrowIfClientIdOutOfRange(clientId);
		return _instrumentIdsByClientId[clientId];
	}

	Socket::SharedArrayEntry<Tools::Bitset64>& GetClientIdsByInstrumentId(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _clientIdsByInstrumentId[instrumentId];
	}

	Socket::SharedArrayEntry<Provider::ServerHeader>& ServerHeader()
	{
		return _serverHeaderBox.GetEntry();
	}

	Data::Instrument& GetInstrument(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		size_t index = static_cast<size_t>(instrumentId);
		std::unique_ptr<Data::Instrument>& instrumentPtr = _instruments[index];

		if (!instrumentPtr) [[unlikely]]
			CreateInstrument(instrumentId);

		return *instrumentPtr;
	}

	Position& GetPosition(int32_t instrumentId)
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);

		size_t index = static_cast<size_t>(instrumentId);
		if (!_positions[index]) [[unlikely]]
		{
			Data::Instrument& instrument = GetInstrument(instrumentId);
			CreatePosition(instrument);
		}

		return *_positions[index];
	}

protected:

	ALWAYS_INLINE void ThrowIfInstrumentIdOutOfRange(int32_t instrumentId)
	{
		if (!ServerHeader().GetReadonlyRef().InstrumentIds[instrumentId]) [[unlikely]]
		{
			throw std::out_of_range(std::string(typeid(*this).name()) + ".ThrowIfInstrumentIdOutOfRange(" + std::to_string(instrumentId) + "), instrumentId has not been allocated.");
		}
	}

	ALWAYS_INLINE void ThrowIfInstrumentHeaderIdOutOfRange(int32_t instrumentHeaderId)
	{
		int32_t maxCount = ServerHeader().GetReadonlyRef().InstrumentsCount;
		if (instrumentHeaderId < 0 || instrumentHeaderId >= maxCount) [[unlikely]]
		{
			throw std::out_of_range(std::string(typeid(*this).name()) + ".ThrowIfInstrumentHeaderIdOutOfRange(" + std::to_string(instrumentHeaderId) + "), instrumentHeaderId should be less than: " + std::to_string(maxCount));
		}
	}

	ALWAYS_INLINE void ThrowIfClientIdOutOfRange(int32_t clientId)
	{
		if (!ServerHeader().GetReadonlyRef().ClientIds[clientId]) [[unlikely]]
		{
			throw std::out_of_range(std::string(typeid(*this).name()) + ".ThrowIfClientIdOutOfRange(" + std::to_string(clientId) + "), clientId has not been allocated.");
		}
	}

    std::atomic<bool> _lock{false};
	void CreateInstrument(int32_t instrumentId)
	{
        Tools::RAIISpinLock lock(_lock);
        if (_instruments[static_cast<size_t>(instrumentId)])
            return;

		int32_t instrumentHeaderId = GetInstrumentHeaderIdByInstrumentId(instrumentId).Read();
		Socket::SharedArrayEntry<Data::InstrumentHeader128>& header128Entry = GetInstrumentHeader(instrumentHeaderId);
		const Data::InstrumentHeader& instrHeader = header128Entry.GetReadonlyRef().AsInstrumentHeader();

		Socket::SharedArrayEntry<Data::MarketByPrice64>& mbpEntry = _marketsByPrice[instrumentId];
		std::unique_ptr<Data::Instrument> instrument = nullptr;

		if (instrHeader.InstrumentType == Data::InstrumentType::Future)
		{
			instrument = std::make_unique<class Data::Future>(instrumentId, header128Entry.Cast<Data::FutureHeader>(), mbpEntry);
		}
		else if (instrHeader.InstrumentType == Data::InstrumentType::Spread)
		{
			Data::SpreadHeader spreadHeader = header128Entry.GetReadonlyRef().AsSpread();
			class Data::Future& longFuture = static_cast<class Data::Future&>(GetInstrument(spreadHeader.LongInstrumentId));
			class Data::Future& shortFuture = static_cast<class Data::Future&>(GetInstrument(spreadHeader.ShortInstrumentId));   
			instrument = std::make_unique<class Data::Spread>(instrumentId, header128Entry.Cast<Data::SpreadHeader>(), mbpEntry, longFuture, shortFuture);
		}
		else if (instrHeader.InstrumentType == Data::InstrumentType::Forex)
		{
			instrument = std::make_unique<class Data::Forex>(instrumentId, header128Entry.Cast<Data::ForexHeader>(), mbpEntry);
		}
		else
		{
			throw std::runtime_error(std::string(typeid(*this).name()) + ".CreateInstrument(" + std::to_string(instrumentId) + "), Unknown instrument type: " + std::to_string(static_cast<int32_t>(instrHeader.InstrumentType)));
		}

		_instruments[static_cast<size_t>(instrumentId)] = std::move(instrument);
	}

	// Defined after ClientContext (bottom of this file): the owner id needs the complete type for
	// the cast, mirroring C#'s `(this as ClientContext)?.ClientId ?? ServerStrategyId`.
	void CreatePosition(Data::Instrument& instrument);

    int32_t GetInstrumentId(int32_t instrumentHeaderId) 
    {
		ThrowIfInstrumentHeaderIdOutOfRange(instrumentHeaderId);
		const Data::InstrumentHeader128& header128 = _instrumentHeaders[instrumentHeaderId].GetReadonlyRef();
		return header128.AsInstrumentHeader().InstrumentId;
	}
};

class ServerContext : public Context
{
	Socket::SharedArray<Execution::PositionHeader> _serverPositionHeaders;

public:

	// Server-only: the client and instrument allocations the server replays at startup. A client
	// context has neither, so these live here rather than on Context.
	const std::filesystem::path ClientsDirectoryPath;
	const std::filesystem::path InstrumentsDirectoryPath;

	// Startup-only reverse lookup for Server::LoadInstruments: the persisted record carries the
	// exchange's id, which is stable across sessions, whereas InstrumentHeaderId is just this run's
	// load order. Returns -1 when the contract is no longer listed, so the caller can skip it.
	int32_t GetInstrumentHeaderIdByExchangeInstrumentId(int32_t exchangeInstrumentId)
	{
		int32_t instrumentsCount = ServerHeader().GetReadonlyRef().InstrumentsCount;
		for (int32_t instrumentHeaderId = 0; instrumentHeaderId < instrumentsCount; instrumentHeaderId++)
		{
			if (_instrumentHeaders[instrumentHeaderId].GetReadonlyRef().AsInstrumentHeader().ExchangeInstrumentId == exchangeInstrumentId)
				return instrumentHeaderId;
		}
		return -1;
	}

	std::filesystem::path GetClientsFilePath(Tools::Timestamp date)
	{
		return ClientsDirectoryPath / (date.ToDateString() + ".allocateclient");
	}

	std::filesystem::path GetInstrumentsFilePath(Tools::Timestamp date)
	{
		return InstrumentsDirectoryPath / (date.ToDateString() + ".allocateinstrument");
	}


	ServerContext(const std::filesystem::path& serverName, Tools::Access access)
    : Context(serverName.string(), serverName, access, Tools::Access::Read),
      _serverPositionHeaders(ServerName / "ServerPositionHeaders", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
      ClientsDirectoryPath(ServerName / "Clients"),
      InstrumentsDirectoryPath(ServerName / "Instruments"),
      ServerStrategyName(GetStrategyDirectoryPath(ServerName.filename().string()))
	{
		ThrowIfInvalidServerName(serverName);

		std::filesystem::create_directories(ClientsDirectoryPath);
		std::filesystem::create_directories(InstrumentsDirectoryPath);

		int32_t cliCapacity = ServerHeader().GetReadonlyRef().ClientIds.Length();
		if (cliCapacity > 64)
			throw std::runtime_error(std::string(typeid(*this).name()) + ".ServerContext(), ClientsCapacity (" + std::to_string(cliCapacity) + ") must be less than or equal to 64.");
	}

	static inline void ThrowIfInvalidServerName(const std::filesystem::path& serverName)
	{
		std::filesystem::path validDirectoryPath = GetDirectoryPath("");
		if (!serverName.string().starts_with(validDirectoryPath.string()))
			throw std::invalid_argument("ServerContext.ThrowIfInvalidServerName(" + serverName.string() + "), serverName is invalid, must start with: " + validDirectoryPath.string());
	}

	static inline std::filesystem::path DirectoriesPath()
	{
		return RootDirectoryPath / "Servers";
	}

	static inline std::filesystem::path GetDirectoryPath(const std::string& serverName)
	{
		std::string modeStr = Clock::Mode == ClockMode::Simulation ? "Simulation" : "Realtime";
		return DirectoriesPath() / modeStr / serverName;
	}

	// The house book's directory: the Strategies tree under the server's leaf name. Strategy 0 has no
	// socket to take a name from, so its position files hang off this instead. See Spec.md.
	const std::filesystem::path ServerStrategyName;

	static inline Socket::LetterBox<Provider::ServerHeader> Connect(const Provider::ServerHeader& serverHeader)
	{
        std::string serverNameStr = serverHeader.ServerName.ToString();
		// Path-joined, matching Context's _serverHeaderBox. Concatenating here names a different
		// region, and Context::EnsureConnected() then spins forever on a box nobody writes.
		Socket::LetterBox<Provider::ServerHeader> serverHeaderBox(std::filesystem::path(serverNameStr) / "ServerHeader", Tools::Access::Write);

		// Reserve the house book before anyone can connect. AllocateClientId hands out
		// ClientIds.LowestClear(), so without this the FIRST client to attach takes id 0 and every
		// manual order from a server workspace would be attributed to it. Pre-setting the bit both
		// keeps it out of the allocator and makes the slot addressable — ThrowIfClientIdOutOfRange
		// and the RiskLayer's StrategyIdNotAllocated check each test this bit.
		Provider::ServerHeader reserved = serverHeader;
		reserved.ClientIds.Set(Execution::OrderIdAllocator::ServerStrategyId);

		if (!serverHeaderBox.TryStore(reserved))
			throw std::runtime_error("ServerContext.Connect(" + serverNameStr + "), Failed to write ServerHeader to shared memory.");

		return serverHeaderBox;
	}

	// --- Global Implementations ---
	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) override
	{
		return _serverPositionHeaders[instrumentId];
	}

	Tools::Bitset64 InstrumentIds() override
	{
		Provider::ServerHeader serverHeader = ServerHeader().GetReadonlyRef();
		return serverHeader.InstrumentIds;
	}

	// --- Specific Server Expositions ---
	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t clientId, int32_t instrumentId)
	{
		int32_t localPositionIndex = GetLocalPositionIndex(clientId, instrumentId);
		return _localPositionHeaders[localPositionIndex];
	}

	const Socket::SharedArrayEntry<Socket::SocketHeader>& GetSocketHeader(int32_t clientId)
	{
		ThrowIfClientIdOutOfRange(clientId);
		return _clientSocketHeaders[clientId];
	}

    int32_t AllocateClientId(const Socket::SocketHeader& socketHeader)
    {       
        // The server's leaf name is reserved for strategy 0's directory — a client named after the
        // server would share the house book's files. See Spec.md.
        if (socketHeader.ClientName == Tools::String128(ServerStrategyName.string()))
            throw std::runtime_error("ServerContext.AllocateClientId(" + ServerName.string() + "), client name " + socketHeader.ClientName.ToString() + " collides with the reserved server strategy name.");

        Socket::SharedArrayEntry<Provider::ServerHeader>& serverHeaderEntry = ServerHeader();
        Provider::ServerHeader& serverHeader = serverHeaderEntry.GetRef();
        Tools::Bitset64& clientIds = serverHeader.ClientIds;

        for(int32_t i : clientIds)
        {
            if (_clientSocketHeaders[i].GetReadonlyRef().ClientName == socketHeader.ClientName)
                return i;
        }

        int32_t clientId = socketHeader.ClientId < 0 ? clientIds.LowestClear() : socketHeader.ClientId;

        if (clientId < 0)
        {
            throw std::runtime_error("ServerContext.AllocateClientId, Failed allocate clientId.");
        }

        serverHeaderEntry.AcquireLock();
        Socket::SocketHeader socketHeaderCopy = socketHeader;
        socketHeaderCopy.ClientId = clientId;
        _clientSocketHeaders[clientId].Write(socketHeaderCopy);
        serverHeader.ClientIds.Set(clientId);
        _instrumentIdsByClientId[clientId].Write(Tools::Bitset64(0ULL));
        serverHeaderEntry.ReleaseLock();

        return clientId;
    }

    Socket::SocketHeader DeallocateClient(int32_t clientId)
    {
        const Tools::Bitset64 instrumentIds = GetInstrumentIdsByClientId(clientId).GetReadonlyRef();
        for(int32_t instrumentId : instrumentIds)
        {
            Tools::Bitset64& clientIds = GetClientIdsByInstrumentId(instrumentId).GetRef();
            clientIds.Clear(clientId);
        }
        GetInstrumentIdsByClientId(clientId).Write(Tools::Bitset64(0ULL));
        return _clientSocketHeaders[clientId].GetReadonlyRef();
    }

    bool TryGetInstrumentId(int32_t instrumentHeaderId, int32_t& instrumentId) override
	{
		instrumentId = GetInstrumentId(instrumentHeaderId);
		if (instrumentId < 0)
			return false;

		return true;
	}

    void OnInstrumentHeader(Data::InstrumentHeader128& header128)
    {
        Socket::SharedArrayEntry<Provider::ServerHeader> serverHeaderEntry = ServerHeader();
        Provider::ServerHeader& serverHeader = serverHeaderEntry.GetRef();
        if (serverHeader.InstrumentsCount >= serverHeader.InstrumentsCapacity)
            throw std::out_of_range("Server::OnInstrumentHeader: InstrumentsCount(" + std::to_string(serverHeader.InstrumentsCount) + ") >= InstrumentsCapacity(" + std::to_string(serverHeader.InstrumentsCapacity) + ")");

        Data::InstrumentHeader& header = header128.AsInstrumentHeader();
        header.InstrumentId = -1;
        header.InstrumentHeaderId = serverHeader.InstrumentsCount;
        Socket::SharedArrayEntry<Data::InstrumentHeader128> header128Entry = _instrumentHeaders[header.InstrumentHeaderId];
        header128Entry.Write(header128);

        serverHeaderEntry.AcquireLock();
        serverHeader.InstrumentsCount++;
        serverHeaderEntry.ReleaseLock();
    }

	int32_t AllocateInstrument(int32_t instrumentHeaderId)
	{
		Socket::SharedArrayEntry<Provider::ServerHeader> serverHeaderEntry = ServerHeader();
		Provider::ServerHeader& serverHeader = serverHeaderEntry.GetRef();
		Socket::SharedArrayEntry<Data::InstrumentHeader128>& header128Entry = GetInstrumentHeader(instrumentHeaderId);
		Data::InstrumentHeader128& header128 = header128Entry.GetRef();
		Data::InstrumentHeader& header = header128.AsInstrumentHeader();

		int32_t instrumentId = header.InstrumentId;
		if (instrumentId >= 0 && GetInstrumentHeaderIdByInstrumentId(instrumentId).GetReadonlyRef() == instrumentHeaderId)
		{
			return instrumentId;
		}

		if (serverHeader.InstrumentIds.IsFull())
			throw std::runtime_error(std::string(typeid(*this).name()) + ".AllocateInstrument(" + std::to_string(instrumentHeaderId) + "), InstrumentIds is Full.");

		instrumentId = serverHeader.InstrumentIds.LowestClear();

		serverHeaderEntry.AcquireLock();

		_marketsByPrice[instrumentId].Write(Data::MarketByPrice64{});
		_clientIdsByInstrumentId[instrumentId].Write(Tools::Bitset64{});
		_instrumentHeaderIdByInstrumentId[instrumentId].Write(instrumentHeaderId);

		std::unique_ptr<Data::Symbology> symbology = header128.Symbology();

		std::string symbol = symbology->Symbol();
		std::string riskLimitPath = GetRiskLimitsFilePath(DirectoryPath, symbol);
		std::optional<std::string> riskLimitLine = Tools::ReadLastLine(riskLimitPath);
        if (riskLimitLine.has_value())
        {
            std::cout << "Context::AllocateInstrument(" <<  symbol << ") Loaded RiskLimit: " << riskLimitLine.value() << std::endl;
        }
		Execution::RiskLimit riskLimit = riskLimitLine ? Tools::Json::Deserialize<Execution::RiskLimit>(riskLimitLine.value()) : (Clock::Mode == ClockMode::Simulation ? Execution::RiskLimit::GetMaxLimits(instrumentId) : Execution::RiskLimit::GetMinLimits(instrumentId));
		riskLimit.InstrumentId = instrumentId;
		// The working quantities are live state, not configuration: they mirror this SESSION's
		// in-flight reservations, and a fresh session has none. Restoring yesterday's values would
		// hand the ledger phantom exposure no retire path can ever release. Matches C#.
		riskLimit.WorstLongWorkingQuantity = 0;
		riskLimit.WorstShortWorkingQuantity = 0;
        _riskLimits.GetEntry(instrumentId).Write(riskLimit);

		std::string positionPath = GetPositionFilePath(DirectoryPath, symbology->Symbol()).string();
		std::optional<std::string> positionLine = Tools::ReadLastLine(positionPath);
		Execution::PositionHeader positionHeader = positionLine ? Tools::Json::Deserialize<Execution::PositionHeader>(positionLine.value()) : Execution::PositionHeader{};
		// Server-wide row: no owning order; a template id (Generation 0) carries the ids.
		// Client 0 stands in for the old -1 sentinel (-1 is unrepresentable in 6 bits) - display/JSON-only change.
		positionHeader.OrderHeader.OrderId = Execution::OrderId().InstrumentId(instrumentId);
		_serverPositionHeaders[instrumentId].Write(positionHeader);

		header.InstrumentId = instrumentId;
		serverHeader.InstrumentIds.Set(instrumentId);

		serverHeaderEntry.ReleaseLock();

		return instrumentId;
	}

	void AllocateInstrument(int32_t clientId, int32_t instrumentId)
	{
		int32_t instrumentHeaderId = GetInstrumentHeaderIdByInstrumentId(instrumentId).GetReadonlyRef();

		if (InstrumentIds()[instrumentId] == false)
		{
			return;
		}

		Data::InstrumentHeader128& header128 = GetInstrumentHeader(instrumentHeaderId).GetRef();
		std::unique_ptr<Data::Symbology> symbology = header128.Symbology();

		// Strategy 0 has no socket to take a name from; its book lives at ServerStrategyName (see Spec.md).
		std::string clientName = clientId == Execution::OrderIdAllocator::ServerStrategyId
			? ServerStrategyName.string()
			: GetSocketHeader(clientId).GetReadonlyRef().ClientName.ToString();
		std::string positionPath = GetPositionFilePath(clientName, symbology->Symbol()).string();
		std::optional<std::string> positionLine = Tools::ReadLastLine(positionPath);
		Execution::PositionHeader positionHeader = positionLine ? Tools::Json::Deserialize<Execution::PositionHeader>(positionLine.value()) : Execution::PositionHeader{};
		// Stamp the full routing identity at allocation so any reader (e.g. the 1-arg
		// Server::WriteToExecution used by ControlAlgoStatus) routes to the right client/CoreGroup
		// BEFORE the first fill overwrites OrderHeader. ClientId == StrategyId for a client's own
		// (algo) positions. A template id (Generation 0) carries the identity until the first fill.
		positionHeader.OrderHeader.OrderId = Execution::OrderId().ClientId(clientId).StrategyId(clientId).InstrumentId(instrumentId);
		GetPositionHeader(clientId, instrumentId).Write(positionHeader);

		Socket::SharedArrayEntry<Tools::Bitset64>& instrumentIdsEntry = GetInstrumentIdsByClientId(clientId);
		Socket::SharedArrayEntry<Tools::Bitset64>& clientIdsEntry = GetClientIdsByInstrumentId(instrumentId);

		clientIdsEntry.AcquireLock();
        instrumentIdsEntry.AcquireLock();
        
        clientIdsEntry.GetRef().Set(clientId);
        instrumentIdsEntry.GetRef().Set(instrumentId);

        instrumentIdsEntry.ReleaseLock();
        clientIdsEntry.ReleaseLock();
	}
};

class ClientContext : public Context
{
public:
	std::string ClientName;
	const int32_t ClientId;

	ClientContext(const std::filesystem::path& clientName, const std::filesystem::path& serverName, Tools::Access access) 
        : Context(serverName.string(), clientName, Tools::Access::Read, access), ClientId(GetClientIdFromMap(clientName.string()))
	{
		if (clientName.string().find(serverName.string()) != std::string::npos)
			ServerContext::ThrowIfInvalidServerName(clientName);
		else
			ThrowIfInvalidClientName(clientName);

		if (clientName.empty())
			throw std::invalid_argument(std::string(typeid(*this).name()) + ".ClientContext(), Client name must be non-empty.");

		ClientName = clientName.string();
	}

	static inline void ThrowIfInvalidClientName(const std::filesystem::path& clientName)
	{
		std::filesystem::path validDirectoryPath = GetDirectoryPath("");
		if (!clientName.string().starts_with(validDirectoryPath.string()))
			throw std::invalid_argument("ClientContext.ThrowIfInvalidClientName(" + clientName.string() + "), clientName is invalid, must start with: " + validDirectoryPath.string());
	}

	static inline std::filesystem::path DirectoriesPath()
	{
		return RootDirectoryPath / "Strategies";
	}

	static inline std::filesystem::path GetDirectoryPath(const std::string& clientName)
	{
		return GetStrategyDirectoryPath(clientName);
	}

	int32_t GetClientIdFromMap(const std::string& clientName)
	{
		Tools::String128 nameCopy(clientName);
		for (int32_t i : ServerHeader().GetReadonlyRef().ClientIds)
		{
			if (_clientSocketHeaders[i].IsEmpty())
				continue;

			Socket::SocketHeader header = _clientSocketHeaders[i].Read();

			if (header.ClientName == nameCopy)
				return i;
		}
		throw std::runtime_error(std::string(typeid(*this).name()) + ".GetClientIdFromMap(" + clientName + "), Client not connected to server '" + ServerName.string() + "'.");
	}

	// --- Local Implementations ---
	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) override
	{
		int32_t localPositionIndex = GetLocalPositionIndex(ClientId, instrumentId);
		return _localPositionHeaders[localPositionIndex];
	}

	Tools::Bitset64 InstrumentIds() override
	{
		return _instrumentIdsByClientId[ClientId].GetReadonlyRef();
	}

    bool TryGetInstrumentId(int32_t instrumentHeaderId, int32_t& instrumentId) override
	{
		instrumentId = GetInstrumentId(instrumentHeaderId);
		if (instrumentId < 0)
			return false;

		Tools::Bitset64 instrumentIds = InstrumentIds();
        if (!instrumentIds[instrumentId])
        {
            instrumentId = -1;
            return false;
        }

		return true;
	}
};

class ContextManager
{
private:
    static inline std::mutex s_lock;
    static inline std::unordered_map<std::string, std::unique_ptr<ClientContext>> s_clientContexts;

public:
    static inline std::filesystem::path ServerName;
    static inline std::unique_ptr<ServerContext> ServerContextInstance;
    static inline bool IsInitialized = false;

    static void Initialize(const std::filesystem::path& serverName)
    {
        std::cout << "ContextManager::Initialize(" << serverName.string() << ")" << std::endl;

        std::lock_guard<std::mutex> lock(s_lock);
        if (!IsInitialized)
        {
            ServerName = serverName;
            ServerContextInstance = std::make_unique<ServerContext>(serverName, Tools::Access::Read);
            IsInitialized = true;
        }
    }

    static ClientContext& GetClientContext(const std::filesystem::path& clientName)
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto it = s_clientContexts.find(clientName.string());
        if (it == s_clientContexts.end())
        {
            auto context = std::make_unique<ClientContext>(clientName, ServerName, Tools::Access::Read);
            ClientContext& ref = *context;
            s_clientContexts[clientName.string()] = std::move(context);
            return ref;
        }
        return *it->second;
    }

    static void Dispose()
    {
        std::lock_guard<std::mutex> lock(s_lock);
        if (IsInitialized)
        {
            s_clientContexts.clear();
            ServerContextInstance.reset();
            IsInitialized = false;
        }
    }
};

// --- Inline definitions that need complete Context class definition ---

// A ServerContext's positions are the server-wide rows; no client owns their order slots (only
// Client.Create ever sets one), so the house id is the correct inert owner. Matches C#.
inline void Context::CreatePosition(Data::Instrument& instrument)
{
	Tools::RAIISpinLock lock(_lock);
	if (_positions[static_cast<size_t>(instrument.InstrumentId)])
		return;
	ClientContext* clientContext = dynamic_cast<ClientContext*>(this);
	int32_t clientId = clientContext ? clientContext->ClientId : Execution::OrderIdAllocator::ServerStrategyId;
	_positions[static_cast<size_t>(instrument.InstrumentId)] = std::make_unique<Position>(instrument, GetPositionHeader(instrument.InstrumentId), *this, clientId);
}

inline bool Position::TryGetQuote(Data::Quote& quote)
{
	Data::MarketByPrice64 mbp = Instrument.MarketByPriceCopy();
	Tools::Bitset64 isOrderActive = _isOrderActive;

	while (!isOrderActive.IsEmpty())
	{
		int32_t localOrderIndex = 0;
		isOrderActive.TryPopLowest(localOrderIndex);

		Socket::SharedArrayEntry<Execution::OrderState>& stateEntry = _context.GetOrderState(GetOrderId(localOrderIndex));

		int32_t ticks = 0;
		int32_t working = 0;
		Data::Side side = Data::Side::Buy;

		uint64_t seq0 = 0;
		uint64_t seq1 = 0;

		while(true)
		{
			seq0 = stateEntry.GetSeq();
			if (Socket::Protocol::IsWriteInProgress(seq0))
			{
				_mm_pause();
				continue;
			}

			const Execution::OrderState& state = stateEntry.GetReadonlyRef();

			if (state.OrderStateStatus == Execution::OrderStateStatus::Done)
            {
                seq1 = stateEntry.GetSeq();
                if (seq0 != seq1) continue;
				goto NextOrder;
            }

			ticks = state.OrderProfile.Ticks;
			side = state.OrderProfile.Side();
			working = std::abs(state.OrderProfile.Quantity - state.QuantityFilled);

			seq1 = stateEntry.GetSeq();
			if (seq0 == seq1) break;
		}

		{
			int32_t dummyDelta;
			if (side == Data::Side::Buy)
			{
				int32_t total = mbp.Bids.GetQuantity(ticks);
				int32_t quantity = std::max(total - working, 0);
				mbp.Bids.TrySetQuantity(ticks, quantity, dummyDelta);
			}
			else
			{
				int32_t total = mbp.Asks.GetQuantity(ticks);
				int32_t quantity = std::max(total - working, 0);
				mbp.Asks.TrySetQuantity(ticks, quantity, dummyDelta);
			}
		}
	NextOrder:;
	}

	if (!Instrument.IsInSession() || mbp.BidsCount() == 0 || mbp.AsksCount() == 0)
	{
		quote = Data::Quote
		{
			.TickSize = Instrument.TickSize(),
			.Bid = Data::Level { .Ticks = 0, .Quantity = 0 },
			.Ask = Data::Level { .Ticks = 0, .Quantity = 0 }
		};
		return false;
	}

	quote = Data::Quote
	{
		.TickSize = Instrument.TickSize(),
		.Bid = mbp.BestBid(),
		.Ask = mbp.BestAsk()
	};
	return true;
}

}
//END_FILE HFT/Provider/Context.hpp