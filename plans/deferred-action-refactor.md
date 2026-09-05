# Component 2 — deferred-action refactor and `m_pauseNetRead` exception safety

> **Status: design / discussion. Not ready to implement.**
> Part 2 of 3. Companion documents: [`first-pass.md`](first-pass.md) (investigation history and
> evidence), [`keepalive-watchdog.md`](keepalive-watchdog.md) (part 1 — detection),
> [`hazard-probes.md`](hazard-probes.md) (part 3 — tests that reproduce these hazards; its
> probes are the acceptance criteria for this refactor).
>
> Part 1 *detects*. This part *prevents*. They are independent; part 1 is worth shipping first
> because it works regardless of whether the hypothesis below is correct.

______________________________________________________________________

## Story for EWM use

*Plain prose, intended to be pasted directly into an EWM work item. Everything below this section
is implementation detail and does not need to go into EWM.*

The SNS live event data listener protects its state with a single mutex, which its background
network thread holds for the duration of each parse cycle — a deliberate simplification that
succeeded on its own terms. However, the work inside that critical section is not limited to the
listener's own state: while holding the lock, the background thread runs a full Mantid algorithm
to construct instrument geometry, calls framework singletons to create workspaces, and writes to
the logging framework from roughly thirty points in the packet handlers. None of those calls
guarantees when it will return. If one of them does not, the background thread holds the lock
indefinitely and the foreground thread blocks on it forever, with neither reaching a point where
it could raise an exception or write a log message. The failure is therefore completely silent,
which matches the observed production incident exactly. This defect is worth removing regardless
of whether it caused that incident, because holding a lock across an operation of unbounded
duration is a fault in its own right.

A second, more general concern affects the same code. The listener creates its own background
thread, which no other part of the system knows exists, whereas every other thread running Mantid
framework code is one the framework created or expects. Reviewing the components the listener
touches from that thread, several — configuration access, the workspace factory, and the algorithm
factory — carry no thread synchronisation at all, being written on an assumption of controlled
access. Others are synchronised but dispatch notifications onto whichever thread called them. The
logging framework holds an internal lock while writing and, when configured to route output
through Python as a service deployment may well be, additionally acquires the Python global
interpreter lock; that combination admits a deadlock between the two which would be entirely
invisible, because it would occur inside the logging system and no diagnostic could ever be
written. The background thread also writes directly into a framework workspace object on every
event received.

This story restructures the listener so these situations cannot arise. Packet handlers become pure
state transitions that may modify the listener's own state but may not call framework code at all;
work requiring the framework is described as a value and carried out afterwards. The stronger form
of the change confines all framework interaction to the foreground thread, so events accumulate in
plain in-memory buffers and are converted into framework objects only when the foreground collects
them. The existing parser already supports interrupting and resuming a parse, and already uses
that facility, so this restructures existing control flow rather than rewriting it. Critically, the
restriction will be enforced by the compiler rather than by convention: background-thread code
moves into a scope from which framework facilities are not visible, so a future change
reintroducing the problem fails to build instead of silently reintroducing a production hang.

Two smaller related items are included. The first is a defensive change so the foreground thread
waits for the lock with a timeout rather than indefinitely, reporting a clear diagnostic error if
it expires. This does not prevent a stall, but converts a silent, unattributable freeze into a
logged and attributable failure — a small, low-risk change deliverable independently and early.
The second is a separate correctness defect found during this investigation: one of the listener's
flow-control flags is cleared only on the successful path through data extraction, so an exception
at the wrong moment leaves the background thread permanently paused. That is not the cause of the
observed failure, since it requires an exception we do not see, but it is a genuine latent fault of
the same family and is cheap to correct while this area is open.

______________________________________________________________________

## 1. The invariant

> **`m_mutex` is never held across a call that can block unboundedly.**

"Unbounded" means anything outside the listener's own state: algorithm execution, singleton
access, logging, framework object allocation, filesystem I/O.

The leading hypothesis ([`first-pass.md`](first-pass.md) §3) is that this invariant is violated
today and that the violation is the hang: the background thread holds `m_mutex` across
`LoadInstrument`, which blocks and never returns, and the foreground blocks on `m_mutex` at
`:1597`. Silent, permanent, and consistent with every observed symptom.

