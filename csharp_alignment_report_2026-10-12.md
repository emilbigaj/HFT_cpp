# C# alignment report — 2026-10-12: reply to `cpp_alignment_report_2026-10-12.md`

For the engineer or agent maintaining the C# side. C++ (`persist-client-sockets`) has ported
`cpp_alignment_report_2026-10-12.md` (C# `d9fb359`). This reply answers its §12 questions and lists
the C++ forms that differ in shape (not behaviour). Nothing here asks C# to change code, except
possibly §2.

## 1. Ported

P1–P12, P14–P20 and P22 are ported. P13 and P21 needed nothing (below). R1–R8: none of them was ever
ported in C++, so nothing to undo.

| Item | C++ form |
|---|---|
| P1 | `Socket::ConnectRefusal` (int32, -1..-7), `Socket::ConnectRefusedException` (a `std::runtime_error` with a `ConnectRefusal` member), `Socket::IsClientReady` (`std::function<bool(int32_t)>`) |
| P2–P5 | `ServerSocket::PollLetterBox` / `DecideConnect` / `ReplyToClient`, the never-dying listen loop, `ServerSocket::IsClientReady` |
| P6–P8 | `Server::TryCloseOutOrders` (replaces `CancelAllOrders`), `IsClientReady`, `IsClientDrained`, the drain counters (`alignas(64)` atomics), injected cancels written to the client's channel before `OnOrderTarget`; the client ignores `OrderType::OrderTarget` |
| P9 | `ClientSocket::Connect` returns the id or throws `ConnectRefusedException`, retries `Closing` every 100 ms, matches name + timestamp + pid, and a refusal disposes (never closes). `Application::AddExitAction` now returns a cancellable `ExitAction` (as C#), and `ClientSocket::Dispose` cancels its "close at exit" action |
| P10 | `ServerContext::AllocateClientId` returns `ReservedClientName` / `NoClientIdAvailable`; `TryGetClientId` added on `Context`; `Server::LoadClients` throws on a refusal |
| P11 | The client constructor keeps only `Recover`; the previous-orders check, the 100 ms sleep and the `OrderRisk` clear are gone; `RiskLayer::ValidateOrder` still resets the row on Create |
| P12 | `Client::Dispose` no longer refuses with all 64 slots active. C++ builds the client's members in its initialiser list, so the C# `try { ... } catch { Dispose(); throw; }` is a guard member declared right after `ClientId`: a throw later in construction destroys it first, and it closes the socket |
| P14 | `Protocol::Stamp`; `SharedArray` stamps every row when opened write-capable. C++ never had an alias array over the server-header letterbox (the letterbox holds its own entry), so nothing to delete |
| P15 | `ReadOnlySocket` keeps no closed state; only writers close |
| P16–P19 | `OrderStateReason::InFlight`; `OrderState::NewInFlight`, `OrderTarget::NewOrderRejected`, `OrderState::NewRejected` (out parameters, as C#); `OrderDiscardedMask = 0x01007F0000000000`; `AheadOfOrder::QuantityAhead` |
| P20 | See §3 |
| P22 | `ServerHeader::ServerProcessId` (int32 @173, 177 bytes, asserted); `ServerContext::Connect` stamps `getpid()`, and a refused post reads the posted pid and throws the "already running" / "previous server has exited" message, never overwriting. Verified against the real `Connect` (scenario 18's two refusals, the dead header kept) |

Verified in C++: build clean; ExecutionTests, DataTests, ToolsTests pass; the C#/C++ JSON harness is
still byte-identical (0 of 140 files differ; `InFlight` and `QuantityAhead` included). Scenario checks
against the real C++ `ServerSocket`/`ClientSocket`/`SharedArray`: §11 scenario 1 (a refused second
connect under a live name leaves that session `Open` and delivering), `NotReady` via the hook, an
allocator refusal reaching the client as its reason, scenario 15 (reader reads on after a close
message) and scenario 14 (stamp). `TryCloseOutOrders` and the drain (scenarios 3, 8, 9, 10) were not
run end to end in C++: they need a full trading server with a live server directory.

## 2. §12 confirmations

- **`ServerHeader.Timestamp` is NOT rewritten every pass by the C++ server.** It is written only when
  market data arrives (`Server::OnMarketByPrice`: `ServerHeader.Timestamp = marketByPrice.TickHeader.NicTimestamp`).
  On a quiet market a remote desk's clock, which C# takes from the copied header, stands still. If the
  remote desk needs a moving clock, decide with the user whether the C++ server should stamp it every
  pass; nothing is changed in C++ yet. (Separately, that header is also written from more than one
  thread: the known-open multi-writer `ServerHeader` item.)
- **A fill's state and position rows are written before the client gets the state message: yes.**
  `Server::OnFill` writes the order row and both position rows under their locks, then writes the
  state, fills and positions to the client's ring.
- **`OrderDiscarded` has all eight reasons: yes**, now as the constant mask `0x01007F0000000000`.
- **Every row the C++ server writes is framed with the same `Magic` and `Length = sizeof(T)`: yes**
  (`Protocol::Write`), and C++ now stamps lock-edited rows itself.
- **No C++ region is named `<server>/Latency`: confirmed.**

## 3. P20 in C++

A C++ client process has no `ServerContext` (it opens only its `ClientContext`), so "set only by the
first `ServerContext`" would leave a C++ client without a leg lookup and its spreads would throw.
C++ keeps "the first context of the process installs it, nobody rebinds it", which the report's
PORT-IF rule already covers for processes that keep their contexts for life (both C++ processes do).
The other half is ported: the context that installed the hook clears it when destroyed, so it can
never call into a dead context.

## 4. Nothing needed

- **P13:** the C++ client never wipes its audit folder, and C++ has no manual client.
- **P21:** C++ sanitises region names with the same rule (`[A-Za-z0-9_.-]`, else `_`) and does not
  otherwise validate client names.

## 5. 2026-10-13: P23, P24, C15–C17 and the extended P22 note

Ported against C# `194f8cf`. Nothing here asks C# to change code.

| Item | C++ form |
|---|---|
| P23 | `SideByPrice64::TrySetQuantity`: the new "better and zero: no-op" check before the far-worse check; the far-better branch's own zero check is deleted. Scenario 19 and a 200-run server/replica fuzz pass; the same fuzz diverges without the fix |
| P24 | Ported in full, reversing the 2026-10-05 item 8 ("C#-only"). `InstrumentHeader::ProductGroupId` (int8 @7) and `ProductGroupRoot` (String4 @60), both in the JSON at C#'s positions; `OrderType::MessageEfficiency = 18`; `MessageEfficiencyTier` and the 84-byte `MessageEfficiency` row in `Execution/RateLimit.hpp` (asserted); `ServerContext::AllocateProductGroupId` (private, called from `AllocateInstrument`; `OnInstrumentHeader` sets -1); `Server::EndMessageEfficiencyDay` on `Closed`; `Server::PostMessageEfficiencies` in `Dispose` before the audit link closes, Realtime only. The array is mapped writable by the server and `<server>/MessageEfficiency` is created with the other folders |
| C15, C16, C17 | Nothing to port. The C++ server keeps its alert link named `<server>.alert` (`AlertManager`: `DirectoryPath + ".alert"`), so the Local Mirror's `<server>/LocalMirror.alert` does not collide |
| P22 (extended) | The Local Mirror start belongs in CME's `main`, after `LoadClients`/`LoadInstruments`. CME does not start it today; that is the user's call, not an HFT change |

**C++ forms that differ in shape, not behaviour:**

- .NET's `DateTime` and `TimeSpan` have no C++ equivalent, so `Tools/DateTime.hpp` adds 8-byte `Tools::DateTime`
  (ticks plus the 2 Kind bits, compared on ticks) and `Tools::TimeSpan` (ticks). They write and read the same
  text as System.Text.Json and the C# `TimeSpanJsonConverter`. A `DateTime` with an offset suffix (Kind Local)
  is refused on read; C# writes none to the files C++ reads.
- `Tools/Session.hpp` ports only `Session.CME.ConvertToLocal(Timestamp)` (America/Chicago through the C++
  tz database).
- **Decided by the user (2026-10-13): only C# uses message efficiency.** C++ reads and writes the row (the server claims,
  loads, resets and posts it), but never counts. This answers the report's "A C++ strategy, if there is one, does the
  same": it does not. `CanSend`/`Send`/`TrySend`, `CMEGetWeightedMessage` and `SessionManager` are not ported, and the
  C++ `Client` calls neither `Send` nor `OnFill` on the row. Nothing for C# to change.
- `RateLimit.hpp` includes `Order.hpp` for `OrderType` (C++ needs the declaration first); `Order.hpp` no
  longer includes `RateLimit.hpp`.

**Verified:** the JSON harness shows 0 of 159 files differing. The new samples are the row (a fractional `TradeDate`), max/min limits,
CME local time in summer (CDT) and winter (CST), a UTC `DateTime` ("Z"), `Reset` on a fresh row (true, then false,
then true a day later), and loading a pre-rename line (`ProductGroup` key, no `Header`) and a fractional date. Against the
real `ServerContext`: MNQ and NQ share row 0; M2K (RY) and ES claim rows 1 and 2; ES loads its tiers from a pre-rename
file while root, id and day are the server's; an empty `ProductGroupRoot` throws the C# message. The `Closed` handler and
the shutdown post were not run end to end: they need a full server with Logging.

**Deploy (L10):** the C++ server needs `ProductGroupRoot` on every header it allocates, so CME must set it from
tag 1151 when it builds headers (`SecDef/SecDefFile.hpp` `FillCommon`; it already parses the tag). The C++ server, CME
and the C# processes on the box deploy together.

## 6. 2026-10-15: P25, P26, P27

Ported against C# `8b0f60f`. Nothing here asks C# to change code.

| Item | C++ form |
|---|---|
| P25 | `Context::IsAlgoLive` and `GetServerPositionHeader`; `ServerPositionHeaders` is mapped by every context (it was `ServerContext`-only); `RiskLayer::ValidateOrder` checks both switches; `Server::OnControlAlgoStatus` turns the server row for strategy 0; `Server::WriteToRecord`; the master is set `Live` in `ServerContext::AllocateInstrument` |
| P26 | `RiskLimit::MaxFillsPerSession` (28 bytes), `WorkingRisk::WorstLongQuantity`/`WorstShortQuantity`/`FillsThisSession` (20 bytes), `ControlRiskLimit::MaxFillsPerSession`, all asserted and in the JSON; reason 57 `TooManyFillsPerSession`; `RiskLayer::OnFill` counts, `OnSessionClosed` zeroes (server and client, on `Closed`); fills room in `ValidateOrder` after the position room and in `TryClipToRiskLimit`. The C++ client's simulation-only discard of 57 is deleted, as in C# |
| P27 | Regions `ServerRiskLimits` (renamed) and `LocalRiskLimits` [client, instrument]; `ControlRiskLimit::StrategyId` @8 (28 bytes); `Context::GetRiskLimit(instrumentId)` is virtual (firm's on the server, the strategy's on a client); `GetRiskLimit(clientId, instrumentId)`, `GetServerRiskLimit`, `RiskLimit::FromFile`; the strategy's row is loaded in `ServerContext::AllocateInstrument(clientId, ...)`; the client ignores a `RiskLimit` (16) on its execution channel |

**Not ported, no C++ caller:** `Position::IsAlgoLive` (C++ has no algo layer; a C++ algo would call `Context::IsAlgoLive`),
and the C17 GUI helpers (`OrderState::NewCancel`, `Instrument::TicksToPriceString`/`PriceFormat`, `Context::DirectoryName`,
`TryReadMarketByPrice64`). C++ already spells `TickDecimals` correctly.

**Verified:** the JSON harness shows 0 of 159 files differing; the samples now carry `MaxFillsPerSession`, the renamed `WorkingRisk` fields with
`FillsThisSession`, `ControlRiskLimit.StrategyId`, and reason 57 in an `OrderRejected` (its name and `IsDiscarded` false
match). Against the real `ServerContext` and `RiskLayer`: a firm line without the field loads 0; the server's
`GetRiskLimit` is the firm's row; a strategy's row loads from `<strategy>/RiskLimits`; the master starts `Live` over a
`Paused` position file; master `Paused` makes `IsAlgoLive` false and leaves the strategy's row `Live`; with limit 10 and
4/4 working, a buy of 3 fails the fills room and passes the position room, and a buy of 2 passes; fills of 4 and -2 count 6, and
`OnSessionClosed` zeroes the count, not the position. `Server::OnControlAlgoStatus`/`OnControlRiskLimit` were not run end
to end. CME builds against it unchanged.

## 7. 2026-10-16: P26 updated (a manual order is never limited by fills)

Ported against C# `edf71e4`. Nothing here asks C# to change code.

- `WorkingRisk`, 24 bytes (asserted, in the JSON): `WorstWorkingLongQuantity` @8, `WorstWorkingShortQuantity` @12, `FillsThisSession` @16,
  `WorstWorkingFills` @20.
- `RiskLayer::ApplyWorstQuantityDelta` adds an algo order's delta to `WorstWorkingFills`; `OnFill` counts algo fills only; the
  fills room is `MaxFillsPerSession - FillsThisSession - WorstWorkingFills`; `ValidateOrder` checks it for algo orders only.
- **Verified:** the JSON harness shows 0 of 159 files differing. Against the real `ServerContext`/`RiskLayer`: the report's scenario (limit 10, 4/4
  working: a buy of 3 refused on fills and 2 accepted) still holds with `WorstWorkingFills` 8; a manual fill moves the position and
  leaves `FillsThisSession` unchanged; with the algo's fills at the limit it has no room, and a manual resting buy of 50 (which
  reserves only `WorstWorkingLongQuantity`) leaves the algo's fills room as it was. CME builds against it unchanged.

## 8. 2026-10-16: P28 (strategy exposure rows under the server's name)

Ported against C# `d4f6402`. Nothing here asks C# to change code.

- Regions `<server>/ServerWorkingRisks` (per instrument, the server's) and `<server>/LocalWorkingRisks` (per strategy and
  instrument, mapped writable by the server and by the client) replace the per-directory `WorkingRisks`.
  `Context::GetWorkingRisk(instrumentId)` is virtual: `ServerWorkingRisks[instrumentId]` on the server (range-checked),
  `LocalWorkingRisks[clientId, instrumentId]` on a client.
- `ServerContext::AllocateInstrument(clientId, instrumentId)` seeds `LocalWorkingRisks[clientId, instrumentId]` with the strategy's
  position; the C++ client already reseeds its row at start, as C#'s does.
- `GetServerRiskLimit` and `GetServerPositionHeader` are deleted, as in C#; the server calls its own `GetRiskLimit` and
  `GetPositionHeader`.
- Not ported, C#-only: `Application.DetachConsole`, the Publish packages, `WorkspaceConfig`.
- **Verified:** against the real `ServerContext`, the strategy's `LocalWorkingRisks` row is written at allocation and `ServerWorkingRisks`
  holds the firm's row; the rest of the P25–P27 test still passes; JSON harness shows 0 of 159 files differing; CME builds unchanged.

## 9. 2026-10-16: P29, C18

Ported against C# `a80baf2`. Nothing here asks C# to change code.

- **P29: ported.** The C++ `RiskLayer` keeps `std::vector<OrderRisk>` (`OrdersCapacity` rows, by `OrderId::GlobalIndex`, zero at
  start) behind a private `GetOrderRisk`. The `<directory>/OrderRisks` region and `Context::GetOrderRisk` are deleted. Logic unchanged.
- **C18: nothing to port.** The C++ client keeps its own book replica in `<client>/MarketsByPrice`; `<server>/MarketsByPrice` is
  unchanged.
- **Verified:** the build is clean; the JSON harness shows 0 of 159 files differing; the P24–P28 `ServerContext`/`RiskLayer` tests still pass; CME builds unchanged.

## 10. 2026-10-19: P30, C19, C20

Ported against C# `642cc0c`. Nothing here asks C# to change code.

- **P30: ported.** `SocketHeader::ServerName`/`ClientName`, `ServerHeader::ServerName` and `AllocateClient::ClientName` are
  `Tools::String256`. Asserted: `sizeof(SocketHeader) == 600` (`ClientName` @256, `Timestamp` @512, the rest +256), and
  `sizeof(ServerHeader) == 305` (`Persistance` @300, `ServerProcessId` @301). `Context::TryGetClientId` takes a `String256`. A name
  over 256 bytes throws where it is built. Deploys at a week start with fresh shared memory, with the C# Local Mirror and the desks.
- **C19: checked.** The C++ `Server::OnControlAlgoStatus` turns the master row when `StrategyId` is 0 (P25).
- **C20, and `Tick.SizeOf`/`TickType.IsTick`:** nothing to port. C++ has no `Tick` struct, tick history, GUI or simulator.
- **CME:** `Server/CmeServerConfig.hpp:246` builds the server name as a `String128` and fails to compile until it is a `String256`;
  that one line is CME's to change.
- **Verified:** the JSON harness shows 0 of 159 files differing, with a ~220-byte client name; against the real sockets, a 200+ byte name connects
  and stays open, and a 300-byte name is refused.

## 11. 2026-10-19: P31, and the client audit-folder note

Ported against C# `5497ba0`. Nothing here asks C# to change code.

- **P31: ported.** `Server::ReadExecution`'s `ControlType::AlgoStatus` case calls `CancelAlgoOrders(strategyId, instrumentId)`
  after `OnControlAlgoStatus` when the status is `Paused`. It walks the instrument's orders (every strategy's for strategy 0, that strategy's
  otherwise) and cancels only the algo ones, using `OrderState::NewCancel()` with
  `Seq` = target seq + `OrderTarget::s_foreignCancelSeqJump` (1,000,000) and `NicTimestamp` = now. It sends each cancel through
  `SendServerOrderTarget` (written to the owner's execution channel, then `OnOrderTarget`), which the injected-cancel drain now
  uses too. `TryCloseOutOrders` uses the named constant. A reject's pause still calls `OnControlAlgoStatus` alone. The walk is
  the new `Context::EnumerateOrders(instrumentId, strategyId, inFlightClientId, onOrder)`, with the same rules as C#'s
  `OrderEnumerator` (callback form; no allocation).
- **Client audit folder:** nothing to drop. The C++ client never creates `<name>/Audit` before `Connect`; its `ClientContext`
  creates its folders after the server accepts it.
- **Verified:** against the real `ServerContext` with eight hand-written orders, the selection is exactly as C#'s verify describes: a master Pause
  takes every strategy's working algo orders, a strategy's Pause takes only that strategy's, and Done, unread, other-instrument
  and manual orders (a GUI's booked to a strategy, the house book's) are never taken; each cancel carries seq + 1,000,000.
  The full server path was not run end to end. JSON harness shows 0 of 159 files differing; CME builds.

## 12. 2026-10-19: P32, C21

Ported against C# `91d7805`. Nothing here asks C# to change code.

- **P32: the row is ported.** `MessageEfficiency::TierEMT` (was `Tier0`, offset 20), `EMTWeightedMessages` @84, 92 bytes (asserted).
  `Reset` zeroes `EMTWeightedMessages`. `EMTEfficiency()` is written second in the JSON, after `Efficiency`, and never read.
  `GetMinLimits` gives `TierEMT` 1,000,000 / 500 and leaves the other tiers 0.
- **Counting is not ported:** `CanSend`, `Send`, `GetMEPTier`, the tier helpers (`IsApplied`, `IsWithin`, `GetWarningRatio`,
  `GetMEPWarningRatio`) and the weighter. The user decided on 2026-10-13 that only C# uses message efficiency; C++ only
  reads and writes the row (the server claims, loads, resets and posts it). The report's "call the weights directly in C++ too"
  therefore has no C++ call site.
- **C21:** nothing to port (C# display only).
- **Verified:** the JSON harness shows 0 of 161 files differing, against `91d7805`. It covers the new `GetMinLimits`, an old `"Tier0"` line (it loads `TierEMT` 0
  on both sides), and the report's new-form line (`EMTEfficiency` written equal). The P24 `ServerContext` test passes on the 92-byte row; CME builds.

## 13. 2026-10-22: P33–P36, C22–C25

Ported against C# `d7f8ea6`. Nothing here asks C# to change code.

- **P33: ported.** `Data::InstrumentHeader` is 128 bytes (`TradingStatusTimestamp` @64, 56 reserved), `InstrumentHeader256` (renamed
  as in C#) is 256, `LeggedHeader` 192, and type-specific fields start at 128; all asserted, JSON identical. `Server::OnTradingStatusUpdate`
  stores the time, then the status (`std::atomic_ref` release store), only on a change. `NotInSessionInFlight = 47`, discard mask
  `0x0100FF0000000000`. `RiskLayer::GetNotInSessionReason`, `NotInSessionInFlightWindow` = 1 s. `Server::OnOrderRejected`
  rewrites per bit (`Bitset64::Replace`). C++ readers take references into the row, never a copy they point into.
- **P34: ported.** The per-update `ServerHeader.Timestamp` store is deleted. The C++ side had found the same line in a
  cache-contention review the day before.
- **P35: ported.** `RiskLayer::MaySend`; the server's create check uses it.
- **P36: ported.** `ServerSocket::PollLetterBox` peeks, decides, then empties the letterbox before the refusal reply or `OpenClient`.
- **C23 check (CME):** iLink order reports carry `TransactTime` in `OrderHeader.ExchangeTimestamp`, and MDP incremental book ticks carry the
  event's `TransactTime`. Two recovery paths, a snapshot-recovered book and the removals-only reset, carry the packet's `SendingTime`; a
  quote after a recovery may hold until the next incremental.
- **CME:** the `NotInSession` mapping is already in place. CME still has to rename `InstrumentHeader128` to `InstrumentHeader256` before it builds.
- **Verified:** the JSON harness shows 0 of 161 files differing. Against the real `ServerContext`/`RiskLayer`: the window (47 at 0.5 s and at exactly 1 s, 50 at 1.5 s),
  the session rule (a create in Auction passes, Halted/Unknown refuse it, the full `MaySend` table), and the rewrite of
  `OrderNotFound|NotInSession` to `StateIsDone|NotInSessionInFlight`, discarded. The real-socket handshake still passes with the peek.
