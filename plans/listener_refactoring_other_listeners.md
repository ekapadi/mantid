# Listener Refactoring v3 — Companion Document: Other Listener Implementations

This document is the companion to `plans/listener_refactoring_v3.md`. It
specifies the per-listener changes required by the v3 refactor for every
concrete `ILiveListener` implementation in the tree besides
`SNSLiveEventDataListener` (whose treatment is in v3 itself).

The v3 base-class interface adds four pure getters with safe defaults:

```cpp
virtual RunStatus runState() const { return NoRun; }
virtual bool isPaused() const { return false; }
virtual ListenerState listenerState() const = 0;
virtual std::optional<RunStatus> lastTransition() const { return std::nullopt; }
```

and deprecates `runStatus()`, providing a base-class default:

```cpp
RunStatus runStatus() {
    if (auto edge = lastTransition()) return *edge;
    return runState();
}
```

**Per the v3 spec §7.4, every production listener provides its own override
of `runState()` rather than relying on the base default.** The defaults exist
so new listeners can be added incrementally and so that mocks can opt out
selectively. `listenerState()` is `= 0` and so *must* be overridden by every
concrete listener.

> **Template-method note (sub-spec 02b).** `API::LiveListener::extractData()`
> is finalised in sub-spec 02b and dispatches to two protected hooks:
> `onBeforeExtract()` (default no-op; foreground-thread "tick" point for
> commits, polls, synthetic-clock advances, etc.) and `doExtractData()`
> (pure virtual; the workspace-construction step). Wherever this document
> shows a private helper (`tickRunState()` in §3.1, `pollStatus()` in §3.6)
> being called from a wrapped `extractData()`, the **actual** implementation
> puts that helper's body directly in `onBeforeExtract()` and leaves
> `doExtractData()` (renamed mechanically in 02b) untouched. See the
> "Adjustment versus OL §3.1/§3.6" notes in `plans/v3_subspecs/03…` and
> `plans/v3_subspecs/04…` for the per-listener wording.

______________________________________________________________________

## 1. Anti-pattern review

Two listeners besides `SNSLiveEventDataListener` have **real side effects
inside `runStatus()`** and must be cleaned up:

1. `FakeEventDataListener::runStatus()` (`FakeEventDataListener.cpp:50–59`)
   mutates `m_nextEndRunTime` and increments `m_runNumber` when it decides
   to emit an `EndRun`.

1. `SINQHMListener::runStatus()` (`SINQHMListener.cpp:60–98`) performs an
   HTTP request, parses the response, writes the `hmhost` member, sets the
   `dimDirty` flag when transitioning `NoRun → Running`, and is *also*
   invoked from inside `extractData()` (line 104) specifically to perform
   these side effects.

Both are the same anti-pattern v3 exists to fix: a "getter" that is in fact
the FSM tick. They are handled in §3.1 and §3.6 below.

The remaining listeners (`FileEventDataListener`, `ISISHistoDataListener`,
`ISISLiveEventDataListener`, `KafkaEventListener`, `KafkaHistoListener`,
`TestGroupDataListener`, `TestDataListener`, and the `MockLiveListener` in
`Framework/API/test/LiveListenerTest.h`) have side-effect-free `runStatus()`
implementations and need only mechanical additions.

______________________________________________________________________

## 2. Listener-by-listener summary matrix

| Listener                                     | Side effects in `runStatus()`? | New overrides required                       |
| -------------------------------------------- | ------------------------------ | -------------------------------------------- |
| `SNSLiveEventDataListener`                   | yes (heavy) — see v3 spec      | full v3 treatment                            |
| `FakeEventDataListener`                      | yes (run-number mutation)      | `runState`, `listenerState`, refactor needed |
| `SINQHMListener`                             | yes (HTTP + dimDirty mutation) | `runState`, `listenerState`, refactor needed |
| `KafkaEventListener`                         | no                             | `runState`, `listenerState`                  |
| `KafkaHistoListener`                         | no                             | `runState`, `listenerState`                  |
| `FileEventDataListener`                      | no                             | `runState`, `listenerState`                  |
| `ISISLiveEventDataListener`                  | no                             | `runState`, `listenerState`                  |
| `ISISHistoDataListener`                      | no                             | `runState`, `listenerState`                  |
| `TestGroupDataListener` (test fixture)       | no                             | `runState`, `listenerState`                  |
| `TestDataListener` (test fixture)            | no                             | `runState`, `listenerState`                  |
| `MockLiveListener` (in `LiveListenerTest.h`) | no                             | `runState`, `listenerState`                  |

`isPaused()` and `lastTransition()` inherit their base defaults
(`false` / `nullopt`) for every listener except `SNSLiveEventDataListener`;
no override is needed elsewhere.

