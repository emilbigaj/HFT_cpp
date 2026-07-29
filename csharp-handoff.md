# Handoff: persistent client sockets — C++ changes to mirror in C#

Branch `persist-client-sockets`:

- `35dbd24` — persist client sockets across client restarts (9 files, +419/−128)
- `6cfc873` — path-join the remaining shared-memory region names (2 files, +7/−5)

This describes what changed on the C++ side and why, so the C# implementation of the same
shared-memory protocol can be brought into lockstep. Two changes are **byte-level breaking** and
will cause the two sides to silently miss each other if not mirrored; the rest is behavioural.

I have only seen `HFT/Logging/LoggingServer.cs` from the C# side, so requirements below are stated
as layout and behaviour rather than as edits to specific C# files, except where I quote
`LoggingServer.cs` directly.

---

## 1. Why any of this exists

A client socket used to be torn down one second after its client process died:

```
client dies -> PollPids -> CloseClient (Closed) -> 1s -> DisposeClient -> Socket disposed
```

and `ServerSocket.Write` dropped anything addressed to a non-`Open` client. So in the window
between a client dying and reconnecting, the server could not deliver to it.

That window is exactly when it matters. On an iLink3 reconnect the exchange retransmits fills the
session missed. The server reconnects to the exchange before the strategies come back, CME pumps
the missed fills in, and the server had nowhere to put them. They were lost from the client *and*
from that client's audit, because the logging server taps the same shared-memory region.

So: **client sockets now outlive their client process.**

---

## 2. BREAKING — `ServerHeader` gained a field

`Provider/Allocate.hpp`, appended after `OrdersPerClient`:

```cpp
// Client sockets outlive their client process: a dropped client goes Detached instead of
// being disposed, so the server keeps writing into its ring and the audit tap keeps reading.
// WIRE FIELD — the C# side must mirror it byte-for-byte.
bool Persistance = true;
```

Layout, verified by compiling and printing:

| | before | after |
|---|---|---|
| `sizeof(ServerHeader)` | 172 | **173** |
| `offsetof(Persistance)` | — | **172** |

It is appended, so **every existing field keeps its offset**. The C# mirror only needs a trailing
field.

### C# `bool` pitfall

`MemoryMarshal.Read<T>` and `Unsafe.SizeOf<T>` use the **managed** layout, where `bool` is 1 byte —
so a plain C# `bool` matches. But `Marshal.SizeOf<T>` and P/Invoke marshalling default `bool` to a
4-byte `BOOL`. If anything in the C# codebase calls `Marshal.SizeOf` on this struct, or the struct
carries `[StructLayout(..., Pack = 1)]` and is used with interop marshalling, you will get 176 and
the two sides will disagree.

Safest: declare it as `byte` with a `bool` property, or `[MarshalAs(UnmanagedType.U1)] bool`.
Either way add an assertion:

```csharp
Debug.Assert(Unsafe.SizeOf<ServerHeader>() == 173);
```

### Semantics

`true` (the default) means the server keeps client sockets alive across client restarts. It is
published in the header the server is constructed with, stored into the `ServerHeader` letterbox,
and therefore visible to every process that opens a `Context`.

One gotcha: `ServerContext::Connect` stores via `LetterBox.TryStore`, which **only writes when the
letterbox is empty**. A server restarting while the region is still mapped by someone keeps the
*previous* header, including its `Persistance`. Changing the flag requires the region to be gone,
not just a process restart.

---

## 3. BREAKING — shared-memory region names changed

`Provider/Context.hpp` switched from string concatenation to `std::filesystem::path` joining for
every shared-memory region name:

```cpp
// before
_serverHeaderBox(serverName + "ServerHeader", ServerAccess)
// after
_serverHeaderBox(serverName / "ServerHeader", ServerAccess)
```

Region names run through `Tools::Sanitize`, which replaces every character that is not
`[A-Za-z0-9_\-.]` with `_`. So the path separator becomes an underscore and **every region name
gained a `_` separator**:

