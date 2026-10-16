//BEGIN_FILE HFT/Provider/Context.hpp
#pragma once

#include "OrderIdAllocator.hpp"
#include "Order.hpp"
#include "RateLimit.hpp"

#include "SeqLock.hpp"
#include "Socket.hpp"
#include "SharedArray.hpp"
#include "Instrument.hpp"
#include "Bitset.hpp"
#include "Position.hpp"
#include <filesystem>
#include "Allocate.hpp"
#include "Timestamp.hpp"
#include "Clock.hpp"
#include "Session.hpp"
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

class ContextManager; // Forward declaration

class Context
{
public:
	// The one mount every durable path on this machine hangs off: servers, strategies, venue logs and
	// session state alike. Named here so no caller ever spells it out again.
	static inline const std::filesystem::path RootDirectoryPath = "/mnt/S";

	// Every context, server or client, checks this before any shared memory is opened: a server lives under
	// Servers/<ClockMode>/, so a simulation never attaches to a live server or the reverse.
	static inline void ThrowIfInvalidServerName(const std::filesystem::path& serverName)
	{
		std::string modeStr = Tools::Clock::Mode() == Tools::ClockMode::Simulation ? "Simulation" : "Realtime";
		std::filesystem::path validDirectoryPath = RootDirectoryPath / "Servers" / modeStr / "";
		if (!serverName.string().starts_with(validDirectoryPath.string()))
			throw std::invalid_argument("ServerContext.ThrowIfInvalidServerName(" + serverName.string() + "), serverName is invalid, must start with: " + validDirectoryPath.string());
	}

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

	static std::filesystem::path GetMessageEfficiencyFilePath(const std::filesystem::path& directoryPath, const std::string& productGroupRoot)
	{
		return directoryPath / "MessageEfficiency" / (productGroupRoot + ".messageefficiency");
	}

	static std::filesystem::path GetRateLimitFilePath(const std::filesystem::path& directoryPath, const std::string& coreGroupName)
	{
		return directoryPath / "RateLimits" / (coreGroupName + ".ratelimit");
	}

	static std::filesystem::path GetCoreGroupFilePath(const std::filesystem::path& directoryPath, const std::string& coreGroupName)
	{
		return directoryPath / "CoreGroups" / (coreGroupName + ".coregroup");
	}

	static std::filesystem::path GetLoggingServerDirectoryPath(const std::filesystem::path& directoryPath)
	{
		return directoryPath / "LoggingServer";
	}

	// The Strategies tree for the current clock mode. ClientContext::GetDirectoryPath is the public
	// spelling; it lives here because ServerContext is declared first and needs it for strategy 0.
	static std::filesystem::path GetStrategyDirectoryPath(const std::string& clientName)
	{
		std::string modeStr = Tools::Clock::Mode() == Tools::ClockMode::Simulation ? "Simulation" : "Realtime";
		return RootDirectoryPath / "Strategies" / modeStr / clientName;
	}

	const std::filesystem::path FillsDirectoryPath;
	const std::filesystem::path PositionsDirectoryPath;
	const std::filesystem::path AlertsDirectoryPath;
	const std::filesystem::path RiskLimitsDirectoryPath;
	const std::filesystem::path MessageEfficiencyDirectoryPath;
	const std::filesystem::path RateLimitsDirectoryPath;
	const std::filesystem::path CoreGroupsDirectoryPath;
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
	Socket::SharedArray<Execution::RiskLimit> _serverRiskLimits;   // per instrument: the firm's limit, every order the server reads
	// One row per product group (index == InstrumentHeader::ProductGroupId): the server claims, loads and ends each
	// group's day; strategies count into it.
	Socket::SharedArray<Execution::MessageEfficiency> _messageEfficiency;
	// One row per CoreGroup, server-written. RollingRateLimit today; another model would be a
	// different 64-byte view of the same row, cast by the caller (see Spec.md "Order rate limit").
	Socket::SharedArray<Execution::RollingRateLimit> _rateLimits;
	// One row per CoreGroup, server-written from its .coregroup file: the name and the core ids.
	Socket::SharedArray<Provider::CoreGroup> _coreGroups;
	Socket::SharedArray<Execution::OrderState> _orderStates;
	Socket::SharedArray<Execution::OrderTarget> _orderTargets;
	
