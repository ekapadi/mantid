# SNSLiveEventDataListener: SPSC Refactor

**Status:** Preliminary draft — for discussion, not implementation
**Target branch:** `mantidproject/mantid` @ `ornl-next`
**Scope:** `Framework/LiveData` — SNS live listener only

> **Verification note.** All line numbers and code facts in this document were
> read from `ornl-next` during drafting. `SNSLiveEventDataListener.{h,cpp}`,
> `ADARAParser.h`, `ADARAPackets.h`, `LiveListener.h`, `ILiveListener.h` and
> `Framework/LiveData/CMakeLists.txt` were byte-identical to `main` at that
> time. Pin this document to a specific SHA before it is used as a work order.

______________________________________________________________________

## 1. Objectives

1. **No Mantid framework calls on the background thread.** Today the socket
   thread calls `WorkspaceFactory::Instance()`, `AlgorithmManager::Instance()`,
   `UnitFactory::Instance()`, `TimeSeriesProperty`, `mutableRun()` and `g_log`
   from inside `rxPacket()`.
1. **Radically simplify the listener state machine.** Eleven pieces of
   cross-thread state, one mutex, and ~40 lines of header comments explaining
   which field is guarded by what.
1. **Preserve behaviour.** No event may be dropped. Existing log message
   wording preserved for anyone grepping.
1. **Make the transition painless for beamline users.** New capability first;
   rewrite of the depended-upon path second.

### Non-goals

- Changes to the ADARA wire protocol or to SMS.
- Changes to `LoadLiveData` / `MonitorLiveData` / `StartLiveData` semantics.
- The ISIS or Kafka listeners.

______________________________________________________________________

## 2. Current architecture

`SNSLiveEventDataListener` is simultaneously `API::LiveListener`,
`Poco::Runnable` and `ADARA::Parser` — one object, two threads, one
`std::mutex`, and a shared `EventWorkspace` (`m_eventBuffer`) that the
background thread writes into and the foreground swaps out.

Every coordination flag exists to schedule access to that one buffer. The
comment at `SNSLiveEventDataListener.cpp:871-880` states the reasoning for
halting network reads at run boundaries:

> 1. We don't need to manage a second buffer in order to keep the events in the
>    just ended run separate from the events in the next run.
> 1. We don't have to deal with the case where the next run has already started
>    but extractData() hasn't been called.
> 1. We don't have to worry about the case where more than one run has started
>    and finished between calls to extractData().

All three are consequences of there being exactly one buffer. The comment then
concedes the cost: if `extractData()` is not called at least once per run,
packets back up and SMS may disconnect the client.

______________________________________________________________________

## 3. Design principles

| Principle                                   | Consequence                                 |
| ------------------------------------------- | ------------------------------------------- |
| Background thread does I/O and framing only | Enforced by the linker, not by review       |
| One ordered stream per concern              | Control records keep ADARA's total order    |
| Events are never dropped                    | Block on capacity; never discard            |
| Bounded memory, lazy allocation             | Capped, recycling block pool                |
| Commit on success                           | Consumer cursor advance is the commit point |

______________________________________________________________________

## 4. Target architecture

Three classes replace one:

| Class                      | Base                | Thread     | Responsibility                              |
| -------------------------- | ------------------- | ---------- | ------------------------------------------- |
| `StreamReader`             | `ADARA::Parser`     | Background | Poll socket, frame packets, route to queues |
| `ChunkAssembler`           | `ADARA::Parser`     | Foreground | Decode control packets, build workspace     |
| `SNSLiveEventDataListener` | `API::LiveListener` | Foreground | Public contract, extract lifecycle          |

The listener is no longer an `ADARA::Parser` and no longer a `Poco::Runnable`.
Both parser roles still exist, as two instances on opposite sides of the queue
boundary.

```
  ┌──────────────────────┐        ┌──────────────────────┐
  │  Background thread   │        │  Foreground thread   │
  │  (no Mantid symbols) │        │  (Mantid framework)  │
  │                      │        │                      │
  │   socket + poll      │        │   ChunkAssembler     │
  │        ↓             │  ═══>  │        ↓             │
  │   StreamReader       │ queues │   Listener           │
  │   (route only)       │        │        ↓             │
  │                      │        │   chunk workspace    │
  └──────────────────────┘        └──────────────────────┘
```