______________________________________________________________________

## 3. Detailed changes per listener

### 3.1 `FakeEventDataListener` — anti-pattern fix

**Current** (`FakeEventDataListener.cpp:50–59`):

```cpp
ILiveListener::RunStatus FakeEventDataListener::runStatus() {
    if (m_endRunEvery > 0 &&
        DateAndTime::getCurrentTime() > m_nextEndRunTime) {
        m_nextEndRunTime = DateAndTime::getCurrentTime() + m_endRunEvery;
        m_runNumber++;
        return EndRun;
    } else {
        return Running;
    }
}
```

**Problem**: increments `m_runNumber` and advances `m_nextEndRunTime` as a
side effect of being polled. This is exactly the v3 anti-pattern.

**v3 treatment**: the periodic-EndRun *decision* moves to a private helper
that is invoked from `extractData()`; `runState()` becomes a pure getter that
reports the currently-recorded state. `isPaused()`, `lastTransition()`
inherit defaults.

```cpp
// header
RunStatus runState() const override;
ListenerState listenerState() const override;
std::optional<RunStatus> lastTransition() const override;

std::shared_ptr<API::Workspace> extractData() override;

private:
void tickRunState();           // moved from runStatus()
RunStatus m_runState{Running};
std::optional<RunStatus> m_lastTransition;
```

```cpp
// cpp
RunStatus FakeEventDataListener::runState() const { return m_runState; }
ListenerState FakeEventDataListener::listenerState() const {
    return m_isConnected ? ListenerState::Connected
                         : ListenerState::Disconnected;
}
std::optional<RunStatus> FakeEventDataListener::lastTransition() const {
    return m_lastTransition;
}

void FakeEventDataListener::tickRunState() {
    m_lastTransition.reset();
    if (m_endRunEvery > 0 &&
        DateAndTime::getCurrentTime() > m_nextEndRunTime) {
        m_nextEndRunTime = DateAndTime::getCurrentTime() + m_endRunEvery;
        m_runNumber++;
        m_runState = EndRun;
        m_lastTransition = EndRun;
    } else {
        m_runState = Running;
    }
}

std::shared_ptr<Workspace> FakeEventDataListener::extractData() {
    tickRunState();
    // ... existing event-generation code ...
}
```

The deprecated `runStatus()` is removed; the base-class default
(`lastTransition().value_or(runState())`) preserves the polled API exactly.

### 3.2 `FileEventDataListener`

`runStatus()` returns the constant `Running`. Replace with:

```cpp
RunStatus runState() const override { return API::ILiveListener::Running; }
ListenerState listenerState() const override {
    return m_chunkNumber > 0 ? ListenerState::Connected
                             : ListenerState::Disconnected;
}
```

Remove the existing `runStatus()` override; fall through to base default.

### 3.3 `ISISLiveEventDataListener`

`runStatus()` returns the constant `Running`. Replace with the analogous
`runState()` override. `listenerState()` maps `m_isConnected` to the enum.
Remove the existing `runStatus()` override.

### 3.4 `ISISHistoDataListener`

`runStatus()` returns the constant `Running`. Same treatment as §3.3.

### 3.5 `KafkaEventListener` and `KafkaHistoListener`

**Current** (`KafkaEventListener.cpp:127–129`):

```cpp
ILiveListener::RunStatus KafkaEventListener::runStatus() {
    return m_decoder->hasReachedEndOfRun() ? EndRun : Running;
}
```

This is already side-effect-free, but it conflates "we are at an end-of-run
boundary right now" with "an EndRun edge has just been consumed". The v3
fix keeps the semantics identical for callers of the deprecated
`runStatus()` while introducing a clean separation:

```cpp
RunStatus runState() const override {
    return m_decoder->hasReachedEndOfRun() ? EndRun : Running;
}
ListenerState listenerState() const override {
    return m_decoder && m_decoder->isCapturing()
             ? ListenerState::Connected
             : ListenerState::Disconnected;
}
```

`lastTransition()` is not overridden (returns `nullopt`); Kafka does not
have the BeginRun/EndRun-edge-detection contract that ADARA does. Remove
the `runStatus()` override.

`KafkaHistoListener` receives identical treatment.

### 3.6 `SINQHMListener` — anti-pattern fix

**Current** (`SINQHMListener.cpp:60–98`):

```cpp
ILiveListener::RunStatus SINQHMListener::runStatus() {
    std::istream &istr = httpRequest("/admin/textstatus.egi");
    // ... parse response ...
    hmhost = daq["HM-Host"];
    // ... interpret status code ...
    if (status == 1) {
        if (oldStatus == NoRun) dimDirty = true;
        oldStatus = Running;
        return Running;
    } else if (status == 0) {
        oldStatus = NoRun;
        return NoRun;
    } else {
        throw std::runtime_error("...");
    }
}
```