	// positions
	Socket::SharedArray<Execution::PositionHeader> _localPositionHeaders;

	// Per instrument: every order on it, written by the server's RiskLayer (the firm's exposure).
	Socket::SharedArray<Execution::WorkingRisk> _serverWorkingRisks;

	// Per instrument: every strategy's fills summed; its AlgoStatus is the master switch every algo on the instrument checks.
	Socket::SharedArray<Execution::PositionHeader> _serverPositionHeaders;
	// Per strategy and instrument: the strategy's limit, its own RiskLayer's. The server loads and edits every row.
	Socket::SharedArray<Execution::RiskLimit> _localRiskLimits;
	// Per strategy and instrument: its own orders, written by that strategy's RiskLayer. Under the server's name so the Mirror
	// copies them to a remote desk.
	Socket::SharedArray<Execution::WorkingRisk> _localWorkingRisks;

	// The allocation ThrowIfInstrumentIdOutOfRange tests: ServerHeader.InstrumentIds for the server,
	// InstrumentIdsByClientId[ClientId] for a client (C# overrides it per context). Set by each derived
	// constructor; a pointer rather than a virtual keeps the hot-path accessors free of an indirect call.
	const Tools::Bitset64* _instrumentIds = nullptr;

	bool _isLegHeaderOwner = false;   // this context installed Data::LeggedHeader::GetLegHeader