### 4.1 Queue topology

Three queues, not four. Geometry, PV logs and run state share a **single
ordered control queue**, because splitting them would destroy the total order
ADARA gives for free — "did this PV value arrive before or after `NEW_RUN`?"
must remain answerable.

| Queue   | Contents                                      | Rate          | Full policy     |
| ------- | --------------------------------------------- | ------------- | --------------- |
| Control | Raw ADARA packet bytes (incl. 16-byte header) | A few per run | Block           |
| Event   | Pulse headers + `ADARA::Event` arrays         | ~60 packets/s | Block           |
| Log     | POD log records                               | Bursty        | Drop with count |

Control markers carry the **event-sequence watermark** at which they take
effect. This is what restores cross-queue ordering.

### 4.2 Decode split (asymmetric)

The decision is per-queue, not global.

**Events decode on the background thread.** Stripped of Mantid calls the work
is arithmetic:

```cpp
if (bankId < 0xFFFFFFFE)
    tof = corFlag ? event->tof / 10.0
                  : (event->tof + sourceTOFOffset) / 10.0;
```

The producer is already walking the `firstEvent()`/`nextEvent()` cursor to copy
it. Decoding yields a flat POD stream, allows the pause filter to be applied
before anything crosses the boundary, and is the only high-rate path.

**Control packets cross as raw bytes.** Three handlers run
`Poco::XML::DOMParser` — `GeometryPkt` (`:708`), `DeviceDescriptorPkt`
(`:1087`), `RunInfoPkt` (`:1289`). Recursive DOM construction, heavy
allocation, throws on malformed input. Not on the socket thread. Poco is not
Mantid, so this would not violate the letter of Objective 1, but it violates
its intent.

Storing the full packet including its 16-byte header lets `ChunkAssembler` feed
the bytes straight into a second `ADARA::Parser` via `bufferParse()`, so the
existing typed `rxPacket()` bodies move across **near-verbatim**.

### 4.3 Routing

`ADARAParser.h:133` declares `virtual bool rxPacket(const Packet &pkt)` — the
dispatcher itself. `Packet` exposes `base_type()`, `packet()`,
`packet_length()`, `payload()`, `payload_length()`.

`StreamReader` overrides **only** that function. The typed dispatch never runs
on the background thread.

______________________________________________________________________

## 5. Queue implementation

### 5.1 Segmented block chain

Not a flat ring, and not a resizable ring. Resizing in place requires copying
while the consumer may be reading, which is a memory-reclamation problem
needing hazard pointers or epochs — disproportionate machinery.

Instead: a chain of fixed-size blocks.

- Producer writes into the tail block; appends a new block when it fills.
- Consumer drains the head block; returns it to the pool when fully drained
  **and it is not also the tail**.
- Shared mutable state: two indices plus one `next` pointer. One release store,
  one acquire load.
- No copying. No block is freed while the other thread may touch it.

### 5.2 Capped recycling pool

Satisfies "fixed size, but do not allocate up front":

- Producer takes a block from a free list; allocates only if the list is empty
  and block count is below `max_blocks`; blocks if at the cap.
- Consumer returns drained blocks to the free list rather than freeing.
- Steady state performs **zero** allocation.
- Resident footprint = high-water mark. Ceiling = `max_blocks × block_size`.

The free list is itself an SPSC ring running consumer→producer. Its capacity is
exactly `max_blocks`, so it can never overflow — an invariant, not a policy.

**High-water retention.** The pool retains the peak, not current occupancy. If
a pathological burst early in a session leaves memory held for hours, add
trimming — but perform the `free()` on the **consumer** side, which already
blocks on Mantid calls. Never put an unbounded-latency call on the producer.
Recommend a config property defaulted off until profiling justifies it.

### 5.3 Block layout