**The refactor is worth doing even if that hypothesis is wrong.** Holding a lock across
unbounded work is a defect on its own terms, and it is the shape of bug that produces exactly the
"frozen, nothing in the log" signature that has made this so hard to chase.

______________________________________________________________________

## 2. Current violations

| Call                           | Sites                                                                                                                      | Reaches                                                                                   |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `loadInst->execute()`          | `:1416`, via `initWorkspacePart2()` — reached from seven `rxPacket()` sites (`:541, :738, :761, :911, :972, :1016, :1063`) | AlgorithmManager, InstrumentDataService, XML SAX parsing, **VTP geometry-cache disk I/O** |
| `WorkspaceFactory::Instance()` | `:1372`, `:1437`, `:1492`, `:1493`                                                                                         | singleton with internal locking                                                           |
| `g_log.*`                      | ~30 sites inside `rxPacket()` bodies                                                                                       | Poco logging and channel dispatch                                                         |

The parse lock is acquired once in `run()` at `:445` and held for the entire parse iteration
(`:427-445`), by deliberate design — the header states the contract at
`SNSLiveEventDataListener.h:266-269`. That design decision is what makes every one of the calls
above run under the lock.

Note the disk I/O is the **VTP geometry cache**, not the IDF directory: the listener supplies
`InstrumentXML` from the ADARA `GeometryPkt`, and `LoadInstrument.cpp:101-115` short-circuits the
file-resolution path entirely. See [`first-pass.md`](first-pass.md) §3.

______________________________________________________________________

## 2b. Framework thread-safety analysis

The hypothesis: the framework may simply not be thread-safe with respect to the listener's
background thread — a thread **no other component knows exists**. Every other thread running
framework code (main, algorithm-async, TBB pool) is one the framework created or expects.

Survey of what the listener actually touches:

| Component                                                  | Synchronization                                                                         | Where the bg thread reaches it                     |
| ---------------------------------------------------------- | --------------------------------------------------------------------------------------- | -------------------------------------------------- |
| `AlgorithmManager`                                         | mutex present                                                                           | `createUnmanaged("LoadInstrument")` `:1401`        |
| `DataService<T>` (ADS, `InstrumentDataService`)            | `std::recursive_mutex` **+ synchronous Poco notifications**                             | inside `LoadInstrument` (instrument caching)       |
| `ConfigService` — property store                           | **yes**, via Poco (`AbstractConfiguration.h:535`)                                       | `getVTPFileDirectory()`, `getTempDir()` → **safe** |
| `ConfigService` — its own caches                           | **none**                                                                                | **not reached** by the listener — see below        |
| `WorkspaceFactory` / `AlgorithmFactory` / `DynamicFactory` | **none** (`DynamicFactory.h`, no locking)                                               | `:1401`, `:1372`, `:1437`, `:1492`, `:1493`        |
| `Algorithm::execute()`                                     | posts notifications **synchronously on the calling thread** (`:246`, `:549`, `:576`, …) | `:1416`                                            |
| Logging (`Poco::SplitterChannel`)                          | `FastMutex` **+ GIL if a Python channel is configured**                                 | ~30 `g_log` sites, incl. the hot path              |
| `EventWorkspace`                                           | MRU cache, `Run`, `TimeSeriesProperty`                                                  | **every event**, `appendEvent()` `:1538`           |

**The hypothesis is partly supported — and narrower than first stated.** Corrected after detailed
analysis ([`hazard-probes.md`](hazard-probes.md) §4):

- **`ConfigService` reads on the listener's path are safe.** The Poco property store beneath
  `getValue()`/`getString()` carries its own mutex. Only Mantid's *cached* members
  (`m_instrumentDirs`, `m_dataSearchDirs`, `m_proxyInfo`) are unsynchronised, and the listener does
  not touch them — it reaches ConfigService only through `getVTPFileDirectory()` and
  `getTempDir()`, and cannot reach instrument-directory resolution at all because `InstrumentXML`
  short-circuits it.
- **`DynamicFactory` genuinely has no locking**, and does underlie both factories the background
  thread calls. But its race needs concurrent `subscribe`/`unsubscribe` against a lookup, and
  registration is a startup activity — so it needs an unusual trigger, not steady-state operation.
- `DataService`, `AlgorithmManager` and `Poco::NotificationCenter` are all internally
  synchronised.

**Net effect on this document's argument:** the "unknown thread" concern is real but does *not* by
itself explain the hang, because none of the unsynchronised state is demonstrably exercised
unsafely by the listener's steady-state access pattern. §2b.1 below does not depend on any of
this — it is a lock-order inversion between two components that are each individually correct,
which is why it remains the strongest candidate.

Two mechanisms deserve naming.

### 2b.1 Logging: a real exposure, but the cycle is NOT established

> **Correction.** An earlier revision claimed a confirmed deadlock cycle. **Retracted.**
>
> - **Confirmed:** `livereduce.py:17,29` calls `mantid.utils.logging.log_to_python("information")`,
>   installing `PythonLoggingChannel`. So the background thread *does* acquire the GIL inside the
>   Poco channel lock on every log call. Real exposure.
> - **Retracted:** the cycle GIL ↔ Python logging handler lock. Measured — CPython **releases** the
>   GIL while blocking on a `threading.Lock`, so the chain unwinds. Two threads logging
>   concurrently is **not** sufficient.
> - **Open:** a deadlock requires a thread holding the GIL that blocks on a **C-level** lock.
>   `Logger.cpp:36-61` releases the GIL on every Python→C++ logging call, closing the obvious
>   route. No production path supplying that half has been found.
>
> See [`hazard-probes.md`](hazard-probes.md) §3.0 for the measurement and what survives.

### 2b.1 Logging: `SplitterChannel` mutex ↔ GIL inversion

`Mantid.properties.template:167` configures the root logger as a `SplitterChannel`, which holds a
`FastMutex` (`SplitterChannel.h:73`) while forwarding to its sub-channels. If the console channel
is a **Python-backed** channel, `log()` then acquires the GIL —
`PythonStdoutChannel.cpp:28` and `PythonLoggingChannel.cpp:50,59,72`, via
`GlobalInterpreterLock` → `PyGILState_Ensure()`.

That produces a genuine two-lock inversion:

- **Background thread:** holds `m_mutex` → `g_log` → takes `SplitterChannel::_mutex` → wants the **GIL**.
- **Any GIL-holding thread:** holds the **GIL** → `g_log` → wants `SplitterChannel::_mutex`.

Deadlock. The background thread then holds `m_mutex` forever, so the foreground blocks on it at
`:1597` and the whole listener stops.

**This mechanism is self-concealing, which is why it fits the silence so exactly: the deadlock is
inside the logging system, so nothing can ever be logged about it.** Any attempt to report the
problem blocks on the same mutex. It also matches "rare" — it needs concurrent logging from a
GIL-holding thread and the listener's background thread — and "production only", since C++ unit
tests use `StdoutChannel` with no GIL anywhere.

**Condition to check:** the default installed configuration is `StdoutChannel`
(`Framework/Kernel/CMakeLists.txt:661`), pure C++ and GIL-free. A Python channel must be
**explicitly configured**. So: does the live-reduce deployment set
`logging.channels.consoleChannel.class` to `PythonStdoutChannel` or `PythonLoggingChannel`? For a
systemd unit wanting Mantid's log lines interleaved with the script's own output in the journal,
that is a very natural thing to have configured. Note also the acknowledged weakness at
`PythonStdoutChannel.h:37`: *"this channel should replace PythonStdoutChannel when we adopt
pybind11 because of robust GIL management."*

Worth noting that `appendEvent()` logs a warning on an invalid pixel ID **inside the hot path**
(`:1540`), while holding `m_mutex`. With a Python channel that is one GIL acquisition per bad
event — a burst of invalid pixel IDs would be an excellent trigger for an otherwise rare race.

### 2b.2 Unsynchronized state: corruption, not blocking

Where unsynchronised state *is* raced — `DynamicFactory`'s registry, or ConfigService's caches in
callers other than the listener — the failure mode differs from a deadlock: a corrupted
`std::map` or red-black tree traversal **spins** rather than blocks.

**That gives a free discriminator.** A wedged process where the stuck thread is in state `R`
burning CPU indicates data-structure corruption; state `S` at 0% CPU indicates a lock or GIL
block; state `D` indicates a kernel-level block. The watchdog already reads
`/proc/<pid>/task/*/status` — adding `utime`/`stime` from `.../stat` distinguishes all three at
zero cost. See [`keepalive-watchdog.md`](keepalive-watchdog.md) §5.5.

### 2b.3 Consequence for the design