	std::vector<std::unique_ptr<Data::Instrument>> _instruments;
	std::vector<std::unique_ptr<Position>> _positions;

protected:
	Context(const std::filesystem::path& serverName, const std::filesystem::path& directoryPath, Tools::Access serverAccess, Tools::Access clientAccess)
    :
    // First initialiser, ahead of every shared array: a wrong-mode server name throws before anything is opened.
    ServerName((ThrowIfInvalidServerName(serverName), serverName)),
    LoggingServerName(GetLoggingServerDirectoryPath(ServerName)),
    DirectoryPath(directoryPath),
    ServerAccess(serverAccess),
    ClientAccess(clientAccess),
    FillsDirectoryPath(directoryPath / "Fills"),
    PositionsDirectoryPath(directoryPath / "Positions"),
    AlertsDirectoryPath(directoryPath / "Alerts"),
    RiskLimitsDirectoryPath(directoryPath / "RiskLimits"),
    MessageEfficiencyDirectoryPath(directoryPath / "MessageEfficiency"),
    RateLimitsDirectoryPath(directoryPath / "RateLimits"),
    CoreGroupsDirectoryPath(directoryPath / "CoreGroups"),
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
    _serverRiskLimits(serverName / "ServerRiskLimits", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    // Why writable on both sides: the server claims, loads and ends each group's day; strategies count into it
    _messageEfficiency(serverName / "MessageEfficiency", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess == Tools::Access::Write ? Tools::Access::Write : ClientAccess),
    _rateLimits(serverName / "RateLimits", ServerHeader().GetReadonlyRef().CoreGroupIds.Length(), ServerAccess),
    _coreGroups(serverName / "CoreGroups", ServerHeader().GetReadonlyRef().CoreGroupIds.Length(), ServerAccess),
    _orderStates(serverName / "OrderStates", ServerHeader().GetReadonlyRef().OrdersCapacity(), ServerAccess, false),
    _orderTargets(serverName / "OrderTargets", ServerHeader().GetReadonlyRef().OrdersCapacity(), ClientAccess, false),
    _localPositionHeaders(serverName / "LocalPositionHeaders", ServerHeader().GetReadonlyRef().LocalPositionsCapacity(), ServerAccess, false),
    _serverWorkingRisks(serverName / "ServerWorkingRisks", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    // Why every context: its AlgoStatus is the master switch each algo checks
    _serverPositionHeaders(serverName / "ServerPositionHeaders", ServerHeader().GetReadonlyRef().InstrumentIds.Length(), ServerAccess),
    _localRiskLimits(serverName / "LocalRiskLimits", ServerHeader().GetReadonlyRef().LocalPositionsCapacity(), ServerAccess, false),
    // Why writable on both sides: the server seeds each row at allocation, the strategy moves it
    _localWorkingRisks(serverName / "LocalWorkingRisks", ServerHeader().GetReadonlyRef().LocalPositionsCapacity(), ServerAccess == Tools::Access::Write ? Tools::Access::Write : ClientAccess, false)
	{
        std::cout << Tools::GetTypeName(typeid(*this)) << "(" << serverName << ", " << directoryPath.string() << ", "
                  << static_cast<int>(serverAccess) << ", " << static_cast<int>(clientAccess) << ")" << std::endl;

        std::filesystem::create_directories(FillsDirectoryPath);
        std::filesystem::create_directories(PositionsDirectoryPath);
        std::filesystem::create_directories(AlertsDirectoryPath);
        std::filesystem::create_directories(RiskLimitsDirectoryPath);
        std::filesystem::create_directories(MessageEfficiencyDirectoryPath);
        std::filesystem::create_directories(RateLimitsDirectoryPath);
        std::filesystem::create_directories(CoreGroupsDirectoryPath);
        std::filesystem::create_directories(AuditDirectoryPath);
        std::filesystem::create_directories(SeriesDirectoryPath);
        std::filesystem::create_directories(WorkspaceDirectoryPath);

		_instruments.resize(static_cast<size_t>(ServerHeader().GetReadonlyRef().InstrumentIds.Length()));
		_positions.resize(static_cast<size_t>(ServerHeader().GetReadonlyRef().InstrumentIds.Length()));

		// A legged header's symbology resolves its legs through sibling headers; hook it up to this
		// context's header array (matches C# LeggedHeader.GetLegHeader). SET ONCE: std::function
		// assignment is not atomic, so a context built at runtime (a GUI attaching mid-session)
		// must not rebind the hook while another thread is calling it. Every context reads the
		// same server-owned array, so first-writer-wins is as good - and it pins the capture to
		// the longest-lived context (the first one built).
		if (!Data::LeggedHeader::GetLegHeader)
		{
			Data::LeggedHeader::GetLegHeader = [this](int32_t instrumentHeaderId)
			{
				return _instrumentHeaders[instrumentHeaderId].GetReadonlyRef();
			};
			_isLegHeaderOwner = true;
		}
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
	// The context that installed the leg lookup clears it, so it never calls into a destroyed context (C# ServerContext.Dispose).
	virtual ~Context()
	{
		if (_isLegHeaderOwner)
			Data::LeggedHeader::GetLegHeader = nullptr;
	}

	virtual Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) = 0;

	// Any strategy's row on the instrument; read-only on a client.
	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t clientId, int32_t instrumentId)
	{
		int32_t localPositionIndex = GetLocalPositionIndex(clientId, instrumentId);
		return _localPositionHeaders[localPositionIndex];
	}

	// An algo may trade the instrument only while both switches are Live: its strategy's row and the server's, the master switch
	// a server workspace turns (see Spec.md "Algo pause"). Hot path: two plain byte reads.
	ALWAYS_INLINE bool IsAlgoLive(int32_t strategyId, int32_t instrumentId)
	{
		return GetPositionHeader(strategyId, instrumentId).GetReadonlyRef().AlgoStatus == Execution::AlgoStatus::Live
			&& _serverPositionHeaders[instrumentId].GetReadonlyRef().AlgoStatus == Execution::AlgoStatus::Live;
	}

    virtual bool TryGetInstrumentId(int32_t instrumentIndex, int32_t& instrumentId) = 0;
	virtual Tools::Bitset64 InstrumentIds() = 0;

	// OrderStates and OrderTargets are ONE server-wide array (keyed by serverName), so every context is
	// a view over the same storage (OrderRisk rows live in each RiskLayer, not here). These used to be virtual, with ClientContext reading the argument
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

	// This context's own limit, the one its RiskLayer checks: the firm's on the server, the strategy's on a client (see Spec.md "Strategy risk limits").
	virtual Socket::SharedArrayEntry<Execution::RiskLimit>& GetRiskLimit(int32_t instrumentId) = 0;

	// Any strategy's limit on the instrument: the server loads and edits them; read-only on a client.
	Socket::SharedArrayEntry<Execution::RiskLimit>& GetRiskLimit(int32_t clientId, int32_t instrumentId)
	{
		return _localRiskLimits[GetLocalPositionIndex(clientId, instrumentId)];
	}

	Socket::SharedArrayEntry<Execution::MessageEfficiency>& GetMessageEfficiency(int32_t productGroupId)
	{
		return _messageEfficiency[productGroupId];
	}

	// This context's own exposure, the one its RiskLayer moves and checks: the firm's on the server, the strategy's on a client.
	virtual Socket::SharedArrayEntry<Execution::WorkingRisk>& GetWorkingRisk(int32_t instrumentId) = 0;

	// One per CoreGroup, server-written (index == CoreGroupId).
	// Finds a client by its exact name; false when no client has it.
	bool TryGetClientId(const Tools::String128& clientName, int32_t& clientId)
	{
		for (int32_t candidateClientId : ServerHeader().GetReadonlyRef().ClientIds)
		{
			if (!_clientSocketHeaders[candidateClientId].IsEmpty() && _clientSocketHeaders[candidateClientId].Read().ClientName == clientName)
			{
				clientId = candidateClientId;
				return true;
			}
		}
		clientId = -1;
		return false;
	}

	Socket::SharedArrayEntry<Execution::RollingRateLimit>& GetRateLimit(int32_t coreGroupId)
	{
		return _rateLimits[coreGroupId];
	}

	// One per CoreGroup, server-written when it loads the CoreGroup's file: the name and the cores its threads pin to.
	Socket::SharedArrayEntry<Provider::CoreGroup>& GetCoreGroup(int32_t coreGroupId)
	{
		return _coreGroups[coreGroupId];
	}

	// Throws when no loaded CoreGroup has that name: a client asking for a group the server does not have is a setup error.
	int32_t GetCoreGroupId(const Tools::String16& coreGroupName)
	{
		for (int32_t coreGroupId : ServerHeader().GetReadonlyRef().CoreGroupIds)
		{
			Socket::SharedArrayEntry<Provider::CoreGroup>& coreGroupEntry = _coreGroups[coreGroupId];
			if (!coreGroupEntry.IsEmpty() && coreGroupEntry.GetReadonlyRef().CoreGroupName == coreGroupName)
				return coreGroupId;
		}
		throw std::runtime_error(std::string(Tools::GetTypeName(typeid(*this))) + ".GetCoreGroupId(" + coreGroupName.ToString() + "), no CoreGroup with that name in " + CoreGroupsDirectoryPath.string());
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

	ALWAYS_INLINE void ThrowIfInstrumentIdOutOfRange(int32_t instrumentId)
	{
		if (!(*_instrumentIds)[instrumentId]) [[unlikely]]
		{
			throw std::out_of_range(std::string(typeid(*this).name()) + ".ThrowIfInstrumentIdOutOfRange(" + std::to_string(instrumentId) + "), instrumentId has not been allocated.");
		}
	}

protected:

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
		// Materialize a spread's legs BEFORE taking the non-reentrant lock: the Spread branch
		// below resolves them via GetInstrument, which must hit the cache, not re-enter
		// CreateInstrument (self-deadlock; shipped and caught on the C# side). Value copy via
		// GetReadonlyRef - clients map headers read-only, GetRef would throw.
		int32_t instrumentHeaderId = GetInstrumentHeaderIdByInstrumentId(instrumentId).Read();
		if (GetInstrumentHeader(instrumentHeaderId).GetReadonlyRef().AsInstrumentHeader().InstrumentType == Data::InstrumentType::Spread)
		{
			Data::LeggedHeader leggedHeader = GetInstrumentHeader(instrumentHeaderId).GetReadonlyRef().AsLegged();
			for (const Data::LegHeader& legHeader : leggedHeader.Legs())
				GetInstrument(GetInstrumentId(legHeader.InstrumentHeaderId));
		}

        Tools::RAIISpinLock lock(_lock);
        if (_instruments[static_cast<size_t>(instrumentId)])
            return;

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
			// Legs reference sibling headers; long = the positive-weight leg (2-leg spreads only: Client::GetInstrument refuses any other shape).
			Data::LeggedHeader leggedHeader = header128Entry.GetReadonlyRef().AsLegged();
			class Data::Future* longLeg = nullptr;
			class Data::Future* shortLeg = nullptr;
			for (const Data::LegHeader& legHeader : leggedHeader.Legs())
			{
				class Data::Future& legFuture = static_cast<class Data::Future&>(GetInstrument(GetInstrumentId(legHeader.InstrumentHeaderId)));
				if (legHeader.Weight > 0)
					longLeg = &legFuture;
				else if (legHeader.Weight < 0)
					shortLeg = &legFuture;
			}
			// C# throws here too: Spread's constructor dereferences both legs.
			if (!longLeg || !shortLeg)
				throw std::runtime_error(std::string(typeid(*this).name()) + ".CreateInstrument(" + std::to_string(instrumentId) + "), spread header must carry one positive and one negative leg.");
			instrument = std::make_unique<class Data::Spread>(instrumentId, header128Entry.Cast<Data::LeggedHeader>(), mbpEntry, *longLeg, *shortLeg);
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
      ClientsDirectoryPath(ServerName / "Clients"),
      InstrumentsDirectoryPath(ServerName / "Instruments"),
      ServerStrategyName(GetStrategyDirectoryPath(ServerName.filename().string()))
	{
		_instrumentIds = &ServerHeader().GetReadonlyRef().InstrumentIds;

		std::filesystem::create_directories(ClientsDirectoryPath);
		std::filesystem::create_directories(InstrumentsDirectoryPath);

		int32_t cliCapacity = ServerHeader().GetReadonlyRef().ClientIds.Length();
		if (cliCapacity > 64)
			throw std::runtime_error(std::string(typeid(*this).name()) + ".ServerContext(), ClientsCapacity (" + std::to_string(cliCapacity) + ") must be less than or equal to 64.");

		// Write mode: the server names its CoreGroups in files, one static JSON CoreGroup each, and each group's rate
		// limit follows by name from its .ratelimit file, static JSON too; Max in simulation and Min in realtime without one, like .risklimit.
		// The channel and thread for an id were built from ServerHeader.CoreGroupIds, so a file outside that set is fatal.
		if (access == Tools::Access::Write)
		{
			const Provider::ServerHeader& serverHeader = ServerHeader().GetReadonlyRef();
			for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(CoreGroupsDirectoryPath))
			{
				if (!entry.is_regular_file() || entry.path().extension() != ".coregroup")
					continue;

				Provider::CoreGroup coreGroup = Tools::Json::Deserialize<Provider::CoreGroup>(Tools::ReadAllText(entry.path()));
				int32_t coreGroupId = coreGroup.CoreGroupId;
				// Out-of-range folds into the same throw: C#'s indexer is bounds-checked, Bitset64's shift is not.
				if (coreGroupId < 0 || coreGroupId >= serverHeader.CoreGroupIds.Length() || !serverHeader.CoreGroupIds[coreGroupId])
					throw std::runtime_error(std::string(typeid(*this).name()) + "(" + ServerName.string() + "), " + entry.path().string() + ": CoreGroupId " + std::to_string(coreGroupId) + " is not set in ServerHeader.CoreGroupIds");
				_coreGroups[coreGroupId].Write(coreGroup);

				std::filesystem::path rateLimitPath = GetRateLimitFilePath(DirectoryPath, coreGroup.CoreGroupName.ToString());
				Execution::RateLimit rateLimit = std::filesystem::exists(rateLimitPath)
					? Tools::Json::Deserialize<Execution::RateLimit>(Tools::ReadAllText(rateLimitPath))
					: (Tools::Clock::Mode() == Tools::ClockMode::Simulation ? Execution::RateLimit::GetMaxLimits(coreGroupId) : Execution::RateLimit::GetMinLimits(coreGroupId));
				rateLimit.RateLimitId = coreGroupId;
				_rateLimits[coreGroupId].Write(Execution::RollingRateLimit(rateLimit));
			}
		}
	}

	static inline std::filesystem::path DirectoriesPath()
	{
		return RootDirectoryPath / "Servers";
	}

	static inline std::filesystem::path GetDirectoryPath(const std::string& serverName)
	{
		std::string modeStr = Tools::Clock::Mode() == Tools::ClockMode::Simulation ? "Simulation" : "Realtime";
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
		reserved.ServerProcessId = static_cast<int32_t>(getpid());

		// Never overwrite a dead run's header: the server would start on that run's rows and rings (see Spec.md)
		if (!serverHeaderBox.TryStore(reserved))
		{
			int32_t previousServerProcessId = serverHeaderBox.GetEntry().GetReadonlyRef().ServerProcessId;
			serverHeaderBox.Dispose();
			throw std::runtime_error(Tools::IsProcessAlive(previousServerProcessId)
				? "ServerContext.Connect: " + serverNameStr + " is already running as process " + std::to_string(previousServerProcessId) + "; stop it before starting another"
				: "ServerContext.Connect: " + serverNameStr + "'s previous server (process " + std::to_string(previousServerProcessId) + ") has exited, but other processes still map its shared memory; stop its clients (Local Mirror, algos, Workspaces) and start again");
		}

		return serverHeaderBox;
	}

	// --- Global Implementations ---
	using Context::GetPositionHeader;

	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) override
	{
		return _serverPositionHeaders[instrumentId];
	}