A block is an **arena**, not a per-pulse container. Pulses pack consecutively:

```
[H][events ×20][H][events ×12][H][events ×24][free]
```

```cpp
struct PulseHeader {
    int64_t  pulseTimeNs;    // ns since EPICS epoch; fg builds the DateAndTime
    double   protonCharge;   // already ×10 corrected
    uint32_t eventCount;
    uint32_t tofOffset;
    uint32_t flags;          // CORflag, continuation bit
};
// immediately followed by eventCount × ADARA::Event (8 bytes each)

static_assert(sizeof(PulseHeader) % alignof(std::max_align_t) == 0);
static_assert(sizeof(ADARA::Event) == 8);
```

Because each header states its own event count, the stream is
**self-describing** — the same pattern ADARA itself uses via `payload_len`. The
consumer walks forward once with no offset arithmetic:

```cpp
const std::byte *cur = blk.data();
const std::byte *end = cur + blk.used();

while (cur < end) {
    PulseHeader h;
    std::memcpy(&h, cur, sizeof h);     // not reinterpret_cast — see below
    cur += sizeof h;
    consumePulse(h, cur, h.eventCount);
    cur += h.eventCount * sizeof(ADARA::Event);
}
```

**Type punning.** `reinterpret_cast<const PulseHeader*>(cur)` is formally
undefined: no object of that type was created there. C++23 provides
`std::start_lifetime_as`; C++20 does not. `memcpy` compiles to the same loads
at `-O2` and stays inside the standard, which matters given §11.

**Splitting.** A pulse larger than a block is split across blocks with the
header duplicated and counts adjusted. Because splitting is always available,
blocks are never closed early: internal fragmentation is at most one
partially-filled block in the whole chain, and block size is decoupled from
pulse size.

> **Correctness hazard.** Proton charge is per-packet, not per-event —
> `pkt.pulseCharge() * 10` is logged once before the event loop
> (`:574-577`). Naive header duplication would log it once per fragment at the
> same timestamp. The `continuation` flag bit exists to suppress this. Proton
> charge is the normalisation divisor; getting this wrong silently corrupts
> science data.

### 5.4 Sizing

`ADARA::Event` is 8 bytes (`uint32 tof`, `uint32 pixel`). The decoded form need
be no larger.

```
budget_bytes ≈ peak_event_rate × 8 × max_extract_interval × safety_factor
```

At 10⁶ events/s and a 10 s update interval: 80 MB ceiling. At 64 KB blocks,
`max_blocks ≈ 1280`; the free-list ring holding those pointers is 10 KB. When
the foreground keeps up, resident footprint is 2–3 blocks (~192 KB).

**Open item:** ORNL peak event rate and shortest update interval in operational
use. These two numbers set `max_blocks`.

> **Why not drain eagerly into the workspace?** `Types::Event::TofEvent` is
> `double m_tof` + `Core::DateAndTime m_pulsetime` = **16 bytes per event**,
> with pulse time stored per event. The queue form is 8 bytes plus one header
> amortised across thousands. Draining early roughly **doubles** memory. The
> queue is the compact representation. This is why the two-thread design beats
> a three-thread design on memory as well as on thread-boundary count.

______________________________________________________________________

## 6. Elimination of `m_pauseNetRead`

Audited: writes at `:813`, `:888`, `:1739`, `:1761`; reads at `:325`, `:915`,
`:1674`.

| Job                                                    | Site          | Fate                                     |
| ------------------------------------------------------ | ------------- | ---------------------------------------- |
| Keep run N+1 events out of run N's buffer              | 813, 888, 325 | **Gone** — markers say this natively     |
| Interrupt `bufferParse()` mid-buffer                   | 915           | **Gone** — nothing needs to stop a parse |
| Protect state being cleared by `onBeginRun`/`onEndRun` | 1739, 1761    | **Gone** — no shared state               |
| Report `ListenerState::ReadWait`                       | 1674          | **Survives**, redefined                  |