```
before:  /mnt/S/Servers/Simulation/Foo + ServerHeader
         -> HFT__mnt_S_Servers_Simulation_FooServerHeaderLetterBox
after:   /mnt/S/Servers/Simulation/Foo / ServerHeader
         -> HFT__mnt_S_Servers_Simulation_Foo_ServerHeaderLetterBox
```

Affected regions — all of them:

| region | new name |
|---|---|
| server header letterbox | `<serverName>/ServerHeader` + `LetterBox` |
| client socket headers | `<serverName>/ClientHeaders` |
| instrument headers | `<serverName>/InstrumentHeaders` |
| header id by instrument id | `<serverName>/InstrumentHeaderIdByInstrumentId` |
| instrument ids by client id | `<serverName>/InstrumentIdsByClientId` |
| client ids by instrument id | `<serverName>/ClientIdsByInstrumentId` |
| markets by price | `<directoryPath>/MarketsByPrice` |
| risk limits | `<serverName>/RiskLimits` |
| order states | `<serverName>/OrderStates` |
| order targets | `<serverName>/OrderTargets` |
| local position headers | `<serverName>/LocalPositionHeaders` |
| server position headers | `<serverName>/ServerPositionHeaders` |

If the C# side still concatenates, it will open a **different, empty region** and see nothing —
no error, just permanent silence. This is the single most likely cause of "it built, it ran,
nothing happened".

Socket channel names (`SocketUtils::GetSocketName`, `GetChannelName`) and the per-instrument
broadcast ring (`SocketChannel::GetInstrumentDataName`) are **unchanged** — still plain
concatenation with explicit `_`.

Also unchanged, and deliberately so: the `.server` / `.audit` / `.alert` suffixes on
`ClientSocket` names (`ServerName.string() + ".audit"` and friends). Those are extensions, not path
segments — `LoggingServer.OnDirectSubscribed` parses them with `Path.GetExtension(clientName)` to
pick a writer. They must stay concatenated.

### The conversion was initially incomplete — now fixed, but worth knowing

Three sites named the `ServerHeader` letterbox and two of them still concatenated, so the server
published its header into one region while `Context` read a different, empty one, and
`Context::EnsureConnected()` spun forever on a box nobody writes. That hung both the `Server` and
`Client` constructors. A fourth site had the same problem for `MarketsByPrice`.

All four now path-join:

| site | expression |
|---|---|
| `Context.hpp:170` `_serverHeaderBox` | `serverName / "ServerHeader"` |
| `Context.hpp:424` `ServerContext::Connect` | `std::filesystem::path(serverNameStr) / "ServerHeader"` |
| `Client.hpp:65` `ReadServerChannelLengths` | `serverName / "ServerHeader"` |
| `Client.hpp:105` `_serverMarketsByPrice` | `ServerName / "MarketsByPrice"` |

The lesson for the C# port: this failure mode is silent in the worst way. Mismatched names do not
error — `CreateOrOpen` happily creates the second region — so the symptom is a hang or an empty
read, far from the cause. If you port this, grep for every region-name construction site at once
rather than converting them as you find them.

---

## 4. Socket layer — the `Detached` lifecycle

### New status

```cpp
enum class ClientStatus : uint8_t
{
    Disposed = 0,
    Detached = 1,   // new
    Open     = 2,
    Closed   = 3
};
```

Renumbered, but this enum lives only in `ServerSocket`'s heap-allocated per-client bookkeeping —
it is never written to shared memory, so the renumber is not a wire change.

`Detached` means: **the socket exists and is writable, but no client process is attached.**

### State machine

```
                    Persistance = true                    Persistance = false
  Disposed --connect--> Open                       Disposed --connect--> Open
  Open --client dies--> Detached                   Open --client dies--> Closed
  Detached --reconnect--> Open                     Closed --1s--> Disposed
```

