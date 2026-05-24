# Specification: Refactor SNSLiveEventDataListener State Management
## (v3.0 — Separate state queries, explicit transition hooks, commit-in-`extractData()`)

---

## 1. Objectives

This refactor has three goals, in priority order:

1. **Expose the listener state and the ADARA run state without conflating them.**
   The current `runStatus()` is a single enum that mixes "what is the DAS doing?"
   with "is the listener happy?", and there is no clean way for a caller (such as
   stand-alone `LoadLiveData`) to ask either question without paying the cost of
   the other.

2. **Separate state-read from state-transition. Make every state mutation
   explicit and named.**
   The current `runStatus()` is a getter with large hidden side effects
   (clearing caches, re-initialising the workspace, consuming the deferred
   `RunStatusPkt`, dropping `m_pauseNetRead`). Reads must be `const` and free of
   side effects. All mutations must live in named methods — `onBeginRun()`,
   `onEndRun()`, `onRunPause()` — that can be individually understood and tested.

3. **Move as much of the state machine as possible into `extractData()`.**
   `extractData()` is the only method that every consumer of `ILiveListener`
   must call, and it is the natural commit point for any pending transition.
   External callers must never need to invoke a transition by hand to keep the
   listener healthy. In particular, stand-alone `LoadLiveData` must work
   without orchestrating the FSM externally.

The v2 attempt satisfied (3) but failed (2) because it folded all the
transition logic into a single private helper, and it failed (1) because it
left edge detection coupled to a mutating call. This v3 spec keeps (3) intact
while restoring (1) and (2) by separating **observing that a transition
occurred** from **committing one**.

---

## 2. Problem Statement (preserved from v2, expanded)

1. **Conflated state.** `runStatus()` returns a value that is part DAS state
   (`NoRun/BeginRun/Running/EndRun`) and part listener-internal FSM (it mutates
   to "Running" or "NoRun" as a side effect of being called).
2. **Hidden side effects.** Calling `runStatus()` clears the geometry cache,
   the name map, marks the workspace uninitialised, runs `initWorkspacePart1()`,
   consumes `m_deferredRunDetailsPkt`, and drops `m_pauseNetRead`.
3. **External-control deadlock.** Stand-alone `LoadLiveData` never calls
   `runStatus()`. After a `BeginRun`/`EndRun` packet sets `m_pauseNetRead = true`,
   the background reader stops, the workspace cannot be re-initialised, and the
   next `extractData()` either hits the 10-second `NotYet` timeout or spins
   indefinitely on a stale workspace.
4. **Implicit ordering contract with `MonitorLiveData`.** The current
   implementation depends on `MonitorLiveData` calling `extractData()` first and
   `runStatus()` second within the same loop iteration. The contract is
   undocumented and not enforceable by the type system.

---

## 3. Conceptual Model

### 3.1 Four orthogonal facts about the listener at any instant

The ADARA protocol exposes two independent state axes that the current `runStatus()` conflates:

* **Run state** — whether the DAS is in a run (`NoRun/BeginRun/Running/EndRun`).
  Driven by `RunStatusPkt`. Transitions require workspace-level coordination
  (cache clears, re-init), so they are queued and committed in `extractData()`.

* **Pause state** — whether the current run is paused.
  Driven by `AnnotationPkt` `PAUSE`/`RESUME` markers. Orthogonal to run
  state: a run can be in `Running` status while paused, and the ADARA
  protocol does not change the `RunStatus` field on a pause. Pause state
  has **no workspace-level side effects** beyond gating event appending;
  it is therefore applied immediately in the background thread, not queued.

| Question                                       | Answer                | Type              |
| ---------------------------------------------- | --------------------- | ----------------- |
| What is the DAS run state right now?           | `RunStatus`           | pure read         |
| Is the current run paused?                     | `bool`                | pure read         |
| Is the listener connected / back-pressure / errored? | `ListenerState` | pure read         |
| What run-state transition (if any) did the most recent `extractData()` consume? | `optional<RunStatus>` | pure read |

Note that `isPaused()` and `runState()` are orthogonal: they answer
independent questions about the DAS. `isPaused()` does **not** modify or
qualify the value of `runState()`. A caller inspecting `runState()` sees
`Running` whether the run is paused or not; a caller inspecting `isPaused()`
sees the pause flag regardless of run phase.

### 3.2 Commit points

| Action                                         | Where                    | How              |
| ---------------------------------------------- | ------------------------ | ---------------- |
| Apply a pending run-state transition           | inside `extractData()`   | dispatches to `onBeginRun()` / `onEndRun()` |
| Apply a pause/resume annotation               | inside `rxPacket(AnnotationPkt)` (background thread) | sets `m_isDasPaused` directly |

The key reframe versus v2: **"explicit transition methods" means
named-and-testable, not necessarily public.** Exposing them publicly would
re-violate objective (3) by letting external code drive the FSM. They are
*protected virtual* hooks: explicit, individually overridable and testable.
`onBeginRun()` and `onEndRun()` are dispatched only from `extractData()`
(foreground thread); `onRunPause()` is dispatched only from
`rxPacket(AnnotationPkt)` (background thread). The asymmetry is intentional
and motivated in §3.1 / §5.4.