`ListenerState::ReadWait` is documented in `ILiveListener.h:33` as *"Connected
but paused at a run boundary (back-pressure)"*. Repoint it at "event ring full,
consumer not keeping up" — closer to what back-pressure should mean, and
operationally informative rather than reporting a routine run boundary.

**Action:** audit `MonitorLiveData` and the test suite for anyone branching on
`ReadWait` before committing to the redefinition.

### 6.1 Knock-on simplifications

- `:915` returning true is what strands bytes inside the parser's buffer, which
  is the entire reason `run()` carries `needParse` and the "skip `poll()`"
  branch at `:341`. With no interrupting return, `bufferParse()` always drains
  fully. `needParse` survives only for the buffer-full case at `:399`.
- `m_bgThreadCaughtUp` goes with `m_pauseNetRead`.
- The refusal at `:877-880` to handle multiple short runs between extracts
  becomes unnecessary — drain to the first `EndRun` marker, return that chunk,
  take the next run on the following call. **Capability gain, not just
  simplification.**

### 6.2 State elimination

| Eliminated                                 | Survives, now single-threaded       |
| ------------------------------------------ | ----------------------------------- |
| `m_pauseNetRead`                           | `m_ignorePackets` (bg-local)        |
| `m_bgThreadCaughtUp`                       | `m_variableMap` (bg-local)          |
| `m_pendingTransition`                      | `m_workspaceInitialized` (fg-local) |
| `m_lastTransition` (as cross-thread state) | `m_stopThread`                      |
| `m_previousExtractCompleted`               | run status (fg-derived)             |
| `m_deferredRunDetailsPkt`                  |                                     |
| `JoiningRun` as *stored* state             |                                     |

`mutable std::mutex m_mutex` disappears. Every atomic disappears except
`m_stopThread` and the ring indices.

> `JoiningRun` remains a value in the public `ILiveListener::RunStatus` enum. It
> can stop being *stored* but may still need to be *reported* — compute it.

______________________________________________________________________

## 7. Call structure

`API::LiveListener::extractData()` is `final` and defines a three-phase
contract: `onBeforeExtract()` → `doExtractData()` → `onAfterExtract()`.

**Both hooks become no-ops for SNS.** They exist because of the ordering
constraint at `:1689-1693` — BeginRun must commit before the snapshot, EndRun
after the harvest. Both are artefacts of the single shared buffer. Once a run
boundary is a marker at a known stream position, there is nothing to sequence.

The hooks remain in the base class for other listeners.

```cpp
std::shared_ptr<Workspace> SNSLiveEventDataListener::doExtractData() {
  // 1. Drain control eagerly.  Safe even if we later throw: the state these
  //    produce (instrument XML, PV names, run details) is foreground-owned
  //    and persists across a NotYet retry.
  const auto boundary = m_assembler.consumeControlUpToMarker();

  // 2. Not enough to build yet.  Nothing committed on the event side.
  if (!m_assembler.workspaceReady())
    throw Exception::NotYet("Waiting for geometry and run details.");

  // 3. Build chunk, appending events up to the watermark so a run
  //    transition never bleeds into this chunk.
  auto chunk = m_assembler.buildChunk(boundary.eventWatermark);

  // 4. Commit.  One release-store per cursor; until here the queues are
  //    untouched from the producer's point of view.
  m_events.commit();
  m_control.commit();

  // 5. Apply the transition, if that is what stopped us.
  if (boundary.transition) {
    m_lastTransition = *boundary.transition;
    m_runStatus = (*boundary.transition == BeginRun) ? Running : NoRun;
  }
  return chunk;
}
```

The retry safety currently requiring `m_previousExtractCompleted` and a
deferred clear across two hooks comes free: **the consumer cursor advance is
the commit point.**

### 7.1 Method-by-method