**This invalidates Variant A on its own.** Deferring framework calls to *outside the lock* removes
the lock-held-across-unbounded-work defect, but the call still happens **on the background
thread** — so every hazard in the table above survives untouched. If the hypothesis holds, §3
alone does not fix the bug. See §3b.

______________________________________________________________________

## 3. Design Variant A: deferred actions, executed unlocked

`rxPacket()` handlers become **pure state transitions**. They may mutate listener state; they may
not call framework code. Work needing the framework is *described* as a value and returned;
`run()` executes the description after releasing the lock.

```cpp
DeferredBatch batch;
{
  std::lock_guard<std::timed_mutex> lock(m_mutex);
  batch = bufferParse();     // pure state work only — no framework calls
}                            // <-- lock released here
executeDeferred(batch);      // ALL framework calls happen here, unlocked
```

### Why this is tractable rather than a rewrite

The parser already supports interrupt-and-resume, and the listener already uses it:

- `rxPacket()` returning `true` **interrupts the parse** — `:455` notes `rxPacket(RunStatusPkt)`
  does exactly this today.
- `run()` already handles resuming a partially-consumed buffer (`:313-316`), including the case
  where bytes are stranded in the parser's internal buffer where `poll()` cannot see them.

So a handler needing framework work sets a deferred-action flag and returns `true`; `run()`
unlocks, performs the work, relocks, and resumes. Crucially this preserves ordering: because the
parse resumes only *after* the deferred work completes, later packets in the same buffer still
observe an initialised workspace. That is the property that would otherwise make deferral unsafe.

### Deferred logging

The ~30 `g_log` calls cannot simply be deleted. Handlers instead append to a fixed-capacity,
preallocated ring of small messages; `run()` flushes it after unlocking. Worth doing on its own
terms — logging is a framework call with its own locking and dispatch, and it currently sits
inside the critical section on every packet path.

### Variant A is necessary but not sufficient

It removes the lock-held-across-unbounded-work defect. It does **not** change which thread makes
the call, so every hazard in §2b survives. Treat A as a stepping stone to B, not an alternative.

______________________________________________________________________

## 3b. Design Variant B: framework calls only on the foreground thread

**The stronger variant, and the one that actually addresses §2b.** The rule becomes not merely
"not under the lock" but:

> **The background thread never touches the framework at all.** It does socket I/O and pure
> parsing into listener-owned plain data. Every framework interaction is performed by the
> foreground thread inside `extractData()`.

### What this requires

The background thread's framework contact is not occasional, so this is a real change:

| Today, on the bg thread                                                                     | Under Variant B                                                       |
| ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| `appendEvent()` → `m_eventBuffer->getSpectrum(i).addEventQuickly()` **per event** (`:1538`) | append to a plain staging buffer: `vector<{pixelId, tof, pulseTime}>` |
| `initWorkspacePart2()` → `LoadInstrument` (`:1416`)                                         | queue a request; foreground runs it                                   |
| `WorkspaceFactory::Instance()` (`:1372`, `:1437`, `:1492`)                                  | foreground only                                                       |
| `g_log.*` × ~30, incl. hot path (`:1540`)                                                   | append to a plain message ring; foreground flushes                    |
| device/log values into `Run`/`TimeSeriesProperty`                                           | plain typed staging, converted by the foreground                      |

The consequence worth being explicit about: **`m_eventBuffer` can no longer be an `EventWorkspace`
written by the background thread.** Events accumulate in POD staging storage and the foreground
converts them into the workspace during `extractData()`. That is the core of the change and the
main source of its risk, since it touches the hot data path.

### Why it is worth it

- The framework is only ever entered from a thread it already expects to run algorithms on.
  The listener's own `Poco::Thread` — invisible to the framework, to Poco's logging, and to
  Python — never enters framework code.
- **The GIL inversion in §2b.1 disappears**, because the background thread never logs and so never
  acquires the GIL.
- The unsynchronized singletons in §2b.2 are no longer accessed concurrently *by the listener*.
- Serialization becomes structural rather than lock-based: framework work happens at one point in
  one thread, so there is nothing to interleave.
- A staging-buffer append is cheaper than an `EventWorkspace` spectrum append, so the hot path
  likely gets faster.

### Costs and open risks

- **Larger change** than A, on the data path rather than around it.
- **Work shifts to `extractData()`**: the foreground now populates the workspace. Net work is
  unchanged, but latency moves. The existing `startupTimeout` wait loop (`:1556-1578`) already
  anticipates initialisation not being complete at extract time.