---

## 4. Interface Changes

### 4.1 Base class `ILiveListener.h`

```cpp
namespace Mantid::API {

/// Listener connection / health, independent of the DAS run state.
enum class ListenerState {
    Disconnected,   ///< Not connected to the DAS
    Connected,      ///< Connected and reading
    ReadWait,       ///< Connected but paused at a run boundary (back-pressure)
    Error           ///< Background thread reported an exception
};

class MANTID_API_DLL ILiveListener : public Kernel::PropertyManager {
public:
    /// ADARA-style run status, unchanged
    enum RunStatus { NoRun = 0, BeginRun = 1, Running = 2, EndRun = 4 };

    // ----- pure state queries (no side effects, all const) -----

    /// Current DAS run state (NoRun / BeginRun / Running / EndRun),
    /// as last reported by the background thread.
    /// Default implementation returns NoRun for listeners that have no
    /// concept of run boundaries.
    virtual RunStatus runState() const { return NoRun; }

    /// Whether the current run has been paused by a DAS annotation.
    /// Orthogonal to runState(): runState() returns Running whether or
    /// not the run is paused. Default returns false.
    virtual bool isPaused() const { return false; }

    /// Listener connection / health. Default returns Connected once
    /// connect() succeeds and Disconnected otherwise; SNS overrides.
    virtual ListenerState listenerState() const = 0;

    /// The run-state transition (if any) that the most recent extractData()
    /// call consumed. Cleared at the *start* of the next extractData() call.
    /// Reports only run-state edges (BeginRun / EndRun); pause/resume are
    /// not reported here — use isPaused() for those.
    /// Listeners that have no transition concept always return nullopt.
    virtual std::optional<RunStatus> lastTransition() const { return std::nullopt; }

    // ----- existing methods, unchanged signatures -----

    virtual std::shared_ptr<Workspace> extractData() = 0;
    virtual bool isConnected() = 0;
    virtual bool dataReset() = 0;
    virtual int runNumber() const = 0;
    // ... name(), supportsHistory(), buffersEvents(), connect(), start(),
    //     setSpectra(), setAlgorithm() unchanged ...

    // ----- deprecated, retained for one release cycle -----

    /// Returns runState() and additionally consumes any pending transition.
    /// Provided so existing call sites compile unchanged during migration.
    [[deprecated("Use runState() / lastTransition() and call extractData() "
                 "to commit pending transitions.")]]
    virtual RunStatus runStatus();
};

} // namespace Mantid::API
```

`runStatus()` is *no longer pure virtual*; it has a default implementation in
`ILiveListener` (see §6) that wraps the new pure getters. Concrete listeners
that already override it can keep doing so, but new listeners need not.

### 4.2 SNSLiveEventDataListener.h

```cpp
class SNSLiveEventDataListener : public API::LiveListener,
                                 public Poco::Runnable,
                                 public ADARA::Parser {
public:
    // pure queries
    RunStatus runState() const override;
    bool isPaused() const override;
    ListenerState listenerState() const override;
    std::optional<RunStatus> lastTransition() const override;

    // commit point
    std::shared_ptr<API::Workspace> extractData() override;

    // unchanged
    int runNumber() const override { return m_runNumber; }
    bool isConnected() override;

protected:
    /// Explicit, named state-transition hooks (objective 2).
    ///
    /// onBeginRun() and onEndRun() are called exclusively from extractData()
    /// (foreground thread) when a queued run-state transition is committed.
    ///
    /// onRunPause() is called exclusively from rxPacket(AnnotationPkt)
    /// (background thread) immediately when a PAUSE/RESUME marker arrives.
    /// It is a named hook rather than inline code so that subclasses can
    /// override it (e.g. for testing or for future protocol extensions),
    /// but it is NOT dispatched through the pending-transition queue.
    ///
    /// (In the implementation header these become /// Doxygen blocks per method.)
    virtual void onBeginRun();
    virtual void onEndRun();
    virtual void onRunPause(bool paused);

private:
    // ----- ADARA / DAS state (written by background thread) -----
    RunStatus m_adaraRunStatus{NoRun};      ///< What the DAS says NOW
    std::shared_ptr<ADARA::RunStatusPkt> m_deferredRunDetailsPkt;

    // ----- pending run-state transition (background -> foreground) -----
    //
    // INVARIANT: at most one un-consumed transition can exist at a time.
    // The background thread enforces this by setting m_pauseNetRead = true
    // whenever it queues a transition (NEW_RUN with m_workspaceInitialized
    // OR END_RUN) and then blocking the reader until extractData() releases
    // m_pauseNetRead via onBeginRun()/onEndRun(). The "little white lie"
    // path in rxPacket(NEW_RUN with !m_workspaceInitialized) queues NO
    // transition and sets NO back-pressure, so it does not violate the
    // invariant.
    //
    // Violation of this invariant is treated as an implementation error
    // and raises std::runtime_error (see §5.2 and §5.4).
    std::optional<RunStatus> m_pendingTransition;

    // ----- result of the most recent commit (read by lastTransition()) -----
    std::optional<RunStatus> m_lastTransition;

    // ----- listener health -----
    std::shared_ptr<std::runtime_error> m_backgroundException;

    // ----- existing members, unchanged -----
    int m_runNumber{0};
    DataObjects::EventWorkspace_sptr m_eventBuffer;
    bool m_workspaceInitialized{false};
    std::string m_instrumentName;
    std::string m_instrumentXML;
    std::vector<std::string> m_requiredLogs;
    std::vector<std::string> m_monitorLogs;
    Poco::Net::StreamSocket m_socket;
    bool m_isConnected{false};
    Poco::Thread m_thread;
    mutable std::mutex m_mutex;              ///< guards all of the above
    bool m_pauseNetRead{false};              ///< retained name; back-pressure flag
    bool m_stopThread{false};
    Types::Core::DateAndTime m_startTime;
    Types::Core::DateAndTime m_dataStartTime;
    bool m_isDasPaused{false};               ///< set by onRunPause(); read by isPaused()
    bool m_keepPausedEvents{false};
    NameMapType m_nameMap;
    VariableMapType m_variableMap;
    bool m_ignorePackets{false};
    bool m_filterUntilRunStart{false};
    std::set<detid_t> m_badMonitors;
    // ... etc. (unchanged from current header)
};
```