| Method                        | Today                             | After                                   |
| ----------------------------- | --------------------------------- | --------------------------------------- |
| `onBeforeExtract()`           | 48 lines, two `NotYet` gates      | deleted — base no-op                    |
| `onAfterExtract()`            | 25 lines                          | deleted — base no-op                    |
| `onBeginRun()` / `onEndRun()` | clear 6 caches, re-init           | absorbed into marker handling           |
| `runState()`                  | lock + read                       | plain read                              |
| `isPaused()`                  | atomic load                       | plain read                              |
| `lastTransition()`            | lock + rethrow                    | plain read                              |
| `listenerState()`             | lock + 3 checks                   | plain read                              |
| `run()`                       | ~260 lines                        | moves to `StreamReader`, much shorter   |
| `rxPacket()` ×11 typed        | mutate `m_eventBuffer` under lock | move to `ChunkAssembler`, near-verbatim |

**Action:** confirm that all five getters and `doExtractData()` are never
concurrent. `MonitorLiveData` and `StartLiveData` both touch the listener;
sequential rather than overlapping as far as the call sites read, but
"no mutex at all" is a strong enough claim to warrant walking
`MonitorLiveData.cpp` and `LoadLiveData.cpp` first. If a getter can race an
extract, the fix is a small mutex around scalar reads.

______________________________________________________________________

## 8. Log aggregation

### 8.1 The prolific case is foreground

`appendEvent()` (`:1527`) logs *"Invalid pixel ID"* from the `else` branch of
`m_indexMap.find()`. That map derives from the instrument, so in the new design
both lookup and warning live in `ChunkAssembler` on the **foreground**. Full
C++ available — no POD records, no fixed-size constraints.

The pattern already exists in this class at `:672`:

```cpp
if (!m_badMonitors.contains(monitorID)) {
  m_badMonitors.insert(monitorID);
  g_log.error() << "Event from unknown monitor ID (" << monitorID << ") seen.\n";
}
```

Log-once-per-distinct-ID, implemented for monitors, never extended to detector
pixels, and carrying no counts.

### 8.2 Policy

| Policy                 | Behaviour                                  |
| ---------------------- | ------------------------------------------ |
| Log-once per key       | First sighting logs, rest silent           |
| Count-and-summarise    | First logs, rest counted, summary at flush |
| Distinct-value roll-up | One line per key with its count            |
| Severity de-escalation | First at warning, repeats at debug         |
| Rate limit             | At most N per interval, tail count         |

**Recommended for bad pixels:** combine log-once with roll-up. First occurrence
of each distinct ID logs at warning with today's exact wording; counts
accumulate; one roll-up line emitted per chunk at the end of `doExtractData()`.
Flush cadence then matches the user's update interval.

**Reset semantics.** `m_badMonitors.clear()` currently happens only at
construction (`:239`). The pixel equivalent must clear at run boundaries, or a
bad pixel logs once per session and a user restarting a run wonders why the
warning vanished.

### 8.3 Background-side messages

Socket errors, oversize packets, unknown packet types. Lower volume but capable
of storming — a flapping connection emits one message per ~100 ms poll.

**Dedup at the producer, not the consumer.** A storming message that enters the
queue fills the ring and evicts messages that mattered.

```cpp
struct LogRecord {
    uint8_t  level;
    uint16_t msgId;        // pre-registered; no format strings on bg
    uint32_t suppressed;   // count elided since last emission
    int64_t  timestampNs;
    uint64_t arg0, arg1;
};
```

Producer-side gate keyed by `msgId` holding last-emitted timestamp and a
suppressed count. The count rides along on the next record that does get
through, so nothing is silently lost.

### 8.4 Transition safety

- First-occurrence text stays **byte-identical** to today's, so log-scraping
  scripts keep working and only gain a summary line.
- Aggregation behind `SNSLiveEventDataListener.logAggregation`, default on, so
  a scientist chasing an intermittent fault can restore the firehose without a
  rebuild.

______________________________________________________________________

## 9. PV-only mode

Mechanically one branch in the `StreamReader` router: do not push
`BankedEventPkt` or `BeamMonitorPkt` to the event queue.

The consequence is disproportionate to the change. In this mode there is **no
event queue, no block pool, no byte budget, no watermark, and no "cannot drop
events" constraint.** Every hard problem in §5 is bypassed. Memory becomes a
few KB/s of PV traffic.