	using Context::GetRiskLimit;

	Socket::SharedArrayEntry<Execution::RiskLimit>& GetRiskLimit(int32_t instrumentId) override
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _serverRiskLimits[instrumentId];
	}

	Socket::SharedArrayEntry<Execution::WorkingRisk>& GetWorkingRisk(int32_t instrumentId) override
	{
		ThrowIfInstrumentIdOutOfRange(instrumentId);
		return _serverWorkingRisks[instrumentId];
	}

	Tools::Bitset64 InstrumentIds() override
	{
		Provider::ServerHeader serverHeader = ServerHeader().GetReadonlyRef();
		return serverHeader.InstrumentIds;
	}

	// --- Specific Server Expositions ---
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
            return static_cast<int32_t>(Socket::ConnectRefusal::ReservedClientName);

        Socket::SharedArrayEntry<Provider::ServerHeader>& serverHeaderEntry = ServerHeader();
        Provider::ServerHeader& serverHeader = serverHeaderEntry.GetRef();
        Tools::Bitset64& clientIds = serverHeader.ClientIds;

        int32_t existingClientId = -1;
        if (TryGetClientId(socketHeader.ClientName, existingClientId))
            return existingClientId;
        int32_t clientId = socketHeader.ClientId < 0 ? clientIds.LowestClear() : socketHeader.ClientId;

        if (clientId < 0)
            return static_cast<int32_t>(Socket::ConnectRefusal::NoClientIdAvailable);

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
        header.ProductGroupId = -1;
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

		_serverRiskLimits.GetEntry(instrumentId).Write(Execution::RiskLimit::FromFile(GetRiskLimitsFilePath(DirectoryPath, symbology->Symbol()).string(), instrumentId));

		std::string positionPath = GetPositionFilePath(DirectoryPath, symbology->Symbol()).string();
		std::optional<std::string> positionLine = Tools::ReadLastLine(positionPath);
		Execution::PositionHeader positionHeader = positionLine ? Tools::Json::Deserialize<Execution::PositionHeader>(positionLine.value()) : Execution::PositionHeader{};
		// Server-wide row: no owning order; a template id (Generation 0) carries the ids.
		// Client 0 stands in for the old -1 sentinel (-1 is unrepresentable in 6 bits) - display/JSON-only change.
		positionHeader.OrderHeader.OrderId = Execution::OrderId().InstrumentId(instrumentId);
		// Why whatever the file says: the master switch is a kill switch, Live at every start until a server workspace pauses it
		positionHeader.AlgoStatus = Execution::AlgoStatus::Live;
		_serverPositionHeaders[instrumentId].Write(positionHeader);
		_serverWorkingRisks[instrumentId].Write(Execution::WorkingRisk{ .Position = positionHeader.Quantity });

		header.ProductGroupId = static_cast<int8_t>(AllocateProductGroupId(header));   // Why a byte fits: one row per instrument id, at most 64
		header.InstrumentId = instrumentId;
		serverHeader.InstrumentIds.Set(instrumentId);

		serverHeaderEntry.ReleaseLock();

		return instrumentId;
	}