**Removed:** the single conflated `m_status` is split into
- `m_adaraRunStatus` — what the DAS run state is right now, read by `runState()`;
- `m_pendingTransition` — exactly one run-state edge waiting to be consumed by `extractData()`;
- `m_lastTransition` — what the most recent `extractData()` consumed, read by `lastTransition()`.

**Renamed:** `m_runPaused` → `m_isDasPaused`, read by `isPaused()`.

`m_pauseNetRead` keeps its current name and its current meaning (foreground/
background back-pressure), to keep the diff focused and the comments in
`rxPacket(RunStatusPkt)` intelligible.

---

## 5. Implementation

### 5.1 Pure getters

```cpp
ILiveListener::RunStatus
SNSLiveEventDataListener::runState() const {
    if (m_backgroundException) throw *m_backgroundException;
    std::lock_guard lock(m_mutex);
    return m_adaraRunStatus;
}

ListenerState
SNSLiveEventDataListener::listenerState() const {
    std::lock_guard lock(m_mutex);
    if (m_backgroundException) return ListenerState::Error;
    if (!m_isConnected)        return ListenerState::Disconnected;
    if (m_pauseNetRead)        return ListenerState::ReadWait;
    return ListenerState::Connected;
}

std::optional<ILiveListener::RunStatus>
SNSLiveEventDataListener::lastTransition() const {
    if (m_backgroundException) throw *m_backgroundException;
    std::lock_guard lock(m_mutex);
    return m_lastTransition;
}
bool SNSLiveEventDataListener::isPaused() const {
    std::lock_guard lock(m_mutex);
    return m_isDasPaused;
}
```

All four getters are `const` and have no side effects; none of them mutate
`m_adaraRunStatus`, `m_isDasPaused`, `m_pendingTransition`, `m_pauseNetRead`, or any cache.

### 5.2 Background reader sets the ADARA state and queues a transition

In `rxPacket(const ADARA::RunStatusPkt &pkt)`, the only changes versus the
current code are:

* Replace assignments to `m_status` with assignments to `m_adaraRunStatus`.
* On `NEW_RUN` with `m_workspaceInitialized == true`, queue
  `m_pendingTransition = BeginRun` and set `m_pauseNetRead = true` (unchanged).
* On `NEW_RUN` with `m_workspaceInitialized == false`, keep the existing
  "little white lie" path: set `m_adaraRunStatus = Running`, call
  `setRunDetails(pkt)` directly, *do not* queue a transition, *do not* set
  `m_pauseNetRead`. The behavior and the long explanatory comment in the
  current `rxPacket(RunStatusPkt)` are preserved verbatim.
* On `END_RUN`, queue `m_pendingTransition = EndRun`, set
  `m_pauseNetRead = true`, set `m_adaraRunStatus = EndRun`, copy
  `setRunDetails(pkt)` if `!haveRunNumber` (unchanged).

**Single-slot invariant.** Whenever the background thread is *about* to assign
`m_pendingTransition` (BeginRun on the non-white-lie path, or EndRun), it must
first verify that `!m_pendingTransition`. If a pending transition is already
queued, the back-pressure mechanism (`m_pauseNetRead`) has failed to block the
reader and the listener is in an inconsistent state — this is an implementation
error, not a runtime condition, so the listener raises:

```cpp
if (m_pendingTransition) {
    throw std::runtime_error(
        "SNSLiveEventDataListener: pending run-state transition was not "
        "consumed before a new transition arrived — back-pressure invariant "
        "violation.");
}
m_pendingTransition = BeginRun;   // or EndRun
m_pauseNetRead      = true;
```

This is a `runtime_error` rather than a debug-only assertion so that the
condition is never silently masked in release builds.

In `rxPacket(const ADARA::AnnotationPkt &pkt)`, the `PAUSE` and `RESUME`
markers call `onRunPause(true/false)` directly from the background thread.
This is intentional: pause state has no workspace-level side effects
(it only gates event appending in `rxPacket(BankedEventPkt)`), so applying
it immediately at the packet boundary gives the most accurate event filtering
relative to the DAS timeline. The pause/resume path does **not** use
`m_pendingTransition` and does **not** set `m_pauseNetRead`.

