# C# alignment report — 2026-10-05: items agreed in the C++/C# review

This handoff is for the engineer or agent aligning C# with C++. Every item below was reviewed and
agreed with the user one at a time. **C++ is the reference for the items marked "C# to implement"**:
its behaviour is correct and C# must implement it so the two libraries behave identically. The
report is self-contained. Line numbers were checked on 2026-10-05 against C++
`persist-client-sockets` and C# `8a8bffc`.

No wire or shared-memory shape changes in any item. Behaviour only.

| # | Item | Status |
|---|---|---|
| 1 | Client-side position check vs the server-wide limit | No change: documented rule, one strategy per instrument |
| 2 | Client startup: refuse on any Active order, then skip the backlog | **C# to implement** (§2) |
| 3 | `ProcessId.IsAlive`: a pid of 0 or below is dead | Done in C# (`8a8bffc`) |
| 4 | Refused target: PendingNew always published; refused Create's Done copies the row | Done: C++ aligned to the C# the user supplied |
| 5 | Restart replay must restore client allocations | **C# to implement** (§1) |
| 6 | `mlockall` failure must stop the process | **C# to implement** (§3) |
| 7 | SIGHUP: shut down cleanly, unless started under `nohup` | **C# to implement** (§4) |
| 8 | `MessageEfficiency` (CME messaging-efficiency counting) | C#-only for now: not ported to C++; C++ strategies are not counted |
| 9 | `RiskLayer.TryClipToRiskLimit` | Done: ported verbatim to C++ (`RiskLayer.hpp`), unused until a C++ Algo layer exists |
| 10 | Validate the server name before `InitDirectories` deletes anything | **C# to implement** (§5); C# can wipe a live server's files today |
| 11 | A fixed string (`StringN` / `FixedString.Set`) that does not fit throws | Done: both sides throw; C++'s `const char*` overload no longer cuts silently |
| 12 | JSON reader accepts `//`, `/* */` comments and trailing commas | Done: C++ aligned to C# (`Tools/Json.hpp`) |
| 13 | `Bitset64` in JSON is a bare number | Done: C++ aligned to C# (`Tools/Bitset.hpp`); was `{"Raw":n}` |
| 14 | `Timestamp.FromString` falls back to `yyyy-MM-dd HH:mm:ss`, `yyyy-MM-dd HH:mm`, `yyyy-MM-dd` | Done: C++ aligned to C#. Known residual kept by choice: C++ accepts trailing characters after the caller's own format, C# does not |
| 15 | A `SharedArray` whose region would exceed `int.MaxValue` bytes is refused loudly | **C# to implement** (§6); C++ throws already |
| 16 | Timestamps come from the sim-aware clock (`Clock.Now` / C++ `Clock::UtcNow()`) | Done: C++ uses it at every site C# uses `Clock.Now`; the wall clock remains only in socket/logger plumbing, at the same sites as C# |
| 17 | Server constructor: `InitDirectories` before `ServerContext.Connect` publishes the header | Done: C++ aligned to C# |
| 18 | Client market-by-price: Delta/Update/Snapshot branches, stale ticks dropped, per-delta `Instrument.MarketByPriceDelta(delta, bytes)` event | Done: C++ aligned to C# (the C++-only `Client::MarketByPrice` callback is replaced by the instrument event) |
| 19 | Client: an unknown instrument-data tick type throws | Done: C++ aligned to C# (was silently ignored) |
| 20 | Allocation sets `AlgoStatus`: Live in simulation, forced Paused in realtime | Done: C++ aligned to C# |
| 21 | Simulation-only discard of a lone `TooManyOrdersPerSession` (57), in `Client.IsDiscarded` used by client and server | Done: C++ aligned to C# (C++ wrongly named 56, already discarded) |
| 22 | Every context validates the server name before any shared memory is opened | Done: C++ aligned to C# (the check is now the first initialiser of the base `Context`, for servers and clients) |
| 23 | Instrument range check is per context: server against `ServerHeader.InstrumentIds`, client against its own `InstrumentIdsByClientId` | Done: C++ aligned to C# (C++ uses a `_instrumentIds` pointer set per context instead of a virtual) |
| 24 | `RateLimit`, `RollingRateLimit`, `SessionRateLimit` each exist once, in `Execution/RateLimit` | Done: C++ aligned to C# (stale duplicates deleted; the live types moved from `Order.hpp` into `RateLimit.hpp`, C# order) |
| 25 | JSON output: C++ must print byte-for-byte the same JSON as C# for every serialised type | Done: C++ aligned; a C#/C++ harness (140 files, every type both sides serialise, pretty and line) diffs byte-identical. Known gaps: `ForexHeader` (C# cannot serialise it, its `Symbology` getter throws); C++ `Profit` has no JSON form |
| 26 | Default `RiskLimit` (Max/Min) stamped with the sim-aware clock inside the factory; `Clock` lives in `Tools`, ported in full from `Tools/Clock.cs` | Done in C++ (`Tools/Clock.hpp`, `GetMaxLimits(instrumentId)`/`GetMinLimits(instrumentId)` as C#). Two C# `Clock` bugs found by the port: **C# to implement** (§7) |
| 27 | `Settlement` tick and its constructor, `Quote.MicroPrice`, `TryGetQuote` returning the cached quote first | Done: C++ aligned. Accepted gap: C++ `TickHeader` has no 5-argument constructor (it stays an aggregate so field-name initialisation keeps working, including in the CME book builder); the `Settlement` constructor fills it field by field with the same values |

---

# 1. Restart replay must restore client allocations

## 1.1 The rule

When a server restarts mid-week, `LoadInstruments` replays every line of the week's
`<server>/Instruments/<date>.allocateinstrument` file. **Each line must be replayed through the client
overload of `OnAllocateInstrument`**, using the client id recorded in that line, so the restart
restores everything a live allocation did. Replaying only the server-side half is wrong.

## 1.2 What C# does today

`Provider/Server.cs:788-807`, unchanged since the initial commit (`ef0f733c`, 2026-08-07):

```csharp
            AllocateInstrument allocateInstrument = Json.Deserialize<AllocateInstrument>(line);
            allocateInstrument.InstrumentHeaderId = _serverContext.GetInstrumentHeaderIdByExchangeInstrumentId(allocateInstrument.ExchangeInstrumentId);
            if (allocateInstrument.InstrumentHeaderId < 0)
            {
                Console.WriteLine($"Server.LoadInstruments: {allocateInstrument.Symbol} (exchange instrument id {allocateInstrument.ExchangeInstrumentId}) is no longer listed; not restored.");
                continue;
            }
            OnAllocateInstrument(ref allocateInstrument);          // server-only overload
```

`OnAllocateInstrument(ref AllocateInstrument)` (`Server.cs:480-501`) only allocates the instrument
server-wide, opens its instrument data and writes the admin audit record.

## 1.3 What C++ does (the reference)

`Provider/Server.hpp`, `LoadInstruments`:

```cpp
            // The full client path, not the server-only one: it restores the client and house-book
            // allocations and fires AllocateInstrument, which the CME adapter needs to rebuild routers
            // and market data for orders still working at the exchange after a restart.
            OnAllocateInstrument(allocateInstrument.ClientId, allocateInstrument);
```

That is the public client overload (C# equivalent `Server.cs:503-526`). For a spread it allocates the
legs first, then the spread, each through the private overload (C# `Server.cs:528-550`). On top of
the server-side allocation, that does the following:

| Step | Effect after a restart |
|---|---|
| `_serverContext.AllocateInstrument(clientId, instrumentId)` | the client's `InstrumentIdsByClientId` bit is restored |
| `_serverContext.AllocateInstrument(ServerStrategyId, instrumentId)` | strategy 0 (the house book) gets the instrument back |
| `_clientIdsByCoreGroupId[coreGroupId].AtomicSet(clientId)` | `ReadExecution` polls that client's channel again |
| `WriteToAdmin(clientId, ...)` (spread or outright, not legs) | an `AllocateInstrument` reply in the restored client's admin ring; harmless, because a reconnecting client skips the old ring (§2) |
| `AllocateInstrument?.Invoke(...)` | the owner's callback fires for every restored instrument |

## 1.4 Why it matters

- **Exchange adapters depend on the callback.** The C++ CME adapter sets `AllocateInstrument`,
  calls `LoadInstruments`, and only then sets the save-and-route callback used for live allocations
  (`CmeServer.hpp:301-308`). It builds each instrument's order router and market-data subscription
  from that first callback. Without it, a restarted server has no router for any restored
  instrument. Fills for orders still working at the exchange are then dropped as "unrouted", cancels
  for them (including the dead-client sweep's `CancelAllOrders`) never leave, and the
  reconnect reconcile skips them. Any C# exchange adapter wired the same way has the same exposure.
- **The house book.** With the server-only replay, strategy 0 owns none of the restored
  instruments, so a manual order from a server workspace is refused `InstrumentNotAllocated` until
  some client allocates the instrument again.
- **Client and poll bits.** The client's instrument bits and the CoreGroup poll bits are not restored. A
  returning client heals this on its first `GetInstrument`, but until then the server does not read
  that client's execution channel. A client that never comes back, but still has orders the server
  must manage, is never polled.

## 1.5 The change

`Provider/Server.cs`, `LoadInstruments`, replace the last line of the loop body:

```csharp
-            OnAllocateInstrument(ref allocateInstrument);
+            // The full client path, not the server-only one: it restores the client and house-book
+            // allocations and fires AllocateInstrument, which an exchange adapter needs to rebuild routers
+            // and market data for orders still working at the exchange after a restart.
+            OnAllocateInstrument(allocateInstrument.ClientId, ref allocateInstrument);
```

Requirements:
- Nothing in the C# repo calls `LoadClients`/`LoadInstruments` today; they are public API for a host
  (the C++ entry points `Provider/Main.cpp` and the CME adapter call them). A host must call
  `LoadClients` before `LoadInstruments`, as `Server.cs:160` already documents, so the recorded client
  ids exist. `OnAllocateInstrument(int, ref ...)` throws for a client id out of range, exactly as for
  a live request.
- Use the **public two-argument overload**, not the private `writeAdminReply: false` one, so both
  servers restore identical state, including the admin reply. If C# would rather suppress that
  reply, raise it as a change for both sides; do not diverge.

## 1.6 Verification

1. Start a server, connect a client, allocate an outright and a calendar spread, stop the server.
2. Restart it with the same week.
   - `InstrumentIdsByClientId[clientId]` holds the outright, the spread and both legs.
   - `InstrumentIdsByClientId[0]` (house book) holds them too.
   - `_clientIdsByCoreGroupId[coreGroup]` has the client's bit.
   - The `AllocateInstrument` callback fired once per restored instrument, legs before the spread.
3. Before any client reconnects, send a manual order from a server workspace on the restored
   outright: it is accepted, not `InstrumentNotAllocated`.

---

# 2. Client startup: refuse on any Active order, then skip the backlog

## 2.1 The rule

A client process checks **all 64 of its order slots once, at construction**, not one instrument at
a time when each instrument is allocated:
1. If any slot's `OrderState` is Active, the client refuses to start (throws). A previous process's
   live order holds room, slots and fills this process never made, on any instrument.
2. Every Done slot's `OrderRisk` row is cleared: it belongs to the previous process.
3. The client sleeps 100 ms, so anything the server is still writing for those finished orders
   lands in the ring.
4. The client parks every read cursor at the head of its socket rings (`Recover`). Everything
   queued is about the previous process's finished orders, which are already in the position rows
   that `WorkingRisk` is seeded from. Processing them again would double-count fills into
   `WorkingRisk.Position` and replay stale states and rejects.

Instrument allocation then only seeds `WorkingRisk` from the strategy's own position row; the
per-instrument check is gone.

## 2.2 Why

- **The per-instrument check was the wrong scope.** A previous process's Active order on an
  instrument this process has not allocated yet was not seen until that allocation, and its fills
  could already be queued.
- **The seed double-counted.** The socket constructor skips the backlog once, when the socket is
  built, before the connect handshake. Anything the server wrote between then and the
  `WorkingRisk` seed (late fills, Done states of the previous process's cancelled orders) was
  read after the seed, and a fill was counted into `WorkingRisk.Position` twice.
- **The server writes rows before rings.** `Server.OnFill` updates the order and position rows
  under their locks, then writes the messages to the client's ring after releasing them. A row can
  read Done a few hundred ns before its last messages are in the ring; the 100 ms sleep covers that.

## 2.3 What C++ does (the reference)

`Socket/Socket.hpp`, `ClientSocket`, a passthrough to the existing `Socket::Recover()`:

```cpp
		// Park every cursor at the head of the ring: whatever is queued is skipped.
		inline void Recover()
		{
			_socket->Recover();
		}
```

`Provider/Client.hpp`, end of the constructor body:

```cpp
        // A previous process's Active order would hold room, slots and fills this process never made (see Spec.md).
        ThrowIfPreviousOrdersActive();
        // ensures all messages cleared out
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Every previous order is Done and already in the position rows WorkingRisk is seeded from: skip what is queued for them.
        _socket.Recover();
```

`Provider/Client.hpp`, the check takes no instrument and scans every slot:

```cpp
    void ThrowIfPreviousOrdersActive()
    {
        for (int32_t localIndex = 0; localIndex < 64; ++localIndex)
        {
            Execution::OrderId orderId = Execution::OrderId().ClientId(ClientId).LocalIndex(localIndex);
            const Execution::OrderState& orderState = ClientContext.GetOrderState(orderId).GetReadonlyRef();
            if (orderState.OrderStateStatus == Execution::OrderStateStatus::Active)
                throw std::runtime_error("Order " + orderState.OrderHeader.OrderId.ToString() + " from a previous process is still Active: start again once the server has cancelled it.");
            // Done: its risk row is the previous process's, so this process starts from an empty one.
            ClientContext.GetOrderRisk(orderId).GetRef() = Execution::OrderRisk{};
        }
    }
```

`Provider/Client.hpp`, `OnInstrumentAllocated`: the per-instrument check is removed; it seeds only:

```cpp
        Data::Instrument& instrument = ClientContext.GetInstrument(instrumentId);
        ClientContext.GetPosition(instrumentId);   // ensure the position is created

        // RiskLayer starts from this strategy's own position, every process: the region can outlive one (the GUI maps it).
        // A spread's legs come through here themselves before the spread (GetInstrument onboards them first).
        ClientContext.GetWorkingRisk(instrumentId).Write(Execution::WorkingRisk{ .Position = ClientContext.GetPositionHeader(instrumentId).GetReadonlyRef().Quantity });
```

## 2.4 The change in C#

`Socket/Socket.cs`, `ClientSocket` (class at `:699`). It has no `Recover`; only `ReadOnlySocket`
(`:205`), `WriteOnlySocket` (`:362`) and `Socket` (`:515`) do. Add the passthrough:

```csharp
    // Park every cursor at the head of the ring: whatever is queued is skipped.
    public void Recover()
    {
        _socket.Recover();
    }
```

`Provider/Client.cs`, constructor: after `RiskLayer = new RiskLayer(Context, OrderRejectedSource.Client);`,
which is the last line today (the check needs `Context`):

```csharp
        // A previous process's Active order would hold room, slots and fills this process never made (see Spec.md).
        ThrowIfPreviousOrdersActive();
        // ensures all messages cleared out
        Thread.Sleep(100);
        // Every previous order is Done and already in the position rows WorkingRisk is seeded from: skip what is queued for them.
        _socket.Recover();
```

`Provider/Client.cs`, `ThrowIfPreviousOrdersActive` (`:328`): drop the `instrumentId` parameter and the
instrument filter, so it scans every slot:

```csharp
    private void ThrowIfPreviousOrdersActive()
    {
        for (int localIndex = 0; localIndex < 64; localIndex++)
        {
            OrderId orderId = new OrderId { ClientId = _clientId, LocalIndex = localIndex };
            ref readonly OrderState orderState = ref Context.GetOrderState(orderId).GetReadonlyRef();
            if (orderState.OrderStateStatus == OrderStateStatus.Active)
                throw new InvalidOperationException($"Order {orderState.OrderHeader.OrderId} from a previous process is still Active: start again once the server has cancelled it.");
            // Done: its risk row is the previous process's, so this process starts from an empty one.
            Context.GetOrderRisk(orderId).GetRef() = default;
        }
    }
```

`Provider/Client.cs`, `OnInstrumentAllocated`: delete the call at `:314` and its comment, keep
the `WorkingRisk` seed that follows:

```csharp
-        // A previous process's Active orders would hold room, slots and fills this process never made; the server cancels them when that process closes (see Spec.md).
-        ThrowIfPreviousOrdersActive(instrumentId);
-
```

## 2.5 Accepted residual

A manual (GUI) order booked to this strategy has another client's id, so neither the check nor the
backlog skip covers it: if it fills during the client's startup its fill can be counted twice. Under
the documented one-strategy-per-instrument rule, with no manual orders on an algo while it starts,
this is an operating rule, not code.

## 2.6 Verification

1. Start a client, create an order, kill the client process while the order is working.
2. Restart the client before the server's cancel completes: it throws "still Active".
3. Restart it after the cancel completes, with fills and the Done for that order still queued
   in its ring: it starts, reads none of them, and `WorkingRisk.Position` equals the strategy's
   position row exactly.

---

# 3. `mlockall` failure must stop the process

## 3.1 The rule

Every process pins its memory with `mlockall(MCL_CURRENT | MCL_FUTURE)` once, the first time it
creates shared or anonymous memory. **If that call fails, the process must not start: throw.** It
fails when the process's memlock limit (`ulimit -l`, `LimitMEMLOCK` in a systemd unit) is too low.
A process that carries on unpinned can take page faults of several milliseconds on its hot path,
and a misconfigured live box would trade with only one stdout line as warning.

## 3.2 What C++ does (the reference)

`Tools/Memory.hpp:298`, called from `Memory::CreateAnonymous` and `Memory::CreateOrOpenShared`
(`:98`, `:119`):

```cpp
		// Pins all current + future pages once per process. std::call_once makes the first
		// call win race-free and re-runs only if it threw (mlockall is idempotent otherwise).
		static void MLock()
		{
			static std::once_flag s_mlockOnce;
			std::call_once(s_mlockOnce, []()
			{
				if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
					throw std::runtime_error("mlockall failed: " + std::string(std::strerror(errno)));
			});
		}
```

## 3.3 What C# does today

`Tools/Memory.cs:438-455`, called from `CreateAnonymous` and the shared factories (`:147`, `:179`):
a failure is printed and the process carries on.

```csharp
    private static class MLockGuard
    {
        public static readonly bool Done = DoMLock();

        private static bool DoMLock()
        {
            if (!OperatingSystem.IsLinux()) return true;
            int rc = LinuxMlockall(MCL_CURRENT | MCL_FUTURE);
            if (rc == 0)
                Console.WriteLine("Tools.Memory: mlockall success.");
            else
                Console.WriteLine($"Tools.Memory: mlockall failed (errno={Marshal.GetLastWin32Error()}). Check ulimits.");
            return true;
        }
    }
```

## 3.4 The change in C#

`Tools/Memory.cs`: throw on failure. Keep the success line and the non-Linux no-op.

```csharp
    // ---- mlock (run-once guard; a failure throws and is retried by the next factory call, like C++ call_once) ----

    private static readonly object _mlockLock = new object();
    private static volatile bool _isMLocked;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void EnsureMLocked()
    {
        if (_isMLocked)
            return;
        lock (_mlockLock)
        {
            if (_isMLocked)
                return;
            if (OperatingSystem.IsLinux())
            {
                if (LinuxMlockall(MCL_CURRENT | MCL_FUTURE) != 0)
                    throw new InvalidOperationException($"mlockall failed (errno={Marshal.GetLastWin32Error()}). Check ulimits.");
                Console.WriteLine("Tools.Memory: mlockall success.");
            }
            _isMLocked = true;
        }
    }
```

Do not throw from a static initialiser (`MLockGuard.Done = DoMLock()`): a type initialiser that throws
caches a `TypeInitializationException` for the life of the process, which is not the C++ behaviour
(C++ `call_once` retries after a throw). Replace `MLockGuard` with the guarded method above.

## 3.5 Verification

1. `ulimit -l 64` (or any limit below the process's mapped size), start a C# server or client:
   it throws `mlockall failed ... Check ulimits` before any region is created.
2. With the limit raised (`ulimit -l unlimited`), it prints `mlockall success.` and starts.

---

# 4. SIGHUP: shut down cleanly, unless started under `nohup`

## 4.1 The rule

A closed terminal or dropped SSH session sends SIGHUP. Its default action kills the process
without the exit actions, so both sides catch it and run the same clean shutdown as Ctrl+C.
**Except when the process inherited SIGHUP as ignored**, which is what `nohup` does: then the
handler is not installed, SIGHUP stays ignored, and the process keeps running after the session ends.

| Terminal closes | Without `nohup` | With `nohup` |
|---|---|---|
| Required (C++ today) | Clean shutdown: exit actions run (a server cancels its orders), then exit | Keeps running |

## 4.2 What C++ does (the reference)

`Tools/Application.hpp`, `Init`:

```cpp
			std::signal(SIGINT, SignalHandler);
			std::signal(SIGTERM, SignalHandler);
			// A process started under nohup inherits SIGHUP ignored and must keep running when the session drops.
			struct sigaction hangUp{};
			sigaction(SIGHUP, nullptr, &hangUp);
			if (hangUp.sa_handler != SIG_IGN)
				std::signal(SIGHUP, SignalHandler);
```

`SignalHandler` prints `Hang-up Signal (SIGHUP) Captured. Shutting down...`, runs `OnExit()`, then exits.

## 4.3 What C# does today

`Tools/Application.cs`, static constructor: the handler is registered unconditionally, so a process
started under `nohup` would shut down (orders cancelled) when the session drops, if the handler fires.

```csharp
                s_hangUpRegistration = PosixSignalRegistration.Create(PosixSignal.SIGHUP, context =>
                {
                    Console.WriteLine("Hang-up Signal (SIGHUP) Captured. Shutting down...");
                    OnExit(null, null);

                    // Allow the process to terminate naturally after cleanup
                    context.Cancel = false;
                });
```

## 4.4 The change in C#

`Tools/Application.cs`: read the inherited SIGHUP disposition first and register only when it is not
ignored. The project already compiles `unsafe` code (`Tools/Memory.cs`).

```csharp
        [DllImport("libc", SetLastError = true, EntryPoint = "sigaction")]
        private static extern unsafe int LinuxSigaction(int signal, void* action, void* oldAction);

        private const int SIGHUP = 1;

        // A process started under nohup inherits SIGHUP ignored and must keep running when the session drops.
        private static unsafe bool IsHangUpIgnored()
        {
            // glibc's struct sigaction (152 bytes on x86-64) starts with sa_handler; SIG_IGN is 1.
            byte* oldAction = stackalloc byte[256];
            return LinuxSigaction(SIGHUP, null, oldAction) == 0 && *(nint*)oldAction == 1;
        }
```

```csharp
            else if (!IsHangUpIgnored())
            {
                // Linux: a closed terminal or dropped ssh session sends SIGHUP, whose default action kills the process without the exit actions
                s_hangUpRegistration = PosixSignalRegistration.Create(PosixSignal.SIGHUP, context =>
                {
                    ...unchanged...
                });
            }
```

The check must read the disposition before anything in the process registers a SIGHUP handler,
which is why it sits in the same static constructor, before `PosixSignalRegistration.Create`. If .NET
already turns out to keep an inherited ignore in place without this check, keep the check anyway: it
states the rule in the code and makes the behaviour independent of the runtime.

## 4.5 Verification

1. Start a C# server or client normally in a terminal, close the terminal: it prints the hang-up
   line, runs its exit actions (a server cancels its orders) and exits.
2. Start it as `nohup <program> &`, log out or close the terminal: it is still running afterwards
   (`ps`), and nothing was cancelled.
3. Repeat both with the C++ server: identical results.

---

# 5. Validate the server name before `InitDirectories` deletes anything

## 5.1 The rule

In simulation, `InitDirectories` deletes every file in the server's sub-directories (`Alerts`,
`Audit`, `Fills`, `Positions`, `Series`, `Clients`, `Instruments`). **The server name must be
validated before that delete**, so a simulation run pointed at a live server's name throws without
touching a file.

## 5.2 The danger in C# today

The name check (`ServerContext.ThrowIfInvalidServerName`: a server must live under
`Servers/<ClockMode>/`) runs inside the `ServerContext` constructor, and the constructor of `Server`
calls `InitDirectories` **before** it builds the context (`Provider/Server.cs:101-108`):

```csharp
        ServerName = serverHeader.ServerName.ToString();
        InitDirectories();                                         // deletes first...
        _serverHeaderBox = ServerContext.Connect(in serverHeader);
        _serverSocket = new ServerSocket(ServerName, serverHeader.ClientIds.Length);
        _serverContext = new ServerContext(ServerName, Access.Write);  // ...the name is validated only here
```

A C# server started in simulation mode with a live server's name (`Servers/Realtime/...`) wipes that
live server's fills, positions, audit, clients and instruments files, and only then throws.

## 5.3 What C++ does (the reference)

`Provider/Server.hpp`, `InitDirectories`, first line:

```cpp
    void InitDirectories()
    {
        // Before anything is deleted: a simulation run against a live server's name must throw here, not after wiping its files.
        ServerContext::ThrowIfInvalidServerName(ServerName);
        for (const char* subDirectory : SubDirectories)
```

(The C++ server additionally refuses to run in simulation at all: a deliberate C++-only guard that
throws before its delete loop. That guard is not part of this item and C# does not need it.)

## 5.4 The change in C#

`Provider/Server.cs`, `InitDirectories`, first line:

```csharp
    private void InitDirectories()
    {
        // Before anything is deleted: a simulation run against a live server's name must throw here, not after wiping its files.
        ServerContext.ThrowIfInvalidServerName(ServerName);
        foreach (string subDirectory in _subDirectories)
```

`ThrowIfInvalidServerName` is static, so it needs no context.

## 5.5 Verification

1. Put a marker file in a live server's `Fills` directory (`Servers/Realtime/<name>/Fills`).
2. Start a C# server in simulation mode with that live server's name: it throws the
   invalid-server-name error and the marker file is still there.
3. Start a simulation server with a valid `Servers/Simulation/<name>`: its directories are cleaned as
   before.

---

# 6. A `SharedArray` whose region would exceed `int.MaxValue` bytes is refused loudly

## 6.1 The rule

A shared array's region is `capacity × entryLength` bytes. **Both sides use the same limit,
`int.MaxValue` (2 GiB − 1), and throw when the region would exceed it.** Neither side may map a
larger region, and neither may truncate the length.

## 6.2 What C++ does (the reference)

`Socket/SharedArray.hpp`, constructor (the C++ memory API takes `int32_t` lengths):

```cpp
			size_t total = static_cast<size_t>(_entryLength) * static_cast<size_t>(capacity);
			if (total > static_cast<size_t>(INT32_MAX))
				throw std::overflow_error("SharedArray: total length overflows int32");
			int32_t totalLength = static_cast<int32_t>(total);
```

## 6.3 What C# does today

`Socket/SharedArray.cs:215-219`: the product is overflow-checked in 64 bits only, so C# maps any region
up to `long.MaxValue`. A capacity that C++ refuses would be accepted by C#, and a C++ peer could
then not open the same region.

```csharp
        long fileLength = checked((long)_entryLength * capacity);
        _mmf = SharedMemory.CreateOrOpen(name, fileLength);
```

## 6.4 The change in C#

```csharp
        long fileLength = checked((long)_entryLength * capacity);
        if (fileLength > int.MaxValue)
            throw new OverflowException($"SharedArray({name}): total length {fileLength} exceeds int.MaxValue, the limit shared with C++.");
        _mmf = SharedMemory.CreateOrOpen(name, fileLength);
```

## 6.5 Verification

Construct a `SharedArray<T>` with a 128-byte stride and capacity `16_777_216` (exactly 2 GiB): both
sides throw. With capacity `16_777_215` (2 GiB − 128 bytes, if memory allows) both create it. Current
arrays are far below the limit (an `OrderStates` region is about 0.5 MB), so nothing in use changes.

---

# 7. `Clock`: two bugs found by the C++ port

The C++ port of `Tools/Clock.cs` (`Tools/Clock.hpp`) fixes two C# defects. C# must apply the same
fixes so both clocks behave identically.

## 7.1 `IsRunning` must be cleared however `Start` exits

**The bug.** `s_isRunning = false` is set only at the end of `RunSimulation` / `RunRealtime`. If a
`Started` handler throws (caught in `Start`, so `Run*` never runs), or the post-loop `Interject`, or an
`Exception` handler throws, `IsRunning` stays true for the life of the process. The "Stop Clock" exit
action then waits `while (IsRunning) Thread.Sleep(1)` forever, so the process cannot shut down.

**C++ (reference)**, `Tools/Clock.hpp`, `Start`:

```cpp
			try
			{
				try
				{
					Started.Invoke(UtcNow());
					if (Mode() == ClockMode::Simulation) RunSimulation(); else RunRealtime();
				}
				catch (...)
				{
					OnCurrentException();
				}
			}
			catch (...)
			{
				// C# leaves IsRunning set when a throw skips the Run* epilogue; the "Stop Clock" exit action would then wait forever.
				s_isRunning = false;
				Stopped.Invoke(UtcNow());
				throw;
			}
			s_isRunning = false;
			Stopped.Invoke(UtcNow());
```

**The change in C#** (`Tools/Clock.cs`, `Start`): clear the flag in the `finally`, before `Stopped`:

```csharp
        finally
        {
            s_isRunning = false;
            Stopped?.Invoke(Now);
        }
```

(The `s_isRunning = false;` lines at the end of `RunSimulation` and `RunRealtime` can stay; they become
redundant.)

## 7.2 `ConsumeReminders` must never run a reminder that is not due

**The bug.** `ConsumeReminders` peeks and then dequeues in two separately locked steps:

```csharp
        while (s_reminders.TryPeek(out _, out Reminder reminder) && reminder.Timestamp <= now)
        {
            if (s_reminders.TryDequeue(out _, out reminder))
```

If another thread removes the peeked (due) reminder with `TryRemoveReminder` between the two calls,
`TryDequeue` pops the **next** reminder even though its time has not come, and runs it early.

**C++ (reference)**: the check and the pop happen under one lock,
`LockedPriorityQueue::TryDequeueIfAtMost(maxPriority, out priority, out value)`:

```cpp
			while (s_reminders.TryPeek(priority, reminder) && reminder.Timestamp <= now)
			{
				// C# TryDequeue: a TryRemoveReminder between the peek and here could pop a reminder not yet due.
				if (s_reminders.TryDequeueIfAtMost(now, priority, reminder))
```

**The change in C#**: add to `LockedPriorityQueue` a dequeue that pops the minimum only if its
priority is `<= maxPriority`, inside the same writer section, and use it in `ConsumeReminders`:

```csharp
            if (s_reminders.TryDequeueIfAtMost(now, out _, out reminder))
```

## 7.3 C++-only differences, accepted (nothing for C# to do)

- The "Stop Clock" exit action does not wait when it runs on the thread that called `Start`: a C++
  signal handler can run on the clock thread itself and would wait for itself. (C# runs Ctrl+C on a
  separate thread.)
- C++ events (`Tools::Event`) unsubscribe with the handle `+=` returned (`std::function` has no
  equality); `Reminder` can be built from a shared callback so copies compare equal, like C#'s
  delegate identity.
- `Clock.Now` is named `Clock::UtcNow()`; `Mode`/`Begin`/`End`/`SimulationSpeed` are getter/setter pairs.
- The C++ sample `Strategy/Scenario.hpp` stops and rethrows the first `Clock.Exception` (it has no
  `AlertManager`), refuses a restart after `Stop`, and feeds `OnInterject` the larger of the server
  header `Timestamp` and `Clock::UtcNow()`.

## 7.4 Open, not part of this item

The C++ server's `ServerHeader.Timestamp` can step backwards (it is written from more than one thread).
That is the known-open multi-writer `ServerHeader` item and will be handled separately.

## 7.5 Verification

1. A `Started` handler that throws: `Start` returns, `Stopped` fired, `IsRunning` is false, and the
   process exits normally (the "Stop Clock" exit action does not hang).
2. Two reminders at t1 < t2; from another thread remove the t1 reminder while the clock is at t1: the
   t2 reminder does not run until the clock reaches t2.