private:
	// Returns the message-efficiency row of this instrument's product group, claiming the first empty row and loading the group's
	// file into it on the group's first instrument. Admin thread, at allocation only.
	int32_t AllocateProductGroupId(const Data::InstrumentHeader& header)
	{
		// Step 1: an instrument of a group already claimed shares its row
		Tools::String4 productGroupRoot = header.ProductGroupRoot;
		if (productGroupRoot.ToString().empty())
			throw std::runtime_error("ServerContext::AllocateProductGroupId: instrument header " + std::to_string(header.InstrumentHeaderId) + " (" + header.Root.ToString() + ") has no ProductGroupRoot, must have one; the feed or simulator that builds the header sets it");
		int32_t productGroupCount = ServerHeader().GetReadonlyRef().InstrumentIds.Length();
		int32_t productGroupId = 0;
		for (; productGroupId < productGroupCount && !_messageEfficiency[productGroupId].IsEmpty(); productGroupId++)
		{
			if (_messageEfficiency[productGroupId].GetReadonlyRef().ProductGroupRoot == productGroupRoot)
				return productGroupId;
		}
		if (productGroupId == productGroupCount)
			throw std::runtime_error("ServerContext::AllocateProductGroupId: " + productGroupRoot.ToString() + " needs a row, but all " + std::to_string(productGroupCount) + " are taken; raise InstrumentIds' capacity");

		// Step 2: the group's first instrument claims the next row and loads the group's last day from its file
		std::optional<std::string> messageEfficiencyLine = Tools::ReadLastLine(GetMessageEfficiencyFilePath(DirectoryPath, productGroupRoot.ToString()).string());
		Execution::MessageEfficiency messageEfficiency = messageEfficiencyLine ? Tools::Json::Deserialize<Execution::MessageEfficiency>(messageEfficiencyLine.value()) : Tools::Clock::Mode() == Tools::ClockMode::Simulation ? Execution::MessageEfficiency::GetMaxLimits(productGroupRoot) : Execution::MessageEfficiency::GetMinLimits(productGroupRoot);
		if (Tools::Clock::Mode() == Tools::ClockMode::Simulation)
			messageEfficiency.TradeDate = Tools::DateTime::MinValue();
		messageEfficiency.ProductGroupRoot = productGroupRoot;
		messageEfficiency.ProductGroupId = productGroupId;
		messageEfficiency.Reset(Tools::Session::CME().ConvertToLocal(Tools::Clock::UtcNow()));
		_messageEfficiency.GetEntry(productGroupId).Write(messageEfficiency);
		return productGroupId;
	}