Under `Persistance`, `Closed` and `DisposeClient` are never reached. That has a consequence the
logging server depends on — see §7.

### What changed around it

- **Write gate**: `ServerSocket::Write` now permits `Open || Detached`. This is the change that
  lets the server buffer fills for an absent client.
- **Read gate**: unchanged, still `Open` only, returning `Closed` otherwise. Deliberate — nothing
  writes to a detached client's inbound ring, and returning `Closed` stops the server acting on a
  dead client's last unread order target, which `CancelAllOrders` is about to cancel anyway.
- **Reconnect**: `PollLetterBox` sees `Detached` and **skips `CreateClient`**, reusing the existing
  `Socket`. This is what preserves both the buffered ring and the server's own read cursors.
- **`ClientSocket::Connect()` now returns `int32_t`** — the `ClientId` from the handshake reply,
  which the server already stamps into the header before storing it. The client no longer needs the
  `AllocateClient` admin message to learn its id. Necessary because on a `Detached` re-attach
  nothing fires `ClientAllocated`, so no admin message is sent.
- **Callback split**: `ClientAllocated(SocketHeader)` now means "a socket was created for this
  client" and fires once per socket. `ClientOpened(int clientId)` / `ClientClosed(int clientId)`
  are new and fire on every attach/detach. Anything that needs a live socket must hang off
  `ClientOpened`, not `ClientAllocated` — at `ClientAllocated` time the socket may not exist yet
  and the status may still forbid writing.

---

## 5. Ring protocol — nothing clears, everything recovers

This is the conceptual core, and the part most likely to be got wrong independently.

### Nothing clears shared memory any more

`Socket::Reset()` used to zero the whole region and reset cursors, and `OpenClient` called it on
every connect. It is no longer called under `Persistance`.

The reason is not just "we want to keep the buffer". `Reset()` **cannot work as a synchronisation
mechanism**: it clears the shared region and *one* side's cursors, but the other side's cursors
live in another process and are unreachable. It only ever appeared to work because clients used to
start at sequence 0, so "clear everything" and "the client's cursor" coincidentally agreed.

Concretely, with a client that recovered to sequence 1002 and a server that then clears and
restarts at 2: `IsThisNewerThan(2, 1002)` is false, forever. The client reads `Empty` for the rest
of its life, with no error anywhere.

### Instead: every socket recovers its own cursors at construction

`Socket`'s constructor calls `Recover()` on each sub-socket:

```cpp
_writeOnlySockets[i] = std::make_unique<WriteOnlySocket>(...);
_writeOnlySockets[i]->Recover();
...
_readOnlySockets[i] = std::make_unique<ReadOnlySocket>(...);
_readOnlySockets[i]->Recover();
```

Both delegate to `Protocol::SkipRing`, which walks the ring to the writer's head:

- **writer** resumes the existing sequence space instead of restarting at 0. A restart makes
  everything it publishes read as stale to any reader parked higher — permanently.
- **reader** parks at the head instead of replaying the backlog.

`Recover()` also clears the socket's `_isClosed` latch: a close message from the previous session
is not ours, and on the writer side a latched `_isClosed` makes `Write()` emit close messages
instead of payloads.

### Why the reader skips the backlog rather than replaying it

This looks wrong at first — the server buffered those fills, why throw them away?

Because the client's authoritative state was never in the ring. `Server::OnFill` updates both
position headers by direct seqlock write to the `SharedArray`, and `OnOrderState` does the same to
`_orderStates`. **Neither is gated on client status.** While a client is `Detached`, its positions
and order states in shared memory are kept current the whole time. Only the *notification* is
gated.

So on reconnect the client re-derives from `PositionHeader` / `OrderState`, which are absolute
snapshots and already correct. Replaying the ring would be actively wrong: `Fill` is an event, and
re-delivering it double-counts anything the strategy accumulates.

The buffering exists so the **audit tap** sees the fills, not so the client replays them.