- **Deferred instrument initialisation** changes when `LoadInstrument` runs relative to packet
  arrival. Ordering is preserved by the interrupt-and-resume mechanism in §3, but the seven
  `initWorkspacePart2()` call sites each need checking.
- **"Foreground" is not the Python main thread** — it is `MonitorLiveData`'s async algorithm
  thread. That is a thread the framework routinely runs algorithms on, so it is a supported
  configuration, but it does not by itself guarantee a Python thread state exists. Variant B
  removes the *listener's* contribution to concurrent framework access; it does not make the
  framework thread-safe in general.
- Memory profile is broadly unchanged — `m_eventBuffer` already buffers until extract.

### Relationship to Variant A

B subsumes A: if the framework is only entered from the foreground, nothing framework-related is
under the parse lock by construction. The deferral machinery in §3 (interrupt-and-resume, the
action batch, the log ring) is exactly the mechanism B needs — B simply has the **foreground**
drain the batch instead of the background thread. So A's implementation work is not wasted; A is
the first half of B.

**Recommendation:** build the deferral machinery from §3, but drain it on the foreground thread
from the outset. Adopt the enforcement in §4 against the stronger rule — *no framework symbol
visible to background-thread code at all* — which is both a cleaner boundary and easier to check
than "not while locked".

______________________________________________________________________

## 4. Enforcement — "impossible", not "absent"

A convention that framework calls stay outside the lock will decay under maintenance. Three
mechanisms, increasing in strength; recommend **2 + 1**.

1. **Runtime assertion (backstop).** Wrap the mutex in a type recording the owning thread; a
   `FrameworkCallScope` RAII asserts the current thread does *not* own it. Cheap, and converts any
   regression into a hard CI failure rather than a once-a-year field wedge.

1. **Visibility-based enforcement (primary).** Move the packet handlers into a class or TU that
   *cannot name* the framework — no `AlgorithmManager.h`, no `WorkspaceFactory.h`, no `Logger`.
   Handlers receive a `ParseContext&` exposing only guarded state plus the deferred-action and
   deferred-log sinks. The compiler then enforces the invariant: the violating call does not
   compile, because the symbol is not visible. Enforcement by what is *includable*, not by
   discipline.

1. **Static check.** A clang-tidy or grep-based CI rule flagging framework symbols within the
   parse-lock region. Blunt, but cheap, and catches the obvious regression if 2 is only partly
   adopted.

______________________________________________________________________

## 5. Independent and cheap: stop the foreground dying silently

Separable from §3, a few lines, no architectural risk. **Worth shipping ahead of the refactor.**

Change `m_mutex` to `std::timed_mutex`; replace the `std::lock_guard` in `doExtractData()`
(`:1597`) with `try_lock_for(N)`. On timeout, throw a specific, loud error naming the condition
and including the current predicate values — *"background thread has held the parse lock for N s;
it appears wedged"*.

This does not prevent a wedge. It converts the observed symptom — silent permanent freeze with
**nothing in the log** — into a visible, logged, diagnosable failure. Given that silence is the
defining characteristic of this bug, this alone would have made it tractable long ago.

Apply the same treatment to the destructor's join path (`:141`), so shutdown reports a wedge
rather than hanging on it.

**Interaction with part 1:** the thrown error and the heartbeat `fgPhase` are complementary — the
exception makes it visible in the unit's own log, the heartbeat makes it visible to the watchdog
even if the foreground never gets far enough to throw.

______________________________________________________________________

## 6. Separate defect: `m_pauseNetRead` exception safety

Latent, and **not** the observed hang — it requires a throw, and no throw is observed
([`first-pass.md`](first-pass.md) §4, §5). Worth fixing on its own merits.

`Framework/API/src/LiveListener.cpp:16-21` has no try/catch and no RAII guard:

```cpp
std::shared_ptr<Workspace> LiveListener::extractData() {
  onBeforeExtract();
  auto workspace = doExtractData();   // if this throws...
  onAfterExtract();                   // ...this never runs
  return workspace;
}
```

`m_pauseNetRead` is set at `:813` / `:888` and cleared at exactly two sites, asymmetrically:

| Edge     | Clear site                      | Runs                       | Throw-safe? |
| -------- | ------------------------------- | -------------------------- | ----------- |
| BeginRun | `:1739`, in `onBeforeExtract()` | *before* `doExtractData()` | yes         |
| EndRun   | `:1761`, in `onAfterExtract()`  | *after* `doExtractData()`  | **no**      |

A throw from `doExtractData()` with an EndRun pending therefore strands `m_pauseNetRead = true`,
parking the background thread in the unbounded wait at `:325`.

### Fix options

- **Base class (thorough).** An RAII guard in `LiveListener::extractData()` so the post-hook always
  runs, or a distinct `onExtractFailed()` for the throw path. **Blast radius: every listener** —
  ISIS, Kafka, and others. Changes a shared template method, so it needs owner agreement beyond
  this class.
- **Listener-local (narrow).** Clear the EndRun pause on the same side as BeginRun, making the two
  edges symmetric and throw-safe without touching shared code.

Recommend the narrow fix first — it removes the asymmetry that makes the bug possible, with no
cross-listener risk. The base-class question is worth raising separately, since the missing
guarantee is arguably a defect in the template method's contract.

### Also worth bounding

The `:325` wait is the only unbounded wait in the file. Even with the asymmetry fixed, giving it a
timeout that logs the predicate values on expiry would prevent any *future* stranding bug from
being silent.

______________________________________________________________________

## 6b. Configuration keys