public:
	void AllocateInstrument(int32_t clientId, int32_t instrumentId)
	{
		int32_t instrumentHeaderId = GetInstrumentHeaderIdByInstrumentId(instrumentId).GetReadonlyRef();

		if (InstrumentIds()[instrumentId] == false)
		{
			return;
		}

		// Already allocated: the live row is fresher than its file and may be mid-fill on the
		// CoreGroup thread - initialise once. Without this, the strategy-0 union rule re-ran the
		// whole body on every other client's allocation of the same instrument, re-reading the
		// position file and rewriting a live row (forcing Paused) from the admin thread.
		if (GetInstrumentIdsByClientId(clientId).GetReadonlyRef()[instrumentId])
			return;

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

		// A backtest has no operator to un-pause it, and the RiskLayer rejects a paused algo with
		// AlgoIsPaused. Realtime is forced Paused rather than left to whatever the restored row said:
		// a persisted Live would otherwise re-arm a strategy at startup with nobody asking for it.
		positionHeader.AlgoStatus = Tools::Clock::Mode() == Tools::ClockMode::Simulation ? Execution::AlgoStatus::Live : Execution::AlgoStatus::Paused;

		GetPositionHeader(clientId, instrumentId).Write(positionHeader);
		// Why the server seeds it too: the row is then readable (a remote desk's Risk Limits) before the strategy first runs; the strategy reseeds it at its start
		_localWorkingRisks[GetLocalPositionIndex(clientId, instrumentId)].Write(Execution::WorkingRisk{ .Position = positionHeader.Quantity });
		GetRiskLimit(clientId, instrumentId).Write(Execution::RiskLimit::FromFile(GetRiskLimitsFilePath(clientName, symbology->Symbol()).string(), instrumentId));

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
		_instrumentIds = &_instrumentIdsByClientId[ClientId].GetReadonlyRef();

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
		int32_t clientId = -1;
		if (TryGetClientId(Tools::String128(clientName), clientId))
			return clientId;
		throw std::runtime_error(std::string(typeid(*this).name()) + ".GetClientIdFromMap(" + clientName + "), Client not connected to server '" + ServerName.string() + "'.");
	}

	// --- Local Implementations ---
	using Context::GetPositionHeader;

	Socket::SharedArrayEntry<Execution::PositionHeader>& GetPositionHeader(int32_t instrumentId) override
	{
		int32_t localPositionIndex = GetLocalPositionIndex(ClientId, instrumentId);
		return _localPositionHeaders[localPositionIndex];
	}

	using Context::GetRiskLimit;

	Socket::SharedArrayEntry<Execution::RiskLimit>& GetRiskLimit(int32_t instrumentId) override
	{
		return GetRiskLimit(ClientId, instrumentId);
	}

	Socket::SharedArrayEntry<Execution::WorkingRisk>& GetWorkingRisk(int32_t instrumentId) override
	{
		return _localWorkingRisks[GetLocalPositionIndex(ClientId, instrumentId)];
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

inline Profit Position::GetProfit()
{
	if (_headerEntry.IsEmpty())
		return Profit(Tools::Clock::UtcNow(), std::nan(""), std::nan(""), 0.0, 0, std::nan(""), std::nan(""));

	Execution::PositionHeader positionHeader = _headerEntry.Read();
	Data::Quote quote
	{
		.TickSize = 0.0,
		.Bid = Data::Level { .Ticks = 0, .Quantity = 0 },
		.Ask = Data::Level { .Ticks = 0, .Quantity = 0 }
	};

	if (Instrument.TryGetQuote(quote))
	{
		double floating = Instrument.GetProfit(positionHeader.AvgPrice, quote.MidPrice(), positionHeader.Quantity);
		return Profit(Tools::Clock::UtcNow(), floating + positionHeader.RealizedProfit, floating, positionHeader.RealizedProfit, positionHeader.Quantity, positionHeader.AvgPrice, quote.MidPrice());
	}
	else
	{
		return Profit(Tools::Clock::UtcNow(), std::nan(""), std::nan(""), positionHeader.RealizedProfit, positionHeader.Quantity, positionHeader.AvgPrice, std::nan(""));
	}
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

	if (Instrument.Header().TradingStatus != Data::TradingStatus::Open || mbp.BidsCount() == 0 || mbp.AsksCount() == 0)
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