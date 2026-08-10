# C++ ↔ C# interop register

Audit of `emilbigaj/HFT_csharp` against this tree, by a 14-agent sweep with adversarial verification of
every finding, plus direct compilation of both sides to get real offsets. Supersedes the earlier
one-directional handoff document.

The two libraries are separate OS processes sharing memory. A disagreement about struct layout, enum
numbering, region naming or ring semantics does not error — `CreateOrOpen` happily makes a second
region, a wrong enum decodes as a different valid value, a mismatched offset reads a neighbouring
field. Every failure mode below is silent.

Status as of `eb5fc0a`.

---

## 1. Closed — verified byte-for-byte in sync

Confirmed identical by compiling both sides and comparing offsets, not by reading:

- **`Header64`**, `HeaderLength`, the magic constant, the wrap marker, `GetAlignedEntryLength`
- **Ring write / read / skip algorithms** — line-for-line equivalent, including pre-zeroing, wrap-marker
  placement, the +1/+2 sequence discipline, and the redirect condition
- **`SkipRing` / `ReadRingSeq` / magic-checked probes / `Recover()`** — C# has all of them; this is *not*
  a C++-ahead area
- **`SocketHeader`** — 344 bytes, same field order and offsets; connect handshake matches
- **`ClientStatus`** values and the `Open`/`Closed`/`Disposed`/`Detached` lifecycle
- **Region naming** — every common region matches byte-for-byte after sanitization, plus the `HFT_`
  namespace, lock infix, sanitize rule, huge-page threshold and LetterBox suffix
- **`ServerHeader`** (173 B, `Persistance` at 172) and **`AllocateInstrument`** (with `ExchangeInstrumentId`)
- **`AlertManager` wire format** — `Header(4)` + object bytes + ASCII message
- **`OrderHeader`, `OrderProfile`, `Fill`, `OrderTarget`, `OrderRejected`, `PositionHeader`, `AheadOfOrder`**
- **`SideByPrice64`** (320 B) and **`MarketByPrice64`** (664 B) — see §6 for why this is luck, not design
- **`RiskLimit`** (36 B), **`OrderRisk`** (64 B), **`OrderStateReason`**, **`OrderRejectedReason`** numbering —
  closed by `3fa223d`

---

## 2. Still breaking interop — C++ must change

### 2.1 `ServerStrategyId` — clientId 0 is the house book on C#, a real client on C++

C# `ServerContext.Connect` (`Context.cs:783-785`) sets `ClientIds` bit 0 before publishing the header, so
`LowestClear()` hands the first real client **id 1** and slot 0 is permanently the server's own manual book.
C++ publishes bit 0 clear, so its first client gets **id 0**.

With a C++ server and a C# GUI, every manual server order (`StrategyId == 0`) is attributed to a real
client's book: that client shares `LocalPositionHeaders` row `0*64+instrumentId` and order slots 0..63 with
the house, and `OrderId.IsAlgoOrder()` (`ClientId != StrategyId`) misclassifies. One-directional — a C++
client against a C# server just sees id 0 taken, which is harmless.

**Fix (C++):** add `OrderIdAllocator::ServerStrategyId = 0`; have `ServerContext::Connect` copy the header
and `copy.ClientIds.Set(ServerStrategyId)` before `TryStore`.

### 2.2 `SocketHeader` length-array tails are uninitialised

`ClientToServerLengths`/`ServerToClientLengths` have no initialiser and the C++ constructor fills only the
first `ChannelCount` entries, so bytes 280..343 carry stack garbage into shared memory. A typical
one-channel client publishes 28 indeterminate bytes per direction. C# writes `this = default` first.

Readers bound their loops by the counts, so nothing misbehaves *today* — but the same logical header
serialises to different bytes depending on which language produced it, which breaks any memcmp, hash or
checksum, and it is an uninitialised-stack disclosure into a region the C# server maps.

**Fix (C++):** `ClientToServerLengths{}, ServerToClientLengths{}` in the ctor init list at `Socket.hpp:107`.

### 2.3 Default values disagree on three wire fields

| field | C++ default | C# default | consequence |
|---|---|---|---|
| `OrderState.OrderStateStatus` @49 | `Active` | `Done` | a partially-initialised state means the opposite thing per side |
| `OrderRejected.OrderRejectedSource` @33 | `Server` | `Client` | misattributes blame for a rejection |
| `SocketHeader.ClientId` | `-1` | `0` | `-1` means "unallocated" to `AllocateClientId` |

`Done = 0` is the safe value on zeroed shared memory, so prefer dropping the C++ `= Active` initialiser and
making every publisher set it explicitly. The other two want an explicit default on the C# side.

### 2.4 `Client` name resolution

`new Client("Alpha", "CME")` gives C++ `/mnt/S/Strategies/Simulation/Alpha` (it applies `GetDirectoryPath`)
and C# `Alpha` (verbatim) — disjoint region sets. C# throws at `ClientContext` construction rather than
opening a phantom region, but only *after* the socket region was created under the wrong name.

### 2.5 `GetDirectoryPath("")` trailing separator

C++ validates against `/mnt/S/Servers/Simulation/`, C# against `/mnt/S/Servers/Simulation`. So
`/mnt/S/Servers/SimulationTest` is accepted by C# and rejected by C++. Fails loudly, but the two disagree
about what a valid server name is.

### 2.6 `String128` encoding

C++ copies UTF-8 bytes and bounds by byte count; C# casts UTF-16 chars to bytes and bounds by char count.
For `U+00E9`, C++ writes `C3 A9` and C# writes `E9`. The handshake compares all 128 bytes, so a non-ASCII
client name makes `Connect()` spin forever. Pure ASCII — all current usage — is unaffected.

### 2.7 `PositionHeader` glaze omits `AlgoStatus`

The `.position` file is authored by the C# LoggingServer and always contains `AlgoStatus`; C++ ignores the
key and leaves the member at `Paused`. That accidentally matches C# realtime but diverges in simulation,
where `Context.cs:976` forces `Live` so a backtest can trade.

**Fix both halves in one commit:** add the glaze entry *and* the simulation override
(`Context.cs:976` mirror). Adding the serialiser alone re-introduces the bug `patch_log.md:86-90` closed —
a persisted `Live` re-arming a strategy at startup.

---

## 3. Still breaking interop — C# must change (C++ is ahead)

### 3.1 `NoInstrumentId` sentinel

C++ reserves the top of the instrument field (16383) as "no instrument" and
`ThrowIfInstrumentIdOutOfRange` refuses it, so `MaxInstrumentId` is 16382. C# hands 16383 out as a valid id.
A C# server that allocates that many instruments mints order ids whose `InstrumentId` reads to C++ as "no
instrument" — and C++ validation rejects them outright, so it cannot process orders for that instrument.