### 5.3 `extractData()` — the only commit point

```cpp
std::shared_ptr<Workspace>
SNSLiveEventDataListener::extractData() {
    if (m_backgroundException) throw *m_backgroundException;

    // ---- Phase 1: commit any pending transition --------------------------
    std::optional<RunStatus> pending;
    {
        std::lock_guard lock(m_mutex);
        pending = m_pendingTransition;
        m_pendingTransition.reset();
    }

    if (pending) {
        // Only clear m_lastTransition when we are actually about to publish a
        // new edge. If pending is nullopt we leave the previous transition
        // visible, so that LoadLiveData's NotYet retry loop (LoadLiveData.cpp
        // lines 476–490) does not lose the edge across re-invocation.
        {
            std::lock_guard lock(m_mutex);
            m_lastTransition.reset();
        }
        switch (*pending) {
          case BeginRun: onBeginRun(); break;
          case EndRun:   onEndRun();   break;
          default:       break;          // Running / NoRun not queued
        }
        std::lock_guard lock(m_mutex);
        m_lastTransition = pending;       // memoise for lastTransition()
    }

    // ---- Phase 2: wait for workspace initialisation (unchanged) ----------
    static const double maxBlockTime = 10.0;
    const DateAndTime endTime = DateAndTime::getCurrentTime() + maxBlockTime;
    while (!m_workspaceInitialized && DateAndTime::getCurrentTime() < endTime) {
        Poco::Thread::sleep(100);
    }
    if (!m_workspaceInitialized) {
        throw Exception::NotYet("The workspace has not yet been initialized.");
    }
    if (m_ignorePackets) {
        throw Exception::NotYet("Waiting for a run to start.");
    }

    // ---- Phase 3: build the new EventWorkspace and swap (unchanged) ------
    EventWorkspace_sptr temp = std::dynamic_pointer_cast<EventWorkspace>(
        API::WorkspaceFactory::Instance().create(
            "EventWorkspace", m_eventBuffer->getNumberHistograms(), 2, 1));
    API::WorkspaceFactory::Instance().initializeFromParent(*m_eventBuffer, *temp, false);
    temp->mutableRun().clearOutdatedTimeSeriesLogValues();
    for (auto &monitorLog : m_monitorLogs)
        temp->mutableRun().removeProperty(monitorLog);
    m_monitorLogs.clear();

    auto monitorBuffer = m_eventBuffer->monitorWorkspace();
    if (monitorBuffer) {
        auto newMonitorBuffer = WorkspaceFactory::Instance().create(
            "EventWorkspace", monitorBuffer->getNumberHistograms(), 1, 1);
        WorkspaceFactory::Instance().initializeFromParent(*monitorBuffer, *newMonitorBuffer, false);
        temp->setMonitorWorkspace(newMonitorBuffer);
    }
    {
        std::lock_guard lock(m_mutex);
        std::swap(m_eventBuffer, temp);
    }
    return temp;
}
```

Important properties:

* **Phase 1 runs once per call.** The pending transition is dequeued atomically;
  no `m_transitionHandled` flag is needed.
* **`m_lastTransition` is reset only when a new edge is being committed.** If
  `pending` is `nullopt` (no new DAS edge since the previous call), the prior
  value of `m_lastTransition` is left in place. This is essential for
  `LoadLiveData`'s `Exception::NotYet` retry loop: the first `extractData()`
  call may commit `BeginRun` and then throw `NotYet` from Phase 2 (workspace
  not yet initialised); subsequent retry calls must still report
  `lastTransition() == BeginRun` so that `MonitorLiveData` can rename the
  workspace. Once a *new* edge arrives and is committed, the stale value is
  overwritten.
* **Phase 1 holds the mutex only while reading the queue**, not while running
  the transition hook. Hooks may safely take the mutex themselves.
* **Exception-safe.** Phase 2's `Exception::NotYet` is thrown *after* the
  transition is already committed and `m_lastTransition` recorded; if Phase 2
  throws, the next call sees no pending transition (correct: it was applied)
  and `m_lastTransition` retains its value (per bullet 2 above).

### 5.4 Transition hooks