**Contract for strategies**: treat `Position::Header()` as the truth for position and P&L, never an
accumulator fed by `Fill` callbacks. Fills that land while a strategy is down are real, are in the
audit, are already integrated into the header, and will never arrive as callbacks.

### `Protocol::SkipRing`

```cpp
static void SkipRing(uint8_t*& src, uint8_t* start, uint8_t* end, uint64_t& lastReadEvenSeq);
```

Walks entries by their length rather than copying them, so it needs no buffer, and never writes to
the ring — a listener mapped to the same region is undisturbed. It stops at:

1. an odd sequence (unpublished) — that slot **is** the head
2. a sequence not newer than `lastReadEvenSeq` — the writer's pre-zeroed slot, i.e. the head
3. the sequence changing under it — a writer touching a settled entry means we reached the head
4. a length that is negative, larger than the ring, or that would advance past `end`

Note (1): it deliberately does **not** spin on an in-progress write, unlike `TryReadFromRing`. A
writer that died mid-write leaves an odd sequence nobody will ever complete, and crash recovery is
precisely when that exists. Spinning there would hang forever.

Note (4): `TryReadFromRing` gets its length bound for free from `Copy()`, which throws when an
entry overruns the destination. With no copy, `SkipRing` must range-check explicitly or a torn
header walks it out of the ring.

Termination: sequences rise strictly by 2, so a full lap always meets an entry that is no longer
newer.

---

## 6. `Protocol` helper changes

### `ReadRingSeq` — one definition of the redirect rule

```cpp
static inline uint64_t ReadRingSeq(Header64*& srcHdr, uint8_t* start);
```

Resolves a cursor to the header the next entry actually lives at: bad magic or the wrap marker
means the writer moved on to `start`. The reference parameter matters — the redirect has to reach
the caller, because the length read and the pointer advanced from must come from the same resolved
header.

Previously this logic was duplicated in `TryReadFromRing` (as a lambda), in the status probe, and
in the new skip. Now all three share it.

The slot-path `TryRead` keeps its own separate lambda deliberately: a standalone slot has nowhere
to redirect *to*, so it returns 0 on bad magic instead. Different rule, not shared.

### `GetReadStatus` / `GetReadStatusFromRing` now check `Magic`

Previously the probe redirected only on the wrap marker, while the read path redirected on
`magic != s_magic || seq == s_ringWrapMarker`. That asymmetry is worth fixing because **the probe
is the logging server's scheduling signal**, and both directions of disagreement are permanent
since neither side ever advances:

- probe says `New`, read says `Empty` → the poller re-queues that reader every pass forever and a
  worker drains nothing. A spin.
- probe says `Empty`, read would have found data → that channel is never scheduled and its audit
  data is stranded.

C# equivalent: make `GetReadStatusFromRing` resolve the cursor exactly as `TryReadFromRing` does,
and make `GetReadStatus` treat bad magic as `Empty` (as a zero sequence already is). Not a wire
incompatibility, but the same latent failure exists independently on each side.

---

## 7. Constraints the LoggingServer must honour

The C++ side now depends on specific logging-server behaviour. These are load-bearing.

### 7.1 The audit tap must never be unsubscribed for a persisted client

The C++ server no longer sends the zeroed-channel close signal, because `DisposeClient` — the only
thing that fires `ClientDeallocated` — is unreachable under `Persistance`.

The C# side must not tear the tap down on its own either. As written it doesn't, and it's worth
understanding why so it isn't accidentally "fixed":

```csharp
ReadStatus overallStatus = ReadStatus.Empty;
...
overallStatus = Min(readStatus, overallStatus);
```

`Min` takes the numerically smaller of `New=0 < Old=1 < Empty=2 < Closed=3`, and the accumulator
starts at `Empty`. So `ClientSocketReader.TryRead` can never return `Closed`, which makes the
`readStatus == ReadStatus.Closed` branch in `WorkerLoop` unreachable for child readers. A client
writing a `CloseMessage` on shutdown does **not** kill its tap. Keep it that way.

