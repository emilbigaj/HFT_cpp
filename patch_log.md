# Patch log

Newest first. Each entry says what changed, why, and what it broke or unblocked.

---

## Names are 256 bytes (C# `8cf5c1b`+`642cc0c`, report 2026-10-12 P30, C19, C20)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §10. Nothing for C# to change.

- **P30** (WIRE, lockstep at a week start with fresh shared memory): name fields become `Tools::String256`.
  - `SocketHeader::ServerName`/`ClientName` (`Socket.hpp`): the header grows to 600 bytes (was 344), and every later offset moves by +256.
  - `ServerHeader::ServerName` (`Allocate.hpp`): `Persistance` moves to 300 and `ServerProcessId` to 301, `sizeof` 305 (was 177).
  - `AllocateClient::ClientName`.
  - `Context::TryGetClientId` takes a `String256`, and the reserved-name check and `GetClientIdFromMap` build one.
  - A name past 256 bytes throws where it is built (`StringN`, as before).
- **C19 checked:** `Server::OnControlAlgoStatus` turns the master row for strategy 0 (P25). No change.
- **C20, and C#'s `Tick.SizeOf`/`TickType.IsTick`:** C#-only. C++ has no `Tick` struct or tick history.

**CME must change one line** (it no longer builds without it): `Server/CmeServerConfig.hpp:246` builds `serverHeader.ServerName`
with `Tools::String128(...)`, and it must be `Tools::String256(...)`. Not done: CME is never edited from here.

Verified: HFT builds clean; ToolsTests, DataTests and ExecutionTests pass. The JSON harness shows 0 of 159 files differing, with a client name of about 220 bytes.
Against the real sockets, a 200+ byte client name connects and its session is open, a 300-byte name is refused, and the rest of the
handshake scenarios still pass. The `ServerContext::Connect` pid checks and the P24–P28 tests pass.

---

## OrderRisk rows leave shared memory (C# `5556885`..`a80baf2`, report 2026-10-12 P29, C18)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §9. Nothing for C# to change.

- **P29 ported** (optional in the report, done for parity). `RiskLayer` owns `std::vector<OrderRisk>` sized
  `OrdersCapacity`, indexed by `OrderId::GlobalIndex`, zero at start, behind a private `GetOrderRisk`. The
  `<directory>/OrderRisks` region and `Context::GetOrderRisk` are deleted. No other process ever read the region (nor did CME).