```cpp
void SNSLiveEventDataListener::onBeginRun() {
    std::lock_guard lock(m_mutex);

    m_workspaceInitialized = false;

    // Cache clears: exactly the set the current runStatus() clears.
    m_instrumentXML.clear();
    m_instrumentName.clear();
    // Note: m_dataStartTime NOT cleared on BeginRun — matches current behavior.
    m_nameMap.clear();

    initWorkspacePart1();

    if (!m_deferredRunDetailsPkt) {
        // Invariant: rxPacket(NEW_RUN) must have stashed the RunStatusPkt
        // before queueing BeginRun. Reaching here means a producer queued a
        // BeginRun transition without populating m_deferredRunDetailsPkt,
        // which is an implementation error in the listener itself.
        throw std::runtime_error(
            "SNSLiveEventDataListener::onBeginRun(): "
            "m_deferredRunDetailsPkt is null — invariant violation.");
    }
    setRunDetails(*m_deferredRunDetailsPkt);
    m_deferredRunDetailsPkt.reset();

    m_adaraRunStatus = Running;   // we've crossed the edge
    m_pauseNetRead   = false;     // release the background reader
}

void SNSLiveEventDataListener::onEndRun() {
    std::lock_guard lock(m_mutex);

    m_workspaceInitialized = false;

    m_instrumentXML.clear();
    m_instrumentName.clear();
    m_dataStartTime = Types::Core::DateAndTime();   // cleared only on EndRun
    m_nameMap.clear();

    initWorkspacePart1();

    m_adaraRunStatus = NoRun;
    m_pauseNetRead   = false;
}

void SNSLiveEventDataListener::onRunPause(bool paused) {
    // Called directly from rxPacket(AnnotationPkt) in the background thread —
    // NOT dispatched through the pending-transition queue.
    //
    // Pause state is orthogonal to run state: m_adaraRunStatus remains
    // Running while the run is paused. m_isDasPaused is read by isPaused()
    // and by rxPacket(BankedEventPkt) to gate event appending.
    //
    // Applying this immediately (rather than deferring to extractData()) gives
    // accurate event counts: events received after the PAUSE annotation but
    // before the next extractData() call are correctly discarded.
    std::lock_guard lock(m_mutex);
    m_isDasPaused = paused;
}
```

Notes:

* The set of side effects in `onBeginRun()` / `onEndRun()` is identical to
  what the current `runStatus()` does (compare `SNSLiveEventDataListener.cpp:1495–1534`).
  Nothing new is cleared, nothing previously cleared is left dirty.
* `onBeginRun()` and `onEndRun()` are `protected virtual` and dispatched only
  from `extractData()`. Unit tests can subclass and override to assert the hooks
  fired with the expected preconditions.
* `onRunPause()` is `protected virtual` and called only from
  `rxPacket(AnnotationPkt)` in the background thread. Its dispatch point is
  different from `onBeginRun`/`onEndRun` by design (see §5.2 rationale). Unit
  tests can override it to observe pause/resume without a live ADARA stream.
* The existing `m_runPaused` check in `rxPacket(BankedEventPkt)` is updated
  to reference `m_isDasPaused`; no other change to that function.

### 5.5 Background reader loop

```cpp
void SNSLiveEventDataListener::run() {
    // ... unchanged setup ...
    while (!m_stopThread) {
        while (m_pauseNetRead && !m_stopThread) {
            Poco::Thread::sleep(100);
        }
        if (m_stopThread) break;
        try {
            read();   // ADARA::Parser::read() drives rxPacket() callbacks
        } catch (...) {
            // unchanged exception capture into m_backgroundException
        }
    }
}
```

No change versus today. The `m_pauseNetRead` back-pressure is released by
`onBeginRun()` / `onEndRun()` inside `extractData()`, so stand-alone
`LoadLiveData` no longer deadlocks.

---

## 6. Backward Compatibility: deprecated `runStatus()`

`runStatus()` is retained with a default base-class implementation:

```cpp
ILiveListener::RunStatus ILiveListener::runStatus() {
    // For callers that pre-date the v3 split.
    // Returns the same value the legacy method would have, but does NOT
    // execute the legacy side effects — those happen inside extractData().
    if (auto edge = lastTransition())
        return *edge;
    return runState();
}
```

This is sufficient for **`MonitorLiveData`** because the call order in that
algorithm is `extractData()` first (via `loadAlg->executeAsChildAlg()` at
`MonitorLiveData.cpp:177`) then `runStatus()` (at `MonitorLiveData.cpp:191`):
by the time `runStatus()` is called, `extractData()` has already committed the
transition and `lastTransition()` is populated. The returned value matches
what the legacy `runStatus()` would have returned (`BeginRun` or `EndRun` once
across the boundary, then `Running`/`NoRun` afterwards).

For any caller that historically relied on the legacy *side effects* of
`runStatus()` — there are none in tree besides `SNSLiveEventDataListener`
itself — the deprecation message points them at the new API.

The `SNSLiveEventDataListener` override of `runStatus()` is **removed**;
it falls through to the base default. The hidden side effects move into
`onBeginRun()`/`onEndRun()` as described above.

Because the base-class default is expressed purely in terms of `runState()`
and `lastTransition()`, both of which have safe defaults (`NoRun` /
`nullopt`), it cannot throw. A listener that for some reason wishes to
forbid `runStatus()` (e.g. because callers should migrate) is free to
override it as `[[noreturn]] throw std::logic_error(...)`; nothing in the
v3 design relies on the default never throwing.

---

## 7. Algorithm impact

### 7.1 `LoadLiveData` (stand-alone)

No source changes required. The existing loop at `LoadLiveData.cpp:476-490`
calls `extractData()` only; the transition commit now happens inside that
call, `m_pauseNetRead` is released as part of the commit, and the listener
proceeds. The deadlock is gone.

### 7.2 `MonitorLiveData`

No source changes required. The existing logic at `MonitorLiveData.cpp:191-228`
calls `listener->runStatus()` after `loadAlg->executeAsChildAlg()`; with the
base-class default `runStatus()` returning `lastTransition().value_or(runState())`,
the return value matches the legacy behaviour exactly.