### 7.2 A recreated `SocketListener` re-reads the whole ring

`SocketListener`'s constructor builds fresh `ReadOnlySocket`s at `_readPtr = _startPtr`,
`_readSeq = 0`. Since nothing clears the ring any more, a tap that is recreated re-ingests every
entry still resident — up to a full ring of duplicate fills and positions, including duplicate rows
in `AuditWriter`'s per-symbol files.

Previously this was bounded because `OpenClient` zeroed the region at roughly the same moment the
tap was rebuilt. That safety net is gone.

### 7.3 Unsubscribe is effectively permanent tap loss

```csharp
private void OnChildSubscribed(SocketHeader socketHeader)
{
    string uniqueName = socketHeader.Name;
    if (_clients.ContainsKey(uniqueName)) return;   // <-- here
```

`OnChildUnsubscribed` → `RemoveClient(name)` with `disposeNow: false` only sets `IsPendingDispose`
and queues; the actual `_clients.TryRemove` happens later on a worker. Both callbacks fire from
`SocketHeaderWriter.Write` on the same thread, back to back, so an unsubscribe/resubscribe pair
hits that early return, the resubscribe is dropped, and then the worker disposes the old reader.

Net: the tap dies and never comes back. Worth hardening even though the C++ side no longer triggers
it.

### 7.4 New at startup: headers and instrument allocations arrive before any client exists

With persistence the server replays its state on startup:

- `LoadClients` writes one `SocketHeader` per persisted client to the `.server` channel, so
  `SocketHeaderWriter` subscribes a tap for each. These arrive **before any client process exists**.
  `SocketHeaderWriter` handles them identically; no change needed.
- `LoadInstruments` re-emits one `AllocateInstrument` per persisted instrument to the `.audit`
  channel.

7.4's second point is not cosmetic. `AuditWriter._symbols` is an in-memory
`Dictionary<int,string>` populated **only** from `AllocateType.Instrument`, and `GetSymbol` falls
back to `$"UnknownSymbol_{instrumentId}"`. That symbol names files:

```csharp
case (byte)OrderType.Fill:      Context.GetFillsFilePath(_filePath, symbol);
case (byte)OrderType.Position:  Context.GetPositionFilePath(_filePath, symbol);
```

and the position file is read back by the server — `ServerContext::AllocateInstrument` does
`ReadLastLine(GetPositionFilePath(DirectoryPath, symbol))` and falls back to a default-constructed
`PositionHeader` when it finds nothing. So a session whose positions landed in
`UnknownSymbol_5.position` makes the *next* startup silently restore a **zero position**.

Duplicate `AllocateInstrument` for an id already in the map is expected and harmless —
`_symbols.TryAdd` ignores it.

---

## 8. `Context` — what is durable and what is derived

Worth being explicit, because it drove several decisions.

**Durable** (survives a full restart):

- `<ServerName>/Clients/<date>.allocateclient` — one JSON `SocketHeader` per line
- `<ServerName>/Instruments/<date>.allocateinstrument` — one JSON `AllocateInstrument` per line,
  including `Symbol`
- per-symbol `.position` / `.fill` / `.risklimit` files

**Derived** (rebuilt at startup, do not treat as a record of truth):

- `_instrumentIdsByClientId`, `_clientIdsByInstrumentId` — rebuilt by `LoadInstruments` via
  `ServerContext::AllocateInstrument(clientId, instrumentId)`
- `_clientIdsByCoreGroupId` — rebuilt on every attach by `OnClientOpened`
- `AuditWriter._symbols` — rebuilt by the replayed `AllocateInstrument` messages

All shared-memory regions are unlinked once the last mapper drops, so shared memory is never the
durable store. The files are.