**Workspace:** reuse what `initWorkspacePart2()` already produces before any
events arrive — an `EventWorkspace` with instrument loaded and `Run` logs
populated, zero events. No new construction path. `buffersEvents()` returns
false.

A lighter variant skipping `LoadInstrument` is possible if metadata-poll
latency matters, but note `m_requiredLogs` derives from parsing the IDF for
parameter elements, so skipping geometry also loses readiness gating. Start
with the full version.

**Plumbing already exists.** `ILiveListener` derives from
`Kernel::PropertyManager`; `StartLiveData` has `copyListenerProperties()` and
`removeListenerProperties()` (`StartLiveData.h:44-45`). A `MetadataOnly`
boolean declared on the listener surfaces in the `StartLiveData` dialog
automatically.

**Open question:** does `GetLiveMetadata` need to be a new algorithm, or is it
`StartLiveData` with `MetadataOnly=True`? A property costs nothing but is
undiscoverable. A thin wrapping algorithm buys discoverability for a few dozen
lines and probably serves users better.

**Protocol caveat.** The client hello at `:289-293` carries a TODO: *"The
packet version should be bumped to 1 and we should add the extra flags field."*
There is currently no way to tell SMS "do not send events" — they arrive and
are discarded on the background thread. Everything downstream is saved;
network bandwidth is not. Reducing that requires SMS-side coordination.

______________________________________________________________________

## 10. Build enforcement

Objective 1 must be enforced mechanically. Review is insufficient because of:

```cmake
target_precompile_headers(LiveData PRIVATE <MantidAPI/Algorithm.h>)
```

Every TU in the `LiveData` target receives `MantidAPI/Algorithm.h` whether it
includes it or not. A stray `WorkspaceFactory::Instance()` in a "background"
`.cpp` inside `LiveData` would compile silently.

**A separate CMake target is therefore not a hardening measure — it is the only
mechanism that works.** `LiveData` links `Mantid::Kernel API DataObjects Geometry NexusGeometry` as `PUBLIC`; a new target linking none of them turns a
violation into a link error.

### 10.1 The move is small

`ADARAParser.h` and `ADARAPackets.h` include only `<cstdint>`, `<map>`,
`<stdexcept>`, `<string>`, `<sstream>`, `<cstring>`, `<vector>`, `<time.h>`,
plus `ADARA.h` and `MantidLiveData/DllConfig.h` — and `DllConfig.h` does not
exist in the repository, being generated at build time by
`generate_mantid_export_header(LiveData FALSE)`. It is export macros only.

**The ADARA substrate has no Mantid dependency today.** All contamination is in
`SNSLiveEventDataListener.cpp`'s `rxPacket()` overrides.

New target `SNSStreamParser` contains:

- `ADARA.h`, `ADARAPackets.{h,cpp}`, `ADARAParser.{h,cpp}` (moved)
- `StreamReader.{h,cpp}` (new)
- Queue and block-pool sources (new)
- Its own generated export header

`LiveData` then links `SNSStreamParser`.

### 10.2 Config hoisting

`m_keepPausedEvents` is read from `ConfigService`. It must be read on the
foreground at construction and passed to `StreamReader` as a plain value.

Similarly `timeFromPacket()` returns `Mantid::Types::Core::DateAndTime`. The
background thread emits a raw `int64_t` nanosecond count; the foreground
constructs the `DateAndTime`.

______________________________________________________________________

## 11. Verification

### 11.1 Existing harness

Substantial and already present:

- `SNSLiveEventDataListenerTest.h` — 1930 lines, in-process `MockSMSServer`
  over Unix-domain sockets
- `SNSLiveEventDataListenerNoNetworkTest.h`
- `SNSLiveEventDataListenerAlgorithmIntegrationTest.h`
- `SNSLiveEventDataListenerLegacyTest.h` — deliberately unregistered,
  "retained as historical reference"

This class has been through a recent correctness campaign — the header
references a "C1 fix", explains why `m_lastTransition` must survive `NotYet`
retries, and documents the BeginRun-collapse case where a run starts and ends
inside the joining window. **The refactor deletes the mechanisms implementing
those fixes; the tests are the only artefact carrying the requirements
forward.** Re-register the legacy test against the new implementation rather
than treating it as historical.

