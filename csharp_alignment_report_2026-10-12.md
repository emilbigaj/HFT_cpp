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