- **C18** (a C# strategy's book is a field of its `Instrument`): nothing to port. The C++ client keeps its replica in
  `<client>/MarketsByPrice`, and `<server>/MarketsByPrice` is unchanged.
- The C# array-id regrouping is C#-only (C++ has no array ids).

Verified: HFT and CME build clean (CME unchanged). ToolsTests, DataTests and ExecutionTests pass, and the JSON harness shows 0 of 159 files differing.
The `ServerContext`/`RiskLayer` test (17 checks) and the P24 test pass.

---

## Strategy exposure rows under the server's name (C# `d4f6402`, report 2026-10-12 P28)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §8. Nothing for C# to change.

- **P28** (`Context.hpp`, WIRE, L11): the per-directory `WorkingRisks` region is replaced by two regions under the server's name.
  - `<server>/ServerWorkingRisks` (per instrument) is the server's.
  - `<server>/LocalWorkingRisks` (per strategy and instrument, writable by the server and each strategy) is new.
  - `GetWorkingRisk(instrumentId)` is now virtual: the server's row on the server, the strategy's own on a client.
  - `ServerContext::AllocateInstrument(clientId, ...)` seeds the strategy's row from its position. The C++ client already
    reseeds it at start.
- **No "Server" getters**, as C#: `GetServerRiskLimit` and `GetServerPositionHeader` are deleted. The server uses its own
  `GetRiskLimit(instrumentId)` and `GetPositionHeader(instrumentId)`.
- Not ported (C#-only): `Application.DetachConsole`, the risk desk publish package, Workspace config.

Verified: HFT and CME build clean (CME unchanged). ToolsTests, DataTests and ExecutionTests pass, and the JSON harness shows 0 of 159 files differing.
The real `ServerContext`/`RiskLayer` test now has 17 checks, all passing. The P28 ones: the strategy's `LocalWorkingRisks` row is seeded at allocation, and
`ServerWorkingRisks` holds the firm's row.

---

## A manual order is never limited by fills (C# `f6b4f52`+`edf71e4`, report 2026-10-12 P26 updated)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §7. Nothing for C# to change.

- `WorkingRisk` (WIRE, L11): `WorstLongQuantity`/`WorstShortQuantity` are renamed `WorstWorkingLongQuantity`/`WorstWorkingShortQuantity`,
  and `WorstWorkingFills` (@20) is new, so the row grows to 24 bytes.
- `RiskLayer` (`RiskLayer.hpp`):
  - `ApplyWorstQuantityDelta` adds an algo order's delta to `WorstWorkingFills`.
  - `OnFill` counts only algo fills into `FillsThisSession`.
  - The fills room is `MaxFillsPerSession - FillsThisSession - WorstWorkingFills`.
  - `ValidateOrder` checks the fills room for algo orders only.

  An operator's manual exit is never refused 57, its fills never count, and its working lots take no fills room.

Verified: HFT and CME build clean (CME unchanged); ToolsTests, DataTests and ExecutionTests pass (the size check updated to 24). The JSON
harness shows 0 of 159 files differing. The `ServerContext`/`RiskLayer` test now has 14 checks, all passing. The new ones: a manual fill moves the
position but not the count; with the algo at the limit there is no room; a manual resting buy of 50 takes no fills room.
(The scratchpad C# clone was corrupted by this pull, with the working tree missing files; re-cloned, same commit `edf71e4`.)

---

## Master algo switch, max fills per session, strategy risk limits (C# `9f8328f`+`8b0f60f`, report 2026-10-12 P25-P27)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §6. Nothing for C# to change.

- **P25, the master algo switch** (`Context.hpp`, `RiskLayer.hpp`, `Server.hpp`):
  - `ServerPositionHeaders` moves from `ServerContext` to every context, so clients map it read-only.
  - New `Context::GetServerPositionHeader` and `Context::IsAlgoLive(strategyId, instrumentId)`: an algo order needs both its
    own row and the server row `Live`. `RiskLayer::ValidateOrder` checks both, on the server and the client.
  - `Server::OnControlAlgoStatus` with strategy 0 turns the server row; with any other strategy, that strategy's row, as
    before. Both are put on record through the new `WriteToRecord`: strategy 0's row on the audit channel, any other
    strategy's on its execution channel.
  - `ServerContext::AllocateInstrument` sets the server row `Live` at every start, whatever the file says.
- **P26, max fills per session** (WIRE, L11):
  - Layouts: `RiskLimit` gains `MaxFillsPerSession` (28 bytes); `WorkingRisk` renames `Worst*WorkingQuantity` to
    `Worst*Quantity` and gains `FillsThisSession` (20 bytes); `ControlRiskLimit` gains the field.
  - Reason 57 is renamed `TooManyFillsPerSession`.
  - `RiskLayer::OnFill` counts |quantity|, and `OnSessionClosed` zeroes the count. The server and client both call it
    on `Closed`.
  - `GetAbsAllowedOrderQuantity` takes `isFillsRoom`. `ValidateOrder` checks the fills room after the position room,
    and `TryClipToRiskLimit` takes the smaller of the two.
  - **Deleted the client's simulation-only discard of reason 57** (`Client::IsDiscarded`). C# deleted it too; the server and
    client now use `OrderRejected::IsDiscarded()` alone.
  - `RiskLimit::FromFile` added.
- **P27, strategy risk limits** (WIRE, L11):
  - Regions: `RiskLimits` is renamed `ServerRiskLimits`, and `LocalRiskLimits` [client, instrument] is new.
  - `ControlRiskLimit` gains `StrategyId` (28 bytes).
  - `Context::GetRiskLimit(instrumentId)` is now virtual: the firm's row on the server, the strategy's on a client.
    `GetRiskLimit(clientId, instrumentId)` and `GetServerRiskLimit` are added.
  - `ServerContext::AllocateInstrument(clientId, ...)` loads the strategy's row from `<strategy>/RiskLimits/<symbol>.risklimit`.
  - `Server::OnControlRiskLimit` edits the firm's row for strategy 0 and otherwise the strategy's, then puts it on record.
  - The client ignores a `RiskLimit` on its execution channel.
- Not ported (C#-only, no C++ caller): `Position::IsAlgoLive` (C++ has no algo layer), and the GUI helpers
  (`OrderState::NewCancel`, `TicksToPriceString`/`PriceFormat`, `Context::DirectoryName`, `TryReadMarketByPrice64`).

**Before trading on a realtime server** (L11): a firm `.risklimit` line written before today loads `MaxFillsPerSession` 0, which
refuses every order. Each strategy with no `<strategy>/RiskLimits/<symbol>.risklimit` gets Min limits, so its own RiskLayer
refuses every algo order. Set both first.

Deploy: the C++ server, CME rebuilt, and every C# process on the box together (L11; L10 still applies).

Verified: HFT and CME build clean (CME needs no change for this set). ExecutionTests (two size checks updated to 28/20), DataTests and
ToolsTests pass. The JSON harness shows 0 of 159 files differing, with the new fields and reason 57's name. Against the real
`ServerContext` and `RiskLayer`, all 11 checks pass:
- a pre-field firm line loads 0;
- the server reads the firm's row, and the strategy's row loads from its own folder;
- the master starts `Live` over a `Paused` file, and pausing it stops the algo without touching its own switch;
- with limit 10 and 4/4 working, a buy of 3 is refused on fills room and a buy of 2 passes;
- fills count both sides, and `Closed` zeroes the count but not the position.

`Server::OnControlAlgoStatus`/`OnControlRiskLimit` were not run end to end (they need a full server).

---

## Better-price delete, server-owned message efficiency (C# `a7accf1`..`194f8cf`, report 2026-10-12 P23, P24)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` §5. Nothing for C# to change.

- **P23** (`Data/MarketByPrice.hpp`): `SideByPrice64::TrySetQuantity` ignores a delete of a price better
  than the best. It used to clear the worse level sharing that ring slot and report delta 0, so the
  server's book lost the level while every client kept it. Before the fix, a fuzz of a server book
  against a replica fed its deltas diverged; after it, 200 runs stay identical, and scenario 19 passes.
- **P24, message efficiency moves to the server** (WIRE, lockstep with C#, L10). This reverses
  the 2026-10-05 item 8 decision ("C#-only"): the user chose the full port.
  - `InstrumentHeader`: `int8 ProductGroupId` at offset 7 (was reserved) and `String4 ProductGroupRoot`
    at offset 60 (was reserved); both added to the JSON.
  - `OrderType::MessageEfficiency = 18`.
  - `Execution/RateLimit.hpp`: `MessageEfficiencyTier` and the 84-byte `MessageEfficiency` row (header,
    `Reset`, `Efficiency`, `OnFill`, `GetMaxLimits`/`GetMinLimits`). `CanSend`/`Send`/`TrySend` are not
    ported: the user decided only C# uses message efficiency, and C++ only reads and writes the row (never counts).
    `RateLimit.hpp` now includes `Order.hpp` (for `OrderType`) instead of the reverse, as in C#; `Context.hpp`
    and `RiskLayer.hpp` include it directly.
  - New `Tools/DateTime.hpp`: `Tools::DateTime` and `Tools::TimeSpan`, 8-byte types wire-compatible with
    .NET, with System.Text.Json's text forms.
  - New `Tools/Session.hpp`: `Session::CME().ConvertToLocal(Timestamp)`, the America/Chicago conversion
    C#'s `Session.CME` does. Only that part of C#'s `Session` is ported.
  - `Context`: maps `<server>/MessageEfficiency`, writable by the server, and creates the
    `MessageEfficiency` folder. It adds `GetMessageEfficiency` and `GetMessageEfficiencyFilePath`.
    `ServerContext::OnInstrumentHeader` sets `ProductGroupId = -1`, and `AllocateInstrument` sets it from
    `AllocateProductGroupId`. That shares a group's row or claims and loads the next one, and **throws
    when the header has no `ProductGroupRoot`**.
  - `Server`: a `Closed` status ends the group's day (`EndMessageEfficiencyDay`; in Realtime, the ended row
    goes on the audit channel). `Dispose` posts every row in Realtime before the audit link closes.
  - `Instrument::ProductGroupId()` reads the header.

**CME must set `ProductGroupRoot`** before this server can allocate an instrument:
`SecDef/SecDefFile.hpp` `FillCommon`, `header.ProductGroupRoot = Tools::String4(p.SecurityGroup);` (tag 1151,
already parsed). A group longer than 4 characters throws there (`StringN` refuses to cut). Not done:
CME is never edited from here.

Deploy: the C++ server, CME rebuilt with the line above, and every C# process on the box together (L10).

Verified: HFT and CME build clean; ExecutionTests, DataTests and ToolsTests pass. The JSON harness shows 0 of 159
files differing, with new samples: the row, max/min limits, CME local time in summer and winter, a UTC `DateTime`,
`Reset` (true, false, then true a day later), and loading an old-format line (with `ProductGroup` and no `Header`)
and a line with a fractional date. Against the real `ServerContext`, MNQ and NQ share row 0, M2K (RY) and ES claim
rows 1 and 2, ES loads its tiers from an old-format file, the day runs to tomorrow in CME local time, and an empty
root throws. The `Closed` handler and the shutdown post were not run end to end (they need a full server with
Logging).

---

## Connect handshake, restart admission on the server, close-out of a gone client (C# `0d47822`+`d9fb359`+`b7136e8`, report 2026-10-12)

**FOR C# CLAUDE:** reply in `csharp_alignment_report_2026-10-12.md` (§12 confirmations; note the
C++ server does NOT rewrite `ServerHeader.Timestamp` every pass).

Ported `cpp_alignment_report_2026-10-12.md` P1–P12, P14–P20 (P13, P21: nothing to do; R1–R8 were
never ported):
- **Handshake** (`Socket.hpp`): `ConnectRefusal` -1..-7, `ConnectRefusedException`, `IsClientReady`.
  The server answers every connect exactly once (`PollLetterBox` + `DecideConnect`, a request's own
  `ClientId` ignored, `Closing`/`Closed` refused, socket built before `ClientAllocated`),
  `ReplyToClient` empties a dead client's leftover reply, the listen loop catches and keeps going.
  `ClientSocket::Connect` returns the id or throws the refusal, retries `Closing`, matches name +
  timestamp + pid, and a refusal disposes without closing (a refused process must not end the live
  session under its name). `Application::AddExitAction` returns a cancellable `ExitAction`;
  `ClientSocket::Dispose` and its destructor cancel the "close at exit" action.
- **Allocation** (`Context.hpp`, `Server.hpp`): `AllocateClientId` returns `ReservedClientName` /
  `NoClientIdAvailable` instead of throwing (a throw killed the listen thread); `TryGetClientId`;
  `LoadClients` throws on a refusal.
- **Restart admission moves to the server** (`Server.hpp`, `Client.hpp`): `CancelAllOrders` is now
  `TryCloseOutOrders` (one walk; "working" decided by the state row once it is this order's; an unread
  Create stamped Done once every CoreGroup thread has passed since the close — the new drain
  handshake; a torn idle target row repaired). `IsClientReady = TryCloseOutOrders`, so a restart is
  refused `NotReady` while a previous order still works, and its cancels are re-sent. The server writes
  each injected cancel to the client's channel (audit); clients ignore `OrderTarget` there. **The
  client constructor's previous-orders check, 100 ms sleep and `OrderRisk` clear are removed** (the
  item-2 change of 2026-10-05, superseded by C#); `Recover` stays. `Client::Dispose` no longer refuses
  with 64 active slots; a throw after `Connect` closes the socket (guard member).
- **Shared memory** (`Protocol.hpp`, `SharedArray.hpp`): `Protocol::Stamp`; write-capable arrays
  frame every row, so lock-edited rows (order states, positions) read through `TryRead`. Ring readers
  no longer latch closed.
- **Order rows** (`Order.hpp`): `OrderStateReason::PendingNew` → `InFlight` (audit JSON writes the
  name: lockstep with C#), builders `NewInFlight` / `NewOrderRejected` / `NewRejected`, constant
  `OrderDiscardedMask`, `AheadOfOrder::Quantity` → `QuantityAhead`.
- **Leg lookup** (`Context.hpp`): the context that installed `LeggedHeader::GetLegHeader` clears it on
  destruction (C++ clients have no `ServerContext`, so "first context wins" is kept).
- **P22, `ServerHeader.ServerProcessId`** (`Allocate.hpp`, `Context.hpp`): new int32 at offset 173,
  header 173 → 177 bytes (WIRE, lockstep with every C# process). `ServerContext::Connect` stamps
  `getpid()` when it posts the header. A refused post never overwrites the old header: it reads the
  posted pid and throws "already running as process <pid>" when that process is alive, otherwise
  "previous server (process <pid>) has exited, but other processes still map its shared memory".
  Verified against the real `Connect`: the posted id is this process's, a second post under a live
  server and a post over a dead run's header both throw their message, and the dead header is kept.

Deploy: in lockstep with C# (connect handshake, `InFlight`); the server before any client that drops
its startup check. CME needs nothing new from this set beyond the six lines already listed.

Verified: build clean; ExecutionTests, DataTests, ToolsTests pass; JSON harness 0 of 140 differ.
Scenario program against the real sockets: second connect under a live name refused
`AlreadyConnected` with the live session still `Open` and delivering, `NotReady` via the hook, an
allocator refusal delivered as its reason, a reader reading on after a close message, and the stamp
(lock-edited row reads `New`, unwritten row `Empty`, a second opener leaves the sequence alone).
`TryCloseOutOrders` / drain not run end to end (needs a full server with a live directory).

---

## Second shutdown signal no longer aborts the cleanup (2026-10-05)

Found on New Release: `timeout` signals both the program and its process group, so SIGINT arrived twice;
the second landed on another thread, `OnExit` returned at once and the handler called `std::exit`,
killing the process mid-cleanup (no cancel-all, no Terminate on segments 68/74). A double Ctrl+C did
the same. `Tools/Application.hpp`: the signal handler now ignores a signal while a shutdown is running,
and `OnExit` returns whether this caller claimed the shutdown, so two simultaneous signals cannot both
reach `std::exit`. The handler never waits (that deadlocked against `CmeServer::Stop`'s joins in
`4c487be`). C# already blocks a second `OnExit` until the first finishes, so the outcome matches; nothing
for C#. Verified with a program whose exit action takes 500 ms, two SIGINTs to its process group 100 ms
apart: the old handler exited before the action finished, the new one completes it.

---

## C++/C# review, item by item (2026-10-05) — see `csharp_alignment_report_2026-10-05.md`

**FOR C# CLAUDE:** read `csharp_alignment_report_2026-10-05.md` (repo root, branch
`persist-client-sockets` — `main` is stale). Its table lists all 28 reviewed items; §1–§7 are the
changes **C# must implement**: restart replay through the client path (§1), client startup refusal +
100 ms + backlog skip (§2), `mlockall` failure throws (§3), SIGHUP honours `nohup` (§4), server-name
check before `InitDirectories` deletes (§5), `SharedArray` > `int.MaxValue` throws (§6), two `Clock`
bugs (§7). Already done in C#: `ProcessId.IsAlive` pid ≤ 0 guard (`8a8bffc`).

Each item was reviewed with the user one at a time, C++ and C# code side by side. C++ changes:

- **Client startup** (`Client.hpp`, `Socket.hpp`): the constructor refuses to start if any of the
  client's 64 order slots is Active, clears the Done slots' `OrderRisk`, sleeps 100 ms, then
  `_socket.Recover()` skips every queued message (new `ClientSocket::Recover` passthrough). The
  per-instrument check at allocation is gone; allocation only seeds `WorkingRisk`.
- **Refused targets** (`Server.hpp`): `OnOrderTarget` always writes and publishes the PendingNew row
  and routes every refusal through `OnOrderRejected`; a refused Create's Done copies the order row and
  takes the reject's header and profile (keeps `ExchangeOrderId`) — matches C# as supplied by the user.
- **Server name validated before anything is opened or deleted** (`Context.hpp`, `Server.hpp`):
  `ThrowIfInvalidServerName` is a static on the base `Context` and runs in its first initialiser for
  every context; `Server::InitDirectories` calls it first, so a simulation against a live server's name
  throws before any file is deleted. The C++-only "no simulation on this server" throw is kept.
- **`RiskLayer::TryClipToRiskLimit`** ported verbatim (unused until a C++ Algo layer exists).
- **Clock** (new `Tools/Clock.hpp`, `Tools/Event.hpp`, `Tools/LockedPriorityQueue.hpp`): full port of
  C# `Tools/Clock.cs` (reminders, events, `Start`/`Stop`, `Begin`/`End`, `OnInterject`,
  `SimulationSpeed`, simulation/realtime loops, "Stop Clock" exit action). `Provider::Clock` deleted;
  every user moved to `Tools::Clock` (`Clock::UtcNow()`, `Mode()`/`SetMode()` throwing while running).
  `RiskLimit::GetMaxLimits/GetMinLimits(instrumentId)` stamp `Clock::UtcNow()` themselves, as C#.
  The sample `Strategy/Scenario.hpp` drives time through the Clock like C# `Program.cs`. Two C# Clock
  bugs fixed here and handed to C# (report §7).
- **Market-by-price** (`Client.hpp`, `Instrument.hpp`, `Strategy.hpp`): C#'s Delta/Update/Snapshot
  branches with stale ticks dropped; the per-delta event is now `Instrument::MarketByPriceDelta(delta,
  bytes)` as in C#, replacing the C++-only `Client::MarketByPrice` callback.
- **JSON is byte-identical to C#** (`Json.hpp`, `Logger.hpp`, metas across Data/Execution/Provider/
  Socket): 2-space indent, .NET number format (15 significant digits, no exponent, NaN/Infinity as
  strings), .NET string escaping, C# computed properties written (never read), C# key order. Proven by
  a C#/C++ harness over 140 files (scratchpad `jsonparity/run.sh`); C++ still reads old C++ and C#
  output. Doubles are now written with 15 significant digits, as C# writes them.
- **`RateLimit` / `RollingRateLimit`** moved from `Order.hpp` into `Execution/RateLimit.hpp` next to
  `SessionRateLimit`, as in C#. `Settlement` gained C#'s constructor.
- Renames: `Clock::GetUtcNow` → `Clock::UtcNow`, `_allocatedInstrumentIds` → `_instrumentIds`.

**CME repo (`~/cpp/CME`, not touched) needs these updates to build against this tree:**
`Server/CmeServer.hpp:268` (`OnQuantityAhead` third argument `quantityBehind`, 0 live),
`Server/CmeServer.hpp:685`, `ILink3/InstrumentRouter.hpp:387`, `Tests/RouterTests.cpp:136`
(`OrderTargetAction::Amend` → `Replace`, `Reduce` takes the modify path), `Server/Main.cpp:40` and
`Tests/ServerTests.cpp:61` (`Provider::Clock::Mode = …` → `Tools::Clock::SetMode(…)`). Since the server
now publishes the Done for a refused Create, the router's own post-reject Done is a duplicate.

Verified: build clean (no warnings); ExecutionTests, DataTests, ToolsTests (with new Clock tests) pass;
JSON harness 0 of 140 files differ.

---

## Regressions in `4c487be`, fixed

Found by a post-commit regression hunt (each finding upheld by two independent refutation
attempts). All came from changes beyond the 2026-10-04 report:
- **`IsProcessAlive` lost its `pid <= 0` guard** (copied from C#): `kill(0, 0)` targets the process
  group and succeeds, so a client slot with pid 0 never closed and its orders were never
  cancelled. Guard restored. **C# has the same bug** (`ProcessId.IsAlive_Linux`) — add the guard there.
- **`LoadInstruments` replayed through the server-only `OnAllocateInstrument`** (C# parity): after a
  restart no client/house-book allocation and no `AllocateInstrument` callback, so the CME adapter
  built no routers or market data — fills for orders still working at CME were dropped as
  "unrouted" and cancels never left. Restored to the client path.
- **Signal handling rewrite** (second caller waited on the first exit chain, plus OnExit via atexit):
  a second Ctrl+C/SIGTERM/SIGHUP during shutdown deadlocked a signal-handling background thread
  against `CmeServer::Stop`'s join. Reverted `Application.hpp`, `Logger.hpp` and the socket exit
  actions to the previous code; SIGHUP is now handled like SIGINT unless inherited as ignored
  (`nohup`).
- **`MLock` stopped throwing** (C# parity): a host without the memlock ulimit would trade unpinned.
  Throwing restored.

Not changed (C# design, flagged to the user): the client-side position check measures the
strategy's own position against the server-wide `MaxPositionQuantity`, so with offsetting books it
can refuse (and pause) what the server would accept; the startup `WorkingRisk` seed can double-count
a fill that lands between socket connect and seed (report §4.6 accepted residual).

---

## Client-side risk copy, RiskLimit/WorkingRisk split, Reduce, duplicate-fill drop (C# `fdae2ab` + `8f229aa`, report 2026-10-04)

**FOR C# CLAUDE — read this first.** The report's §0 "Baseline" and §1.9 claims about the C++ tree
(no `RiskLimit.Timestamp`, no `OrderTarget.TimeInForce`, no `TimeInForce::Day = 0`, discard set
missing 45) and its C++ line numbers were taken from branch `main`, which is still the initial
commit. All C++ alignment work lives on **`persist-client-sockets`**; diff against that branch.
Two "already handed off" items were in fact NOT yet ported and are done here: `SideByPrice64.Quantity`
and SIGHUP. C++ has no TCP mirror (the mirror is C#-only and numbers arrays by its own C# creation
order), so the missing C++ `MessageEfficiency` array does not shift mirror ids.

WIRE (deploy in lockstep with C#): `RiskLimit` 24 B (Worst* removed); new `WorkingRisk` 16 B,
`OrderType::WorkingRisk = 17`; `OrderRisk` still 64 B but `AbsAckedOrderQuantity@4`,
`AbsOrderQuantities[29]@6`, `MaxActiveTargets = 29`, zero-arg `GetAbsWorstOrderQuantity()`;
`OrderTargetAction {Create 0, Replace 1, Cancel 2, Reduce 3}`; `OrderProfile::IsReduceOf`;
`OrderState` 64 B with `QuantityBehind@60` (2026-09-26 item); `AheadOfOrder` 20 B;
`SideByPrice64.Quantity@17` (320 B kept), `MarketByPrice64` pack 1 / 664 B; new `Settlement` 64 B
tick. Shared arrays: `<dir>/WorkingRisks` (new, last base array), `<dir>/OrderRisks` now per
context, base-array creation order matches C#. Static asserts for every size/offset above.

RiskLayer runs on server AND client over its own context (`RiskLayer(Context&, source)`): server-
only early returns removed; `ApplyWorstWorkingQuantityDelta` seq-bumps `WorkingRisk`;
`GetAbsAllowedOrderQuantity` (int64, floor for negative room) + `IsWithinRiskLimit` check-then-
commit; rate limit counts Cancel and a true `Reduce` (IsReduceOf vs STATE row) without refusing;
`OnOrderState(state)` one-arg; `OnFill(fill, isReserved)` moves `WorkingRisk.Position` for every
fill and releases only non-legged; `OnOrderRejected` ignores own-source and Cancel.
`TryClipToRiskLimit` not ported (Algo-only).

Server: resent fills dropped whole (`|reported filled| <= |row filled|`, before any stamp/lock);
a manual order's reject never pauses (both sites); `RiskLayer::OnFill` for every fill; refused
Create publishes Done/Rejected through `OnOrderState` BEFORE the reject (`8f229aa`);
`OnQuantityAhead(id, ahead, behind)` one 64-bit atomic_ref store; every timestamp via the
sim-aware `Clock::GetUtcNow()`; `InitDirectories` before `Connect`; `LoadInstruments` replays
server-side only. Read-loop try/catch lives in the vendor loops (CME `CmeServer` already wraps
them in `TryCatch`). The C++-only "Simulation on live server" guard is kept deliberately.

Client: echo gate (`_ackedSeqs[64]`), `OnFill(fill, own ClientId)`, reject release before the
discard check, `IsDiscarded` with the sim-only TooManyOrdersPerSession (57) carve-out (C++ had
56 — wrong), spread-shape refusal (two-leg ±1 only) before onboarding, startup refusal on a
previous process's Active order + `OrderRisk` clear + `WorkingRisk` seed from own position;
`ApplyMarketByPrice` follows the C# Delta/Update/Snapshot branches. `Strategy::Amend` sends
Reduce when `IsReduceOf` the active profile, else Replace. Simulation algos allocate Live.

Tools/Socket parity: SIGHUP handled; Bitset64 JSON is a bare number; JSON accepts comments and
trailing commas; `StringN::Set` throws on overflow (no silent truncation); `Timestamp::FromString`
fallback formats; `MLock` reports instead of throwing; sockets/logger cancel their exit actions
in their destructors (C++-only: OnExit now also runs from atexit). Stale duplicate
`Execution/RateLimit.hpp` structs deleted.

CME adapter (`~/cpp/CME`, uncommitted there): router sends only the reject for a refused Create
(three sites: `ExecutionReportReject`, `OnRequestRefused`, `OnOrderRefused`); a refused modify
echoes the request's own action; `Reduce` already encodes as `NewReplace` (iLink modify, never a
default/throw branch); queue tracker passes `QuantityBehind = 0` (no queue model on a live
session); reject re-arm accepts Replace|Reduce.

Not ported (C#-only or decision items): MessageEfficiency array/structs (pre-existing gap, no C++
consumer); `TryClipToRiskLimit`; Algo/ActiveTarget/simulator/widgets.

Verified: build clean; ExecutionTests (incl. §6.2 OrderRisk differential test, 29-slot cap,
QuantityNotValid edge cases, fixed vector [1,9,7,9]), DataTests (200k-step book property test),
ToolsTests pass; CME adapter builds against the new headers and its unit tests pass. Five
independent reviewers (wire, risk, server, client, completeness) re-diffed against C#; one JSON
key regression found and fixed.

---

## Session state is the exchange's TradingStatus; Alert carries a Timestamp (C# `9155763`, 2026-09-24)

**TradingStatus gate**: C# deleted `Instrument.SessionManager`/`IsInSession`; ours was a
`return true` stub, so this side goes from "always in session" to the real gate. The three call
sites now test `Header().TradingStatus == Open`: `RiskLayer::ValidateOrder` rejects a create
`NotInSession` otherwise, and `Instrument::TryGetQuote` + the position quote return empty.
**Unknown counts as closed**: an instrument whose status was never published neither trades nor
quotes, so the C++ CME server MUST publish each instrument's status from the snapshot /
SecurityStatus at startup via `OnTradingStatusUpdate` (2026-09-10 report T1-T5) - this gate makes
that a hard requirement, not a display nicety.

**Alert wire** (`Provider/AlertManager.hpp`): now `Header | Timestamp | [OrderRejected | String64
Symbol] | Message`, matching C# `Alert.ToBytes/FromBytes` byte for byte (the GUI parses it).
`Timestamp` = sim-aware `Clock::GetUtcNow()` at construction on the raising thread. The
`String64 Symbol` after an OrderRejected was ALSO missing here (pre-existing divergence - our
port predated it); resolved on the alert thread via the header path (`GetSymbol`), never
`GetInstrument` (no lazy Instrument creation from that thread; a rejection may name an
instrument this client never allocated), `UnknownSymbol_<id>` when the lookup fails. glaze meta
gains Timestamp + Symbol to match C# JSON.

Not portable: `Clock.SimulationSpeed` pacing-break (C++ Provider::Clock has no pacing loop),
MessageEfficiency day-end rewire (no MessageEfficiency array here yet), simulator
SessionManagerByExchange + workspace speed drop-down (C#-only).

Verified: build clean, ExecutionTests + DataTests pass.

---

## CoreGroups from files, cancels never throttled, PendingNew queue seed (C# `a8fd70f`+`868467c`+`e5b8bfc`, 2026-09-23)

Three C# commits in one alignment; the ladder-layout commit (`bdd921f`) is widget-only.

**Cancels counted but never refused** (`a8fd70f`): `RollingRateLimit::SendOrder` rolls the ring
and counts like `TrySendOrder` but skips the Limit check (bucket byte still stops at 255);
`ValidateOrder` routes a Cancel through it and everything else through `TrySendOrder`. A cancel
is the message that reduces risk - found when a pause could not cancel the algo's own orders with
the limit set to 10. What the combined window now costs is create/amend throughput while cancels
fly, not cancel throughput.

**New shared array `CoreGroups`** (`868467c`): region `<server>/CoreGroups`, created directly
after `RateLimits` (array-id order for the mirror), `CoreGroupIds.Length()` rows of
`Provider::CoreGroup` - 36 B: `String16 CoreGroupName@0`, int32 `CoreGroupId@16`,
`ServerCoreId@20`, `MarketDataCoreId@24`, `StrategyCoreId@28`, `ReservedCoreId@32`, all ids -1
unset (offsets asserted). The `Data::CoreGroupId` naming enum is DELETED - each server names its
groups in `<server>/CoreGroups/<name>.coregroup` files (static whole-file pretty JSON, NOT the
appended-line `.risklimit` form; `Tools::ReadAllText` added for these). A ServerContext opened
for WRITE loads every such file into the row at its id - out-of-set OR out-of-range id throws
(C#'s indexer is bounds-checked; folding the range into the same throw keeps the loud fail
without Bitset64's UB shift) - then that group's rate limit from `RateLimits/<name>.ratelimit`
(whole-file JSON `RateLimit`, `Duration` in the canonical underscored string form both sides
already share), else `GetMaxLimits` (1 s, int32 max) in simulation / `GetMinLimits` (1 s, 0) in
realtime, id = CoreGroupId. `CMEOrderEntry` is DELETED and the Server-ctor seeding loop with it:
New Release, Certification and Production publish different limits, so the number is per server
directory. `Context::GetCoreGroup(id)` + `GetCoreGroupId(String16)` (scans set bits, throws when
absent) added; a realtime strategy pins to the row's `StrategyCoreId` - our sim-only Scenario has
no pinning path, so only the accessors port. `EnumerateCoreGroups` skipped like
`EnumerateRateLimits` (C# widget-only). NOTE the flip: a realtime CoreGroup with no `.ratelimit`
file now has Limit 0 - every non-cancel refused until configured - and a CoreGroup with no
`.coregroup` file has an unwritten (all-zero) rate-limit row; the C++ CME server must ship its
`.coregroup` files. C#'s Json resolver index-walk fix is System.Text.Json-specific, nothing to
port.

**PendingNew carries a provisional QuantityAhead** (`e5b8bfc` / report addendum): the Create
branch seeds `QuantityAhead` with the server's own `MarketByPrice64` quantity at the order's
price on its side (Bids for a buy, Asks for a sell), read before the order-row lock; 0 showed
every fresh order at the front of the queue for the whole round trip. The ack overwrites it;
`OnQuantityAhead` unchanged. The simulated-queue publish narrowing in the same commit is
simulator-only.

Verified: build clean, ExecutionTests + DataTests pass; property test (400k interleaved
SendOrder/TrySendOrder ops): SendOrder never refused, window count includes cancels and tightens
creates, Total == sum(Counts) throughout, burst cap holds at 255 without wrap; C#-canonical
`.ratelimit`/`.coregroup` JSON parse and round-trip, absent fields keep -1.

---

## Session contracts the risk layer depends on (C# `1eef81a`+`91afdcf` / report 2026-09-22)

No wire or shape changes; the C++ code was already conformant. C# shipped a
reconcile-on-any-quantity-change `OnOrderState` (`1eef81a`) and reverted it the same day
(`91afdcf`): correct, but it hid the sequence violation. Final form is the two-branch
Acked / else-if Done we already have - byte-identical arithmetic verified against `91afdcf` -
plus a comment stating the contract, now mirrored here. Their leak (12 of 1,316 amend-fills-
on-arrival left 10 long / 20 short reserved forever) was a SIMULATOR bug: it matched first
and acked only the remainder. `ServerSimulator.Enqueue` now acks before `Take` trades; C++
has no simulator, nothing to port.

Two contracts recorded for whoever writes the C++ CME session/adapter (no such code in this
repo yet):
- **Acceptance before trade**: deliver states in execution-report order; never coalesce an
  ack into a fill/cancel/elimination, never reorder a fill ahead of the ack of the version it
  references (sequence on tag 2422 `OrderRequestID`); on recovery replay acks before fills;
  if a venue ever coalesces, synthesise the `Acked` in the adapter - do NOT add tolerance to
  `RiskLayer`.
- **In-Flight Mitigation always on**: log on with tag 9768 = 1 and assert it in the logon
  response; `QuantityFilled` == CME `CumQty`, cumulative across every cancel/replace. Our
  `WriteOrderState` keeps `max(stored, reported)` only as a stale-message guard (already so),
  never to bridge a CumQty reset - a non-IFM reset passed through would over-release the Done
  remainder and silently drop fills from the position ledger.

Day-end audit check (both sides): every order's reserve/ack/fill/done ledger nets to zero,
and no fill carries a quantity differing from the last acked quantity for that order.

---

## Order rate limit (C# `b8b6252`, 2026-09-22)

New shared array `<server>/RateLimits`: `CoreGroupIds.Length()` (64) rows of `RollingRateLimit`,
64 bytes, index == CoreGroupId, server-written. `RateLimit` (16 B: Duration int64 nanos @0, Limit
@8, RateLimitId @12) + 32 byte-buckets each `Duration/31` wide, so the 32 span one bucket MORE than
the window and the count only ever over-states - the safe direction for a throttle. `Total` keeps
the bucket sum so a send is one compare; a bucket refuses at 255 (the burst cap). The server writes
the CME default - **3 s / 500, under the reject line on the stricter reading of CME's window** -
into every CoreGroup row at construction: unlike every other limit, zero blocks everything and
unlimited protects nothing, so the default is the exchange's own number and a live session is
protected before anyone configures it. `RiskLayer::ValidateOrder` throttles server-side per
CoreGroup (a CoreGroup maps to an iLink session, the scope CME throttles), cancels included - one
combined window sized at the tighter line can never breach either exchange line - rejecting
`TooManyOrdersPerSecond` (56, already aligned). Plain ref, no seq bump: the CoreGroup thread owns
the row; readers derive Count at their own clock with `GetCount`, which mutates nothing (a
published integer would freeze the moment the algo stops sending). `CoreGroupId` names enum added
to Data (OS 0, Reserved 1, SandP500 2, Equity 3, Forex 4, Crypto 5).

Verified with a 500k-op property test: no true window ever exceeds Limit, the bucketed count never
under-states the exact window, Total == sum of buckets throughout, burst cap refuses at 255.
Erratum for C#: Spec.md's "400 in 3 seconds" prose is stale - `RateLimit.CMEOrderEntry` is 500.
A C++ server creates and owns the region, so a C# GUI's Rate Limits widget reads it directly.
No EnumerateRateLimits on this side yet (its only consumer is the C# widget).

---

## Single-writer server rows (C# `c7d97d3` / report 2026-09-10)

Controls move to the CoreGroup EXECUTION channel: `ControlRiskLimit` (NEW, 20 B, `ControlType::
RiskLimit = 201`) and `ControlAlgoStatus` are handled by `ReadExecution` - the thread that owns
every row they touch - and `ReadAdmin` does allocation only. `OnRiskLimit`/`SaveRiskLimit` are
DELETED: the server never writes `.risklimit` files (the logging server's audit writer appends the
posted row), and `OnControlRiskLimit` sets only the two maxima + Timestamp in place, so an operator
edit can no longer race a reservation or rewind the working quantities. WIRE: `RiskLimit` is 32 B
(`StrategyId` removed - limits are server-wide); `OrderTarget` is 52 B with `TriggerTimestamp` at
offset 32 (converged with our C++-first change; C# defined the semantics: TriggerTimestamp = NIC
arrival of the message the target reacted to, `OrderHeader.NicTimestamp` = send time). Lockstep
deploy for both arrays. The client now tracks its two clocks off every inbound message and stamps
targets accordingly; fill events share ONE NicTimestamp across the state and its fills so the
audit keeps ring order. `Context::AllocateInstrument(clientId, instrumentId)` is idempotent - a
second allocation leaves the live local position row untouched (it used to re-read the file and
force Paused from the admin thread against a row mid-fill). Server `TargetIsActive` is Amend-only:
a Cancel always equals the acked profile, so the no-op check refused every first cancel (69/69 in
the 2026-09-08 live run). The stale "vendor RX thread" comments are corrected: one thread per
CoreGroup runs exchange reads + client reads; the seq bumps are single-writer correctness, not
locks. Note: `ControlType`'s 200/201 sit outside magic_enum's default reflection range - the
`enum_range` specialization in Allocate.hpp is load-bearing for serialization.

Mirrors the convention in the C# repo's `patch_log.md`. The two libraries talk through shared memory
as separate processes, so an entry here that changes a wire struct, an enum value, a region name or a
ring protocol rule needs a matching entry there.

---

## MaturityType is deleted (C# `6d5ef46`, amendment 2026-09-08)

The type letter in symbols broke lexical-order == maturity-order: every `M`-file sorted before any
`Q`-file, so first-match selectors returned an August monthly when a June quarterly existed.
`MaturityDate` alone identifies a contract (verified across all 187,087 catalog files, zero
collisions), so the concept is gone, not worked around.

New formats: future ticker `"ES 2025-12-15"`; spread legs `"+2025-12-15"`, weight magnitude
between sign and date (`"+22026-07-31"` = weight 2). **Leg-token grammar: the date is the
fixed-width LAST 10 chars; digits between sign and date are the magnitude** - a left-to-right scan
eats the year as the weight now that no letter delimits them (shipped in C#, caught). NO legacy
tolerance here (divergence from C#, deliberate): a maturity token is parsed blindly as a date, so
a lettered future token fails loudly in Timestamp::FromString - migrate the catalog, don't limp.
(A lettered SPREAD leg still parses on both sides: the fixed-width grammar drops the letter into
the ignored magnitude zone - structural, identical to C#.) ShortSymbol is now `"ES Dec25"`.

Deleted: the enum, `FutureHeader.MaturityType` (tail byte after MaturityDate - no other offsets
moved; glaze drops the key), `Future::MaturityType()`, `Spread::Long/ShortMaturityType()`.
Round-trips verified: future, calendar, weighted butterfly, legacy-letter input.

**Deploy note:** symbol-named state on the live server (`.position`/`.risklimit`/`.fill` files)
must be renamed at deploy with the C# migration regexes: token `[DWMQY](?=\d{4}-\d{2}-\d{2})` -> ""
in names and contents, and the JSON line `"MaturityType": ...` removed. Collision-check first.

---

## The spread vertical (C# `2e1ddfa` / report 2026-09-08)

A spread is imaginary: risk, positions and P&L live on the outright legs; the spread instrument
keeps its book, its order flow, and a volume-accounting row. Ported from C#, which is live in sim.

**WIRE (lockstep deploy): `Fill` is 64 bytes with `double Price` at offset 40** — CME assigns leg
fills at increments finer than the trading grid, so a fill is a terminal price fact, never ticks.
`OrderProfile` left the struct; consumers use `fill.Quantity`/`fill.Price`/`fill.Sign()`. Old fill
JSON no longer parses — rotate live `Fills/` and audit files at deploy.

**`LeggedHeader` replaces `SpreadHeader`** in the 128-byte overlay: `LegCount` + six
`{InstrumentHeaderId, Weight}` legs; `AsLegged()` (type-guarded, as are `AsFuture`/`AsForex` now).
Spread symbology builds from the legs, signed-weight tokens, root once:
`"Spread XCME ES +M2025-12-15 -M2026-03-15"` — this string names data rings and `.risklimit`
files, so `LeggedSymbology` matches C# byte-for-byte (round-trip verified).

**Per-leg risk**: `ApplyWorstWorkingQuantityDelta(orderId, sideSign, magnitudeDelta)` is the single
home of aggregate arithmetic; every hook is a one-liner through it and the validator commits through
the same code. Validation: max-order per leg in leg units before TryAdd; phase-1 pure per-leg
position check routed by legDelta's own sign (a buy calendar reserves the back leg SHORT); phase-2
commit. `OnFill` releases the raw fill quantity — per-fill releases + the Done remainder telescope
to exactly the reserved worst, per leg.

**`Server::OnFill(OrderState&, span<Fill>)`**: one atomic event — state + spread accounting fill +
one fill per leg, all position rows locked in fills order (server row then local, released in
reverse), risk released only for non-legged instruments (a spread's own fill is volume accounting;
releasing it would double-release the legs). Vendor contract: one ExecutionReport = one call;
fills[0] = the order's own instrument; leg fills copy the OrderId and rewrite only InstrumentId.

**Allocation unions**: the server allocates a spread's legs first (full client path, no admin echo —
the GetInstrument handshake is one-request-one-reply); the client onboards legs recursively through
GetInstrument (exactly-once via the data-ring early-return); `Context::CreateInstrument`
materializes legs BEFORE its non-reentrant spinlock (self-deadlock, shipped and caught in C#).
`Spread` is not a `Future` any more — no multiplier, no maturity of its own; ±1 calendar weights
only, anything wider throws at construction.

---

## `OrderRisk::MaxOrderQuantity` is the limit, not the bound

C# renamed and re-based this constant after the first port: `MaxOrderQuantity = 55`, the largest
quantity a single order may carry, tested with `> MaxOrderQuantity`. C++ had it as `56`, an exclusive
array bound tested with `>=`.

Both accept exactly 1..55, so nothing was ever misjudged — but the constant is public on both sides
and reads as a limit, so anyone using `OrderRisk::MaxOrderQuantity` as the answer to "how large may an
order be" got 56 in C++ and 55 in C#. The counts array now sizes itself `MaxOrderQuantity + 1` to keep
index 55 addressable and the struct on one cache line; `sizeof` stays 64 and the offsets are unchanged,
so this is not a wire change. `Remove` is private, as in C#.

---

## `3fa223d` — Adopt the C# risk model

The C# side is ahead on risk and its structs are the reference. Six wire-visible changes taking C++
to byte-for-byte agreement, all confirmed by compiling and dumping offsets rather than by eye.

**`RiskLimit` 52 → 36 bytes.** Drops `MaxOrdersPerSession`/`MaxOrdersPerSecond` — rate limits are
enforced nowhere in either language — and gains `WorstLongWorkingQuantity`/`WorstShortWorkingQuantity`,
the reserved exposure held per instrument, signed so long is positive and short negative. Both sides
already agreed through offset 28; they now agree at every offset.

The glaze meta previously omitted `Timestamp` and `StrategyId`, so neither survived the
`<symbol>.risklimit` round-trip: a strategy-scoped limit silently became server-wide on the next
restart. All eight fields are in the meta now.

**`OrderStateReason`.** Why a state is being published; `Filled` onwards is terminal. It occupies
byte 50 of `OrderState`, which was the first reserved byte, so the struct stays 60 bytes — the kind
of drift a `sizeof` assert can never catch.

**`OrderRejectedReason` renumbered from 45 up** to match C#: `TooManyActiveTargets` enters at 45,
`QuantityExceedsRiskLimit`/`PositionExceedsRiskLimit` split the old `QuantityTooLarge`/
`PositionTooLarge`, `ExceptionThrownByRiskLayer` moves to 63. These are bit positions in a
`Bitset64`, so before this a rejection crossing the language boundary decoded as a *different reason*.

**`OrderRisk` (64 B), new.** Counts how many unacked targets are live at each absolute quantity, so a
slot's worst case is the highest live quantity rather than the last one sent — a pipelined amend
10 → 3 → 7 keeps reserving 10 until the 10 retires. `Context` gains `<serverName>/OrderRisks`,
server-owned, keyed by `OrderId::GlobalIndex`.

**`RiskLayer` reserves on send, releases only on an exchange-originated event.** `OnOrderState`
retires an ack and releases on `Done` rather than on a reason match, so a cancel carrying a label the
switch doesn't know cannot leak its reservation permanently. `OnFill` converts reservation into
position. `OnOrderRejected` retires exactly the target the exchange named. `Server` calls all three at
the same points C# does.

Two fixes carried across that were not asked for but are load-bearing:

- `ValidateOrder` returns before taking any mutable ref when the source is not `Server`. A client maps
  those arrays read-only, so without it every client order comes back `ExceptionThrownByRiskLayer` —
  the first open item in the C# patch log, which this port would otherwise have inherited.
- `OnRiskLimit` preserves the live working quantities instead of the sender's copy. The GUI
  read-modify-writes the whole struct, so an operator edit would rewind the ledger to whenever the
  dialog opened.

**Known divergence.** C# `RiskLimit.GetMaxLimits`/`GetMinLimits` stamp `Clock.Now`; C++ `Clock` lives
in `Provider` and `RiskLimit` in `Execution`, so the factories cannot reach it. `Timestamp` is left at
`MinValue` rather than reaching for `Timestamp::UtcNow()`, which would reintroduce the
wall-clock-in-a-backtest bug the C# patch log records fixing. Moving `Clock` to `Tools`, as C# has it,
would close this properly.

---

## `a7b2717` — Match the C# wire structs, and key instruments by exchange id

`Order.hpp` did not compile: `TimeInForce` was declared twice, once matching the C# values (`Day = 0`)
and once with the old numbering.

Three structs had the right size but the wrong field order, which no `sizeof` assertion can catch.
`Fill` had `FillType` and its padding ahead of `FillId` and `OrderProfile`, so C# would have read the
fill id out of the padding. `OrderState` and `OrderTarget` were missing `TimeInForce` entirely and
spent the byte on reserved padding instead.

`OrderTargetAction`/`OrderTargetStatus` had no default initializers, so a default-constructed
`OrderTarget` left them indeterminate.

Separately, `AllocateInstrument` carries `ExchangeInstrumentId` and `LoadInstruments` resolves the
header id through it. A header id is only an instrument's index in the definition file the catalog was
built from, so it moves whenever that file is republished and can name a different contract on the
next run — the venue's id stays with the contract for its life. An instrument no longer listed is
reported and skipped rather than restored onto whatever now occupies its old slot.

---

## `29cae32` · `9495980` · `4d756e2` · `8b41693`

- **`ServerContext` owns `ClientsDirectoryPath`/`InstrumentsDirectoryPath`.** Both were rebuilt inline
  on every call and neither was ever created. `SaveClient`/`SaveInstrument` open their stream in append
  mode, which fails silently when the parent directory is missing, so the first save wrote nothing and
  `LoadClients` then found no file. Also replaced the exchange-id lookup map with a scan of the
  instrument headers — the map was a second copy of shared-memory state and could drift from it.
- **Top instrument id reserved as `NoInstrumentId`.** An id carrying no instrument had to borrow a real
  value, and 0 is a real instrument. Reserving the top of the *field* rather than the top of today's
  allocation keeps the sentinel meaningful as the allocated space grows.
- **Risk limits accepted over the admin channel** — write to shared memory, forward to the owning
  strategy, append to the per-symbol `.risklimit` file that `ServerContext::AllocateInstrument` reads
  back at startup.
- **`GetCoresSharingLastLevelCache`** reads the kernel cache topology so a pipeline's threads can share
  one complex; **`StringN` gains `<=>`**.

---

## `6cfc873` — Path-join the remaining shared-memory region names

Three sites named the `ServerHeader` letterbox and two still concatenated, so `ServerContext::Connect`
published into one region while `Context::_serverHeaderBox` read a different, empty one.
`EnsureConnected()` then spun forever on a box nobody writes, hanging both the `Server` and `Client`
constructors. `Client::_serverMarketsByPrice` had the same mismatch against `Context::_marketsByPrice`.

Mismatched names never error — `CreateOrOpen` just makes the second region — so the symptom is a hang
or an empty read a long way from the cause. Grep every region-name construction site at once.

---

## `35dbd24` — Persist client sockets across client restarts

A client socket was disposed a second after its process died, and `ServerSocket::Write` dropped
anything addressed to a non-`Open` client. So a fill arriving before the client reconnected was lost
from the client *and* from its audit, since the logging server taps the same region. That is exactly
the iLink3 retransmit window.

Client sockets now outlive their client process: a dropped client goes `Detached` rather than
`Closed → Disposed`, the socket stays mapped and writable, and reconnect reuses it. Gated by
`ServerHeader::Persistance` (wire field, default true).

**Nothing clears shared memory any more.** `Reset()` cannot work as a synchronisation mechanism: it
clears the region and *one* side's cursors, but the peer's live in another process and are
unreachable. Both sides recover instead — `Protocol::SkipRing` walks the ring to the writer's head, so
a writer resumes the existing sequence space rather than restarting at 0 (which reads as stale
forever to anyone parked higher) and a reader parks at the head rather than replaying a backlog it
must not re-apply.

The reader skipping the backlog is deliberate: `Fill` is an event and re-delivering it double-counts,
whereas the client's authoritative state is the `OrderState`/`PositionHeader` arrays, which the server
updates regardless of client liveness. The buffering exists so the **audit tap** sees the fills.

`Protocol::ReadRingSeq` factors out the wrap/magic redirect so the read, skip and probe paths resolve a
cursor identically; `GetReadStatus` checks magic to match. A probe that disagreed with the read would
spin or strand a channel permanently, since neither side advances to correct it.

**Contract for strategies:** treat `Position::Header()` as the truth for position and P&L, never an
accumulator fed by `Fill` callbacks. Fills landing while a strategy is down are real, are in the audit,
are already in the header, and will never arrive as callbacks.

---

## Open / known incomplete

- **Nothing here has been executed.** `Provider/Main.cpp` has no run loop — it calls `Connect()` and
  returns — so neither the fresh-start nor the reload path has run. Everything is verified by reading
  and by compiled offset dumps.
- **`DefaultServerHeader.ServerName` is `"ServerName"`**, which `ServerContext::ThrowIfInvalidServerName`
  rejects; it needs the full `/mnt/S/Servers/<Mode>/<name>` path.
- **No geometry check on socket re-attach.** A client reconnecting with different channel lengths (e.g.
  `CoreGroupIds` changed between runs) leaves the existing views pointing at wrong offsets.
- **`Persistance = false` is untested** and `OpenClient` still calls `Reset()` on that path, clearing
  the region after the client already recovered its cursors — leaving the client deaf. Deliberate.
- **A server restart while a client is still running fails at construction**: `ServerContext::Connect`
  throws when the `ServerHeader` letterbox is non-empty, and that region survives while any client maps
  it. Pre-existing, but it is the scenario persistence is aimed at.
- **`Clock` lives in `Provider`, not `Tools`**, so `Execution` cannot stamp simulation time — see the
  known divergence under `3fa223d`.
- **`csharp-handoff.md` is written for the opposite direction** (C++ → C#) and predates the risk-model
  port. Treat this log as current where the two disagree.