A follow-up cleanup (out of scope for this refactor, recorded in the release
note) can migrate `MonitorLiveData` to call `listener->lastTransition()`
directly, which is then a `const` query.

### 7.3 `StartLiveData`

No changes.

### 7.4 Other `ILiveListener` implementations

Each concrete listener provides its own override of `runState()` (and any of
the other new pure-getter methods) that reflects its own internal state —
they do **not** rely on the base-class defaults. The base defaults exist so
that new listeners can be added incrementally and so that mocks can opt out
selectively, but the production listeners in the tree are updated explicitly
as part of this refactor. The full per-listener change list is in
`plans/listener_refactoring_other_listeners.md`.

Notably, `FakeEventDataListener::runStatus()` currently has real side
effects (it mutates `m_nextEndRunTime` and `m_runNumber` when generating a
periodic `EndRun`; see `FakeEventDataListener.cpp:50–59`). This is a second
instance of the anti-pattern v3 fixes: those mutations must move into an
explicit transition step driven from `extractData()`, with `runState()` made
into a pure getter. The companion document specifies the per-listener
treatment.

---

## 8. Behaviour Preservation — does v3 conserve existing behaviour?

**Short answer: yes, for every observable algorithm-level behaviour, and yes
for the internal cache-clear set. The one intentional difference is the
removal of the implicit `extractData()`-then-`runStatus()` ordering
contract — and that difference is invisible because the only in-tree caller
honoured the contract already.**

| Behaviour                                                                            | Current code         | v3                                                                                     | Preserved? |
| ------------------------------------------------------------------------------------ | -------------------- | -------------------------------------------------------------------------------------- | ---------- |
| `runStatus()` returns `BeginRun` exactly once at the start of a run                  | yes, via mutation    | yes, via `lastTransition()` populated by `extractData()`                               | ✅ |
| `runStatus()` returns `EndRun` exactly once at the end of a run                      | yes                  | yes                                                                                    | ✅ |
| Workspace is re-initialised at run boundaries                                        | inside `runStatus()` | inside `onBeginRun()` / `onEndRun()`, called by `extractData()`                        | ✅ |
| `m_dataStartTime` cleared on `EndRun` only, not on `BeginRun`                        | yes                  | yes (see §5.4)                                                                         | ✅ |
| `m_instrumentXML`, `m_instrumentName`, `m_nameMap` cleared at both boundaries        | yes                  | yes                                                                                    | ✅ |
| `m_deferredRunDetailsPkt` consumed at `BeginRun`                                     | yes                  | yes, inside `onBeginRun()`                                                             | ✅ |
| `m_pauseNetRead` released after the boundary is consumed                             | inside `runStatus()` | inside `onBeginRun()` / `onEndRun()`                                                   | ✅ |
| "Little white lie" path when `NEW_RUN` arrives before `m_workspaceInitialized`       | yes                  | yes — code in `rxPacket(RunStatusPkt)` is preserved verbatim                            | ✅ |
| `m_runPaused` (`m_isDasPaused`) flips on `AnnotationPkt` MARKER_PAUSE / MARKER_RESUME | yes, inline     | yes, now routed through `onRunPause()` in background thread                            | ✅ |
| Paused events discarded at the correct packet boundary                               | yes              | yes — `m_isDasPaused` flipped immediately in background thread, not deferred            | ✅ |
| `isPaused()` query available without calling `runStatus()`                           | no               | yes (`isPaused()` is a `const` pure getter on `ILiveListener`)                         | 🔧 new |
| `MonitorLiveData` workspace renaming triggers on `BeginRun`/`EndRun`                 | yes                  | yes (legacy `runStatus()` shim returns the edge)                                       | ✅ |
| Stand-alone `LoadLiveData` produces a workspace                                      | **no** (deadlocks)   | yes (commit happens inside `extractData()`)                                            | 🔧 fixed |
| Listener can be queried for its state without mutating it                            | **no**               | yes (`runState()`, `isPaused()`, `listenerState()`, `lastTransition()` all `const`)    | 🔧 fixed |

There is **one behaviour that is intentionally not preserved**, and it is the
bug the refactor exists to fix: stand-alone `LoadLiveData` previously
deadlocked after a run boundary; it now succeeds.

Two **subtleties to flag** in code review:

1. **`m_lastTransition` lifetime.** It is populated when `extractData()`
   actually commits a transition, and reset only when a *new* transition is
   about to be committed. Calls to `extractData()` that find nothing to commit
   (e.g. `LoadLiveData`'s `NotYet` retry loop at `LoadLiveData.cpp:476-490`)
   leave the previous value intact, so an edge that was committed but whose
   workspace publication had to wait for initialisation still survives the
   retry, and `MonitorLiveData` still sees it. A caller that never calls
   `extractData()` never sees an edge (correct — no commit, no edge).

2. **Pause/resume packets are processed in the background thread** via
   `onRunPause()`, not at `extractData()` time. This is intentional: applying
   `m_isDasPaused` immediately at the annotation packet boundary gives
   accurate event filtering — events arriving between the `PAUSE` annotation
   and the next `extractData()` call are correctly discarded. Deferring to
   `extractData()` would retroactively mis-categorise those events. The
   `onRunPause()` hook is `protected virtual` so subclasses can observe or
   override the pause/resume behaviour; the dispatch point (background thread)
   is noted clearly in its documentation.

---

## 9. Files modified

| File                                                                       | Change                                                                          |
| -------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| `Framework/API/inc/MantidAPI/ILiveListener.h`                              | Add `ListenerState` enum, `runState()`, `isPaused()`, `listenerState()`, `lastTransition()`; default-implement `runStatus()`; deprecate it. |
| `Framework/API/src/ILiveListener.cpp` (new, if not present)                | Out-of-line default `runStatus()` implementation.                               |
| `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`         | Split `m_status` into `m_adaraRunStatus` / `m_pendingTransition` / `m_lastTransition`; rename `m_runPaused` → `m_isDasPaused`; declare `onBeginRun`, `onEndRun`, `onRunPause`; declare `runState()`, `isPaused()`, `listenerState()`, `lastTransition()`; change `std::mutex m_mutex` → `mutable std::mutex m_mutex` (the pure getters are `const` and lock the mutex). |
| `Framework/LiveData/src/SNSLiveEventDataListener.cpp`                      | Implement getters; rewrite `extractData()` Phase 1; extract `onBeginRun`/`onEndRun` from current `runStatus()`; remove `runStatus()` override; route `AnnotationPkt` pause/resume through `onRunPause()`; rename `m_status → m_adaraRunStatus`, `m_runPaused → m_isDasPaused`; add `std::runtime_error` checks per §5.2 and §5.4. |
| `Framework/LiveData/inc/MantidLiveData/FakeEventDataListener.h` + `.cpp`   | Override `runState()` (pure), `listenerState()`; move side effects out of `runStatus()` per `listener_refactoring_other_listeners.md`. |
| `Framework/LiveData/inc/MantidLiveData/FileEventDataListener.h` + `.cpp`   | Override `runState()` (returns `Running`), `listenerState()`.                    |
| `Framework/LiveData/Kafka/inc/MantidLiveData/Kafka/KafkaEventListener.h` + `.cpp` | Override `runState()` (queries decoder), `listenerState()`.                |
| `Framework/LiveData/Kafka/inc/MantidLiveData/Kafka/KafkaHistoListener.h` + `.cpp` | Override `runState()`, `listenerState()`.                                    |
| `Framework/LiveData/ISIS/inc/MantidLiveData/ISIS/ISISLiveEventDataListener.h` + `.cpp` | Override `runState()` (returns `Running`), `listenerState()`.          |
| `Framework/LiveData/ISIS/inc/MantidLiveData/ISIS/ISISHistoDataListener.h` + `.cpp`     | Override `runState()` (returns `Running`), `listenerState()`.          |
| `Framework/SINQ/inc/MantidSINQ/SINQHMListener.h` + `.cpp`                  | Override `runState()`, `listenerState()`.                                       |
| `Framework/LiveData/test/TestGroupDataListener.h` + `.cpp`                 | Override `runState()` (returns `Running`), `listenerState()`.                    |
| `Framework/LiveData/test/TestDataListener.h` + `.cpp`                      | Override `runState()`, `listenerState()`.                                       |
| `Framework/API/test/LiveListenerTest.h` (`MockLiveListener`)               | Override `runState()`, `listenerState()`.                                       |
| `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`                   | New tests, §10.                                                                 |
| `plans/listener_refactoring_other_listeners.md`                            | Companion doc: per-listener change instructions (rewritten for v3).             |
| `docs/source/release/v6.x/Framework/LiveData/...`                          | Release note (bugfix: stand-alone LoadLiveData; new API: pure-getter state).    |

Approximate scope: ~15 source files touched (most are one-line additions),
~300 lines added in total, ~80 lines moved, ~30 lines deleted. The bulk of
the new lines are in `SNSLiveEventDataListener.{h,cpp}` and the new tests.

---

## 10. Testing

### 10.1 Unit tests, SNSLiveEventDataListener

1. `test_runState_pure_getter_does_not_mutate` — call `runState()` 100×
   across each ADARA state; assert no internal field changes.
2. `test_listenerState_reflects_connection_and_pause` — drive
   connect/disconnect/pause via test fixture; assert each state is reachable.
3. `test_lastTransition_reports_BeginRun_once` — inject `NEW_RUN` packet,
   call `extractData()`, assert `lastTransition() == BeginRun`; call
   `extractData()` again with no new packet, assert `lastTransition() == nullopt`.
4. `test_lastTransition_reports_EndRun_once` — symmetric.
5. `test_extractData_commits_BeginRun_side_effects` — subclass overrides
   `onBeginRun()` to record entry; inject `NEW_RUN`; call `extractData()`;
   assert hook fired exactly once, caches cleared, `m_pauseNetRead == false`,
   workspace re-initialised.
6. `test_extractData_commits_EndRun_side_effects` — symmetric, plus
   `m_dataStartTime` cleared.
7. `test_no_transition_no_hook` — inject normal event packets, call
   `extractData()`, assert neither hook fires.
8. `test_onRunPause_invoked_for_pause_resume_markers` — feed
   `AnnotationPkt(PAUSE)` / `AnnotationPkt(RESUME)` directly; assert
   `m_isDasPaused` flips and `isPaused()` returns the correct value.
   Assert that `m_adaraRunStatus` is **not** modified by pause/resume.
9. `test_isPaused_orthogonal_to_runState` — assert that `runState()`
   returns `Running` before and after a PAUSE annotation; only `isPaused()`
   changes.
10. `test_legacy_runStatus_returns_edge_then_state` — the deprecated wrapper
    must match the historical sequence: `BeginRun, Running, Running, ..., EndRun, NoRun`.
11. `test_background_exception_propagates_from_all_getters` — set
    `m_backgroundException`; assert `runState()`, `isPaused()`,
    `listenerState()`, `lastTransition()`, `extractData()`, and
    `runStatus()` all throw (where applicable).

### 10.2 Concurrency tests

12. `test_concurrent_getters_no_data_race` — 4 reader threads spamming
    `runState()`/`isPaused()`/`listenerState()`/`lastTransition()` while the
    background thread injects packets; ThreadSanitizer clean.
13. `test_pending_transition_queue_invariant_violation_throws` — using a
    test fixture that injects packets directly into `rxPacket(RunStatusPkt)`
    (bypassing the background reader's `m_pauseNetRead` gate), call
    `rxPacket(NEW_RUN)` followed immediately by `rxPacket(END_RUN)` without
    an intervening `extractData()`. Assert that the second call throws
    `std::runtime_error` (the single-slot invariant from §5.2). In normal
    operation the reader's `m_pauseNetRead` back-pressure prevents this
    sequence; the test exercises the safety net.

### 10.3 Integration tests

14. `test_LoadLiveData_standalone_no_deadlock` — instantiate
    `LoadLiveData` with a fake SNS listener that simulates a `BeginRun`
    boundary; assert the algorithm completes within 5 s and produces a
    workspace. This is the regression test for the bug that motivated v3.
15. `test_MonitorLiveData_workspace_renaming_unchanged` — drive a
    `NoRun → BeginRun → Running → EndRun → NoRun` sequence; assert the
    output workspace is renamed with the correct suffix at each boundary.
    Uses the *unmodified* `MonitorLiveData` to prove backward compatibility.

### 10.4 Documentation / clang-tidy / pre-commit

16. `clang-tidy` on the modified files with the repo `.clang-tidy` config.
17. `pre-commit run --files <modified files>`.

---

## 11. Implementation Plan

1. **Header changes only.** Add `ListenerState`, the three new pure getters,
   the deprecation on `runStatus()`, and a base-class default. Build the tree;
   add the trivial `listenerState()` overrides to every other listener and
   their mocks. No behavioural change yet. (1 day)

2. **Split `m_status`.** Introduce `m_adaraRunStatus`, `m_pendingTransition`,
   `m_lastTransition`. Update `rxPacket(RunStatusPkt)` to write
   `m_adaraRunStatus` and queue into `m_pendingTransition` instead of
   touching `m_status`. Implement the three getters. Keep the existing
   `runStatus()` override temporarily; have it dequeue the pending transition
   itself for now, identical to current code. All existing tests must still
   pass. (1–2 days)

3. **Extract `onBeginRun`/`onEndRun`/`onRunPause`.** Move the side-effect
   blocks out of `runStatus()` and the `AnnotationPkt` handler into the named
   hooks. Have the current `runStatus()` override call the hooks instead of
   inlining the work. Existing tests still pass. (1–2 days)

4. **Move the commit to `extractData()`.** Add Phase 1 dispatching to the
   hooks. Delete the `SNSLiveEventDataListener::runStatus()` override; fall
   through to the base-class default. Run the full SNS listener and
   `MonitorLiveData` integration tests. (1 day)

5. **Stand-alone `LoadLiveData` regression test.** Add the test from §10.3.13;
   confirm it passes without changes to `LoadLiveData`. (½ day)

6. **Docs + release note.** (½ day)

Total: 5–7 days, single PR practical.

---

## 12. Summary

* **Objective 1 — separate listener state and ADARA state.**
  `listenerState()` and `runState()` are independent pure getters.
* **Objective 2 — explicit, named transition methods, no hidden side
  effects on reads.** All mutations live in `onBeginRun()`, `onEndRun()`,
  `onRunPause()`. All public reads are `const`.
* **Objective 3 — commit the state machine inside `extractData()`.**
  `extractData()` is the only place transitions fire. External callers do
  not orchestrate the FSM. `lastTransition()` is a memoised observation,
  not a control point — that is what makes (3) compatible with (1) and (2).

**Behaviour preservation: yes.** Every algorithm-observable behaviour is
identical to the current implementation, except that stand-alone
`LoadLiveData` no longer deadlocks (the bug this refactor fixes). The set
of caches cleared at each transition is exactly the current set, the
"little white lie" path in `rxPacket(NEW_RUN)` is preserved verbatim, and
`MonitorLiveData` runs unmodified against the deprecated `runStatus()`
shim.

---

**Version**: 3.0
**Date**: 2026-05-24
**Owner**: Mantid Development Team
**Supersedes**: `listener_refactoring_v2.md`