`extractData()` then *calls* `runStatus()` explicitly (`SINQHMListener.cpp:104`)
to refresh `hmhost` and trigger `dimDirty`. This is the same anti-pattern.

**v3 treatment**: extract the HTTP poll into a private `pollStatus()` helper
called from `extractData()`. `runState()` returns the cached `oldStatus`.

```cpp
// header
RunStatus runState() const override;
ListenerState listenerState() const override;

private:
void pollStatus();             // contains the HTTP request and field updates
RunStatus m_cachedRunState{NoRun};
```

```cpp
// cpp
RunStatus SINQHMListener::runState() const { return m_cachedRunState; }
ListenerState SINQHMListener::listenerState() const {
    return connected ? ListenerState::Connected
                     : ListenerState::Disconnected;
}

void SINQHMListener::pollStatus() {
    // ... existing body of current runStatus(), but writing m_cachedRunState
    //     instead of returning, and continuing to write hmhost and dimDirty ...
}

std::shared_ptr<Workspace> SINQHMListener::extractData() {
    pollStatus();                          // replaces the implicit runStatus() call
    if (dimDirty) loadDimensions();
    // ... rest unchanged ...
}
```

The deprecated `runStatus()` is removed; the base-class default returns the
cached state which is precisely what every external caller historically
read.

### 3.7 `TestGroupDataListener` and `TestDataListener`

Both return constant `Running` from `runStatus()`. Apply the constant-state
template:

```cpp
RunStatus runState() const override { return API::ILiveListener::Running; }
ListenerState listenerState() const override {
    return ListenerState::Connected;
}
```

### 3.8 `MockLiveListener` (`Framework/API/test/LiveListenerTest.h`)

The mock used for `LiveListener` unit testing must implement the new
`listenerState()` (pure virtual) and should override `runState()` for
completeness. Typical mock override:

```cpp
class MockLiveListener : public ILiveListener {
public:
    RunStatus runState() const override { return Running; }
    ListenerState listenerState() const override {
        return ListenerState::Connected;
    }
    // ... other mocked methods ...
};
```

Any algorithm-level mocks (e.g. in `LoadLiveDataTest`, `MonitorLiveDataTest`,
`StartLiveDataTest`) that derive from `ILiveListener` directly need the
same two-line addition.

______________________________________________________________________

## 4. Migration ordering

To keep the tree green at every step:

1. Land the base-class additions to `ILiveListener.h` first (defaults present;
   `listenerState()` pure virtual). The tree will fail to compile until
   every concrete listener provides `listenerState()`.

1. In the same PR, add the two-line `listenerState()` (and the one-line
   `runState()`) overrides to every side-effect-free listener listed in
   §3.2–§3.5, §3.7, §3.8. Drop their existing `runStatus()` overrides.

1. In the same PR, apply the anti-pattern fixes in §3.1 (`FakeEventDataListener`)
   and §3.6 (`SINQHMListener`).

1. Apply the `SNSLiveEventDataListener` rewrite per
   `listener_refactoring_v3.md`.

This is the same single-PR approach the v3 spec recommends; the work is
small per listener (the cleanups in §3.1 and §3.6 are the only places that
require any thought).

______________________________________________________________________

## 5. Risk

- **Trivial overrides** (§3.2–§3.5, §3.7, §3.8): no behavioural change.
- **`FakeEventDataListener` refactor** (§3.1): used only by `FakeEventDataListenerTest`
  and as a teaching example in `LoadLiveData` integration tests. The
  externally-observable cadence of `EndRun` and the `runNumber` increment is
  preserved; only the trigger moves from `runStatus()` polling to
  `extractData()` invocation.
- **`SINQHMListener` refactor** (§3.6): the `extractData()` body already
  triggers the side effects (line 104), so callers that only invoke
  `extractData()` see no change. Callers that polled `runStatus()` for the
  side effects (HTTP refresh of `hmhost`, `dimDirty` reset) would see no
  refresh — but the in-tree caller does not exist; only `MonitorLiveData`
  calls `runStatus()`, and it does so after `extractData()`, so the cached
  value matches what the legacy code would have returned.

______________________________________________________________________

## 6. Verification

For every modified listener:

- The listener's existing unit tests must pass unchanged.
- `clang-tidy` on each modified file.
- `pre-commit run --files <modified files>`.

For `FakeEventDataListener` and `SINQHMListener` specifically:

- Add a unit test asserting that two consecutive `runState()` calls without
  an intervening `extractData()` return the same value and do not mutate
  any other member (the anti-pattern regression test).