The date key is the **CME week open** (Sunday 17:00 America/Chicago, as UTC), not the calendar day —
see `CmeWeekOpen` in `Provider/Main.cpp`. `Tools::Timestamp` gained `FromChrono` / `ToChrono` to
support it.

### Ordering requirement

`AllocateClientId` zeroes `_instrumentIdsByClientId[clientId]` on the allocate path (the
name-match path returns early and is safe). So `LoadClients` **must** run before `LoadInstruments`,
or the rebuilt subscriptions are wiped.

Startup order, from `Provider/Main.cpp`:

```
1. Server ctor                 // sets _serverSocket.Persistance from the header
2. LoadClients(week)
3. install AllocateClient hook -> SaveClient
4. LoadInstruments(week)
5. install AllocateInstrument hook -> SaveInstrument
6. Connect()                   // Listen() only
```

Two deliberate details:

- the save hooks are installed **after** the loads, so replaying state does not re-append it. Do the
  same on the C# side or the files grow by N lines per restart.
- `Connect()` starts the listen thread and does nothing else. It must come **after** the loads, or
  the poll thread races them for the same `clientId` — both would assign
  `_clientHeaders[id].ClientSocket`.

---

## 9. Checklist for the C# side

Must, or the two sides do not talk:

- [ ] `ServerHeader`: append `Persistance`, verify `Unsafe.SizeOf<ServerHeader>() == 173`
- [ ] region names: path-join semantics (`<name>/<Array>`, sanitized to `_`) for all 12 regions

Must, or persistence misbehaves:

- [ ] `ClientStatus.Detached`, and the `Open → Detached → Open` lifecycle
- [ ] write gate accepts `Open || Detached`
- [ ] reconnect reuses the existing socket rather than rebuilding it
- [ ] stop clearing shared memory on connect
- [ ] `Socket` ctor recovers each sub-socket's cursors (`SkipRing`); **not** `SocketListener`, which
      must keep reading from the start
- [ ] `ClientSocket.Connect()` returns the `ClientId` from the reply
- [ ] `ClientOpened` / `ClientClosed` callbacks, with anything needing a live socket on
      `ClientOpened`

Should, same latent bug exists independently:

- [ ] `GetReadStatus` / `GetReadStatusFromRing` check `Magic` and resolve the cursor as the read
      path does
- [ ] harden `OnChildSubscribed` against the deferred-removal early return (§7.3)

---

## 10. Not done, and caveats

- **None of this has been executed.** Both paths were verified by reading. `Provider/Main.cpp` has
  no run loop — it calls `Connect()` and returns, so nothing dispatches `ReadAdmin()` or
  `ReadExecution(cg)`. What I'd most want to watch under a real client is the `Detached` re-attach
  preserving the buffered ring, and the tap continuing across a disconnect without duplicating.
- **`DefaultServerHeader.ServerName` is not a valid server name.** It is `"ServerName"`, but
  `ServerContext::ThrowIfInvalidServerName` requires the name to start with
  `/mnt/S/Servers/<Mode>/`. So `Provider::Server server(Provider::Server::DefaultServerHeader)`
  throws `invalid_argument` at construction. The header needs the full path — the same string
  `ServerContext::GetDirectoryPath(name)` produces, since that is what a client derives its region
  names from.
- **No geometry check on re-attach.** If a client reconnects with different channel lengths — e.g.
  `CoreGroupIds` changed between runs — the existing socket's views point at wrong offsets in a
  differently-sized region. Should compare the header's channel counts and lengths and rebuild on
  mismatch.
- **`Persistance = false` is untested** and has a known hazard: `OpenClient` still calls `Reset()`
  on that path, which clears the region after the client has already recovered its cursors in its
  constructor, leaving the client deaf. Left in deliberately, but do not run non-persistent servers
  without addressing it.
- **A server restart while a client is still running fails at construction**: `ServerContext::Connect`
  throws when the `ServerHeader` letterbox is non-empty, and that region survives while any client
  maps it. Pre-existing, but it is the scenario persistence is aimed at.