> **Platform caveat.** The integration tests are excluded on Windows because
> cxxtestgen's regex parser does not honour the `#ifndef _WIN32` guards inside
> them. A green Windows build is not coverage.

### 11.2 SPSC verification

The queue is small enough for exhaustive model checking rather than sampling.

- **GenMC** — compiles to LLVM IR and exhaustively explores every execution
  permitted by RC11. For a bounded harness ("producer pushes 4 records into a
  capacity-2 queue, consumer pops 4") it either reports a violating trace or
  certifies the harness clean.
- **ThreadSanitizer** — understands C++ atomics, so no false positives on
  correct lock-free code. Finds bugs; never proves absence. Smoke test only.
- Assert FIFO order, no loss, no duplication, and the free-list
  never-overflows invariant from §5.2.

**Residual, to state honestly in review:** GenMC verifies against RC11, not the
ISO text; you verify source, not the compiled binary; the harness bound is an
argument you make, not one the tool makes.

### 11.3 Endpoint handles

SPSC's contract is exactly one producer and one consumer. No implementation can
enforce that. Push it into the type system:

```cpp
auto [producer, consumer] = EventQueue::create();
```

Move-only, non-copyable handles from a factory; `Producer` exposes only `push`,
`Consumer` only `pop`. "Two producers" then requires an explicit move, which is
greppable, rather than being an invisible property of the call graph.

______________________________________________________________________

## 12. Delivery plan

### Phase 0 — Extract `SNSStreamParser` (no behaviour change)

Move the ADARA files into a Mantid-free target. Same virtuals, same call sites,
listener unchanged. Existing test suite passes untouched. Lands the enforced
boundary while the diff is still trivially reviewable.

### Phase 1 — PV-only mode + `GetLiveMetadata`

Exercises router, control queue, `ChunkAssembler` and the new
`doExtractData()` end-to-end **without** the event queue, block pool,
watermarks or split-record logic. Roughly a third of the risk, and it ships
while the event path continues on existing code.

Users gain a capability rather than absorbing a rewrite of one they depend on.

### Phase 2 — Event queue

Block pool, watermarks, split records, proton-charge continuation flag.
Verification harness from §11.2.

### Phase 3 — Retire the old path

Delete `m_pauseNetRead` and dependents. Re-register the legacy test.

______________________________________________________________________

## 13. Open items

| #   | Item                                                              | Owner | Blocks           |
| --- | ----------------------------------------------------------------- | ----- | ---------------- |
| 1   | ORNL peak event rate + shortest operational update interval       | ORNL  | §5.4 sizing      |
| 2   | Audit `MonitorLiveData` for `ReadWait` consumers                  | —     | §6               |
| 3   | Confirm getters never race `doExtractData()`                      | —     | §7 mutex removal |
| 4   | `GetLiveMetadata`: new algorithm or property?                     | —     | Phase 1          |
| 5   | Monitor events: share event queue with a source tag, or separate? | —     | Phase 2          |
| 6   | Do pixel IDs within a bank map to contiguous workspace indices?   | —     | §14 optimisation |
| 7   | Pin this document to a commit SHA                                 | —     | All              |

______________________________________________________________________

## 14. Deferred

**Bank-grouped index lookup.** The consumer's dominant cost is a per-event
`m_indexMap` hash lookup in `appendEvent()`. ADARA delivers events in
contiguous runs by bank (`curBankId()` changes at run boundaries during
iteration). Preserving that grouping would allow one lookup per bank instead of
per event — but only if pixel IDs within a bank map to contiguous workspace
indices. Plausible for typical SNS IDFs; must be verified before relying on it.

**Third drain thread.** Considered and rejected. The memory argument was
inverted: `TofEvent` is 16 bytes against 8 on the wire, so draining eagerly
into the workspace roughly doubles memory. Only a latency-smoothing argument
remains, which does not justify reintroducing Mantid calls off the foreground
thread. Recorded here so it is not re-proposed.
