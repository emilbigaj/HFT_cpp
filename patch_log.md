# Patch log

Newest first. Each entry says what changed, why, and what it broke or unblocked.

Mirrors the convention in the C# repo's `patch_log.md`. The two libraries talk through shared memory
as separate processes, so an entry here that changes a wire struct, an enum value, a region name or a
ring protocol rule needs a matching entry there.

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