**Fix (C#):** `NoInstrumentId = (1 << InstrumentBits) - 1`, `MaxInstrumentId = NoInstrumentId - 1` at
`OrderIdAllocator.cs:28`, and mirror the `Execution/Main.cpp:40-53` assertions in a C# test.

### 3.2 C# `ExponentialPause` is dead code

`Tools/Tools.cs:42-50` declares `int count = 1` as a method-local, loops once, then does `count <<= 1` on a
value discarded at return. Both `Protocol.cs:220` and `:387` therefore issue exactly one `Pause()` — the same
backoff as C++'s `_mm_pause()`. Behaviourally in sync by accident; the intended backoff never happens.

### 3.3 C# short-side position limit cannot trip — and C++ now reproduces it

`RiskLayer.cs:361/368/374` applies the sign twice:

```
worstWorkingQuantityDelta = ((Wa * s) - (Wb * s)) * s   ==   (Wa - Wb) * s²   ==   (Wa - Wb)
```

`s²` is 1 for both sides, so the delta is an unsigned magnitude. Added to `WorstShortWorkingQuantity` at
`:378` it drives the short aggregate *positive* — which is exactly the bug the comment above it claims to
have fixed. With the aggregate positive, `worstShortQuantity < -MaxPositionQuantity` can never be satisfied
however much is working.

**This tree now reproduces it verbatim**, because the port was faithful. Fix on both sides together: drop one
of the two `* sign` applications.

---

## 4. Shared bugs — both sides wrong the same way

### 4.1 `RiskLimit` is published on an execution channel

Both servers `WriteToExecution(strategyId, coreGroupId, riskLimit)`. `RiskLayerRefactorPlan.md:409` states
the opposite as a correctness constraint — records without an `OrderHeader` go to the admin channel only.
Two failure modes:

1. **Client kill.** Any client whose `StrategyId` matches receives type 16 on its execution channel and
   throws out of the hot loop (`Client.hpp:434` / `Client.cs:481` both `throw` on an unknown type).
2. **Logger corruption.** `ClientSocketReader.TryRead` applies `GetCreationTimestamp` to every record on a
   non-admin channel; it slices 4 bytes and reads the next 28 as an `OrderHeader`. `RiskLimit` has none, so
   `MaxOrderQuantity`/`MaxPositionQuantity` are read as timestamps. With `int.MaxValue` that advances
   `_maxSeen` permanently, pushing the 10 ms watermark far into the future so every subsequent execution
   record is released immediately and out of order.

**Fix:** `WriteToAdmin` on both sides, and harden both clients' `default:` into a counted-and-ignored branch
so a future wire addition degrades instead of killing a trading process.

### 4.2 `RiskLimit` restore does not zero the working quantities

C# `Context.cs:932-936` explicitly zeroes `WorstLong/WorstShortWorkingQuantity` before writing a limit
restored from `<symbol>.risklimit`; C++ `Context.hpp:609-611` writes the file's values straight in. A
persisted reservation from a previous session is resurrected as live exposure with no orders behind it.

---

## 5. C#-only capability C++ lacks

Not interop breaks — C++ simply cannot participate. Listed so the gap is a decision rather than a surprise.

| area | C# | C++ |
|---|---|---|
| `MessageEfficiency` struct + shared region | yes | absent |
| `Session` / `SessionManager` | yes | `Instrument::IsInSession()` returns `true` |
| `Target` / `ActiveTarget` / enumerator | yes | absent |
| `InstrumentDetails` / search | yes | absent |
| MBO: `MarketByOrder`, `Order`, `OrderAction`, `TickType` 8–12 | yes | absent |
| `Settlement` struct | yes | absent (though `TickType::Settlement` exists) |
| Series records: `Filld`, `Histogram`, `Candle`, `Factor`, `Mean`, `StdDev` | yes | absent (`FileType` names them) |
| `Position.AlgoStatus` events, `GetOrderId` | yes | absent |
| Client can send a `RiskLimit` on admin | yes | absent |
| Mirror `SharedArray` layer | yes | absent |

Two C++-side dead ends worth deleting or wiring: `Position::IsTradingSuspended` has no reader, and
`AlertManager` compiles but is never constructed, so no C++ process opens the `.alert` socket.

---

## 6. Latent traps

**`MarketByPrice64` is declared outside `#pragma pack(1)`.** Both sides agree today at 664 bytes *only*
because `SideByPrice64` is 320, a multiple of 8, so no padding precedes the first `Timestamp`. Shrink
`_reserved[47]` to 46 and C++ silently inserts 4–7 bytes, shifting all three timestamps and corrupting every
book — with no assertion anywhere to catch it. Move the `pop` below `MarketByPrice64` and add
`static_assert(sizeof(SideByPrice64) == 320)` and `static_assert(sizeof(MarketByPrice64) == 664)`.

**`SideByPrice64::Clear()` drops the `Side` byte on C++ and re-stamps it on C#.** `MapTicksToBestOffset`
multiplies by `(int)Side`, so `Flat(0)` collapses the whole book onto one level. On a freshly created or
zero-attached region C# self-heals on the first snapshot; C++ stays permanently broken and silently reports
a one-level book.

**`ControlType` cannot be JSON-serialised on C++ at all.** `Tools/Json.hpp:18-19` derives every enum's glaze
meta from `magic_enum`, whose default scan range is `[-128, 127]`. `ControlType::AlgoStatus = 200` is out of
range, so `enum_count<ControlType>() == 0` and serialising `ControlType`, `Header<ControlType>` or
`ControlAlgoStatus` is a hard compile error. `AllocateType` (100/101) is in range and fine.

**Series file extension case.** C++ `GetTypeName<T>()` preserves case (`MySeries.Point`); C# uses
`typeof(T).Name.ToLower()` (`MySeries.point`). On a case-sensitive filesystem these never reload each other.

**C++ Provider wire structs carry no size assertions.** C# hard-asserts `ServerHeader == 173` at type-init.
Adding `static_assert(sizeof(...))` to the Provider structs would have caught several of the above.

**Audit types C++ never emits.** `AuditWriter` handles `AllocateClient` (100) and `ControlAlgoStatus` (200);
no C++ component writes either to a socket. And unknown types are still dropped silently on both sides
(`default: break` / `default: return ""`), which is how the `Header<T>` readonly bug stayed invisible.

**`FillType` namespace.** `Execution` on C++, `Data` on C#. Cosmetic, but it makes grep-based porting miss it.