**Every option is a `Mantid.properties` key**, following the existing convention at
`Framework/Properties/Mantid.properties.template:127-130` — `SNSLiveEventDataListener.` prefix,
camelCase leaf, `=` spacing, durations as bare seconds. To be added after the existing
`SNSLiveEventDataListener.testAddress` entry (and after Component 1's `keepAlive.*` block).

```properties
# Seconds the foreground thread will wait for the parse lock in extractData()
# before giving up and raising a diagnostic error naming the condition.  This
# does not prevent a background-thread stall; it stops the foreground blocking
# on one silently and forever.  0 restores the previous unbounded wait.
SNSLiveEventDataListener.lockTimeout = 30
# Seconds the background thread will remain in the back-pressure pause loop
# waiting for the foreground to extract, before logging the full predicate state.
# Guards against a flow-control flag being stranded set.  0 means unbounded.
SNSLiveEventDataListener.pauseWaitTimeout = 300
```

Notes:

- **`lockTimeout`** is the §5 change. `0` meaning "wait indefinitely" is an operational escape
  hatch, not a design switch: if the timeout ever fires spuriously under a legitimate slow path it
  can be neutralised in the field without a rebuild.
- **`pauseWaitTimeout`** bounds the only unbounded wait in the file (`:325`). Even with §6's
  asymmetry fixed, this stops any *future* stranding bug from being silent.

Both are tunable thresholds. Neither selects between designs.

### Deliberately not configurable

**No key selects between Variant A and Variant B, and none toggles deferred logging.** Variant B
is simply implemented; the old behaviour is not preserved behind a switch.

An earlier revision of this document proposed a `deferLogging` key as a live A/B test of the
§2b.1 GIL hypothesis. That was wrong on its own terms: under Variant B the background thread
**cannot** log, because §4's visibility enforcement removes `Logger` from its scope entirely. A
runtime toggle would require keeping the pre-refactor code path alive and reachable, which
defeats the compile-time enforcement that makes the fix durable — and would double the concurrency
design surface that has to be tested.

**Verification is by comparison against the pre-implementation branch**, not by a runtime flag:
build and run both, and compare behaviour and instrumentation output. That gives a cleaner
comparison than a flag ever could — the two builds differ in exactly the intended way, with no
dead path carried into production.

______________________________________________________________________

## 7. Suggested order

0. **Check the log-channel configuration** (§2b.1). If the deployment routes Mantid logging
   through a Python channel, that alone is a candidate root cause and is worth confirming before
   anything is built — it costs one look at the deployed properties. A temporary switch to
   `StdoutChannel` would also be a *testable mitigation*, not just a diagnostic.
1. **§5** — timed lock in `doExtractData()`. Few lines, no architectural risk, directly fixes
   "silent". Ship first.
1. **§6** — the narrow `m_pauseNetRead` symmetry fix, plus a bound on the `:325` wait.
1. **Deferred logging** (§3, log ring only) — as the **first increment of Variant B**, not as a
   toggle. If §2b.1 is the mechanism, removing `g_log` from the background thread fixes it
   outright, and it is far smaller than the full refactor while being a strict subset of it.
   Highest value per line in this document, and nothing done here is discarded by step 4.
1. **§3b + §4** — the remainder of the foreground-only refactor and its enforcement. The real fix;
   largest change; best done with the above and part 1 already in place so any regression is
   visible.

Steps 3 and 4 are increments toward the same end state, so there is no intermediate design to
maintain: after step 3 the background thread no longer logs, and after step 4 it no longer touches
the framework at all.

______________________________________________________________________

## 7b. Verification

Since no runtime flag is carried, correctness is established by **comparing the refactored branch
against the pre-implementation branch on identical input**.

**Deterministic replay.** The ADARA tooling at
`Framework/PythonInterface/mantid/LiveData/ADARA/utils/packet_playback/` (`packet_player.py`,
`session_server.py`) replays a recorded packet stream, so both builds can consume byte-identical
input. This is what makes the comparison meaningful rather than anecdotal.

**What to compare across the two builds:**

- Extracted workspace equivalence — event counts per spectrum, pulse times, run logs, and the
  run-state transition sequence reported by `lastTransition()` / `runState()`.
- Log output: same messages, same order relative to run boundaries. Deferred logging changes *when*
  a message is emitted relative to the parse, so ordering differences here are expected and need
  reviewing rather than merely diffing — see §8 question 8.
- Timing: parse-iteration and `extractData()` durations. Variant B moves workspace population from
  the background to the foreground, so a shift is expected; the check is that total throughput does
  not regress.

**Tests.** The existing fixtures (`SNSLiveEventDataListenerTest.h`,
`SNSLiveEventDataListenerNoNetworkTest.h`, `SNSLiveEventDataListenerLegacyTest.h`,
`SNSLiveEventDataListenerAlgorithmIntegrationTest.h`) already subclass the listener and inject
state, and should pass unchanged — they are the regression net for the refactor.

**Enforcement is itself testable.** §4's thread-ownership assertion turns the invariant into a
test rather than a review comment: a fixture that drives a full parse cycle and asserts no
framework call occurred on the background thread will fail loudly if the property is ever lost.
Worth adding as part of step 4 rather than afterwards.

**ThreadSanitizer** (`CMakePresets.json:210`, `linux-64-ci-thread-sanitiser`) is worth running over
the LiveData suite here — not for the wedge, which it cannot see
([`first-pass.md`](first-pass.md) §6B), but because Variant B changes which thread touches shared
state, and races are exactly what TSan is for.

**Keep-alive as an observability aid.** If Component 1 is already deployed, its phase field and
trace ring give a cheap way to confirm during verification that the background thread's phases no
longer include `InFramework` — direct evidence the invariant holds on a live stream.

______________________________________________________________________

## 8. Open questions

1. ~~Does the deployment configure a Python log channel?~~ **ANSWERED: yes** —
   `livereduce.py:17,29` installs `PythonLoggingChannel`. But this establishes *exposure*, not a
   deadlock: the cycle originally proposed does not close (§2b.1). **The live question is now:
   what production path lets a thread hold the GIL and then block on a C-level lock?** That is the
   missing half, and without it the logging hypothesis is a hazard class rather than a cause.
1. Under Variant B, does moving workspace population into `extractData()` cause unacceptable
   latency at the extract boundary, given the foreground already tolerates a `startupTimeout` wait?
1. Are there framework calls on the background thread reached *indirectly* that the §2b survey
   missed — e.g. via `DataService` notification observers registered by other components, which
   run on whichever thread posted the notification?
1. How far does §4's visibility enforcement reach before it fights the existing class layout? The
   handlers touch many members directly, so `ParseContext` must expose enough without becoming a
   second copy of the class.
1. Does the deferred-action pattern interact badly with any `rxPacket()` handler that depends on
   framework state *mid-parse*? `initWorkspacePart2()` is the known case, handled by
   interrupt-and-resume; the other six call sites need checking individually.
1. Is `std::timed_mutex` acceptable at every `m_mutex` site, or do any paths need the plain-mutex
   fast path?
1. Base-class versus listener-local fix for §6 — who owns the `LiveListener::extractData()`
   contract decision?
1. Does deferring `g_log` output change any test that asserts on log ordering relative to packet
   processing?
