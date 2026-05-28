# Sub-spec 05 — Invariant tests (C1 fix + single-slot back-pressure + pause/resume)

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 5 of 6. Adds **6** test methods to the fixture, covering
the v3-refactor invariants that are *visible* over the wire: the C1
fix, the single-slot pending-transition invariant, and pause/resume
orthogonality (including the `keepPausedEvents` config gate).

| Section | Area                                      | Tests in this commit |
| ------- | ----------------------------------------- | -------------------- |
| 6.4     | C1 fix — `lastTransition` survives NotYet | 1                    |
| 6.5     | Back-pressure single-slot invariant       | 2                    |
| 6.7     | Pause / resume                            | 3                    |

§6.6 (`m_deferredRunDetailsPkt`) is **deliberately not** in this
commit — it is delegated to `SNSLiveEventDataListenerNoNetworkTest.h`
and only referenced from the file-header doc comment that was added in
`subspec03`.

Total: 6 new `test_*` methods added to
`Framework/LiveData/test/SNSLiveEventDataListenerTest.h`, appended
after the methods from `subspec04`.

______________________________________________________________________

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface__agents`.
1. **Scope fence.** Edit exactly **one** file:
   `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`. Do not
   modify CMake, `MockSMSServer.h/.cpp`, the legacy header, or any
   file under `Framework/LiveData/src/` / `Framework/LiveData/inc/`.
1. **Static verification only — DO NOT build, DO NOT run tests.**
   Cross-check the cited line ranges in
   `Framework/LiveData/src/SNSLiveEventDataListener.cpp` against the
   purpose statements. The single-slot invariant is at lines 651–656
   and 743–750; the C1 guarantee is structural (see §1 and §4.1 below
   for the precise references); pause/resume at 1629–1635 and 96–99,
   405\.
1. **No build artefacts in the PR.**
1. **No `TestableSNSListener`.** Drive the real listener via
   `connectListener()`.
1. **Ambiguity protocol.** The two §6.5 tests intentionally accept
   *either* an exception from `extractData()` *or* an `Error`
   `listenerState()` as the failure surface — see §5 below. Do not
   tighten this. If you find the production behaviour surfaces neither,
   stop and surface it in the PR description.
1. **No production code changes.**
1. **One commit, all 6 tests.**

______________________________________________________________________

## 1. Goal of this commit

Make the integration suite verify the three v3-refactor invariants
that are observable over the wire:

- **C1 fix** — structural guarantee: `API::LiveListener::extractData()`
  (`Framework/API/src/LiveListener.cpp:16-21`) does not call
  `onAfterExtract()` when `doExtractData()` throws `NotYet`, so the
  reset at `SNSLiveEventDataListener.cpp:1566` is bypassed. The
  `m_lastTransition` slot set in `onBeforeExtract()` at `:1555`
  therefore survives the NotYet path; a subsequent successful extract
  still reports the original `BeginRun` transition.
- **Single-slot pending-transition invariant**
  (`SNSLiveEventDataListener.cpp:651-656, 743-750`): two consecutive
  RunStatus transitions without an intervening `extractData()` must
  surface as an error.
- **Pause/resume orthogonality**
  (`SNSLiveEventDataListener.cpp:1629-1635` and 96–99, 405):
  `runState()` is unchanged by pause / resume annotations; the `pause`
  log property is populated; the `keepPausedEvents` config key toggles
  retention of mid-pause events.

______________________________________________________________________

## 2. Files touched in this commit

| Action | Path                                                                                                           |
| ------ | -------------------------------------------------------------------------------------------------------------- |
| Edit   | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (append 6 `test_*` methods inside the existing class) |

______________________________________________________________________

## 3. Conventions (same as `subspec04`)

The shorthand `PKT(name)`, `BLD_RUN(...)`, `BLD_BANKED(...)`,
`BLD_VAR_U32(...)`, `BLD_VAR_DBL(...)` is exactly as defined in
`subspec04` §3. The `PKT(name)` macro is already in the test file from
`subspec03` §4.2.

______________________________________________________________________

## 4. §6.4 — C1 fix (1 test)

### 4.1 `test_lastTransition_preservedAcrossNotYet`

**Purpose:** `extractData()` on the NotYet path (workspace not yet
initialised — Geometry not yet received) must not clobber
`m_lastTransition`. The guarantee is structural:
`API::LiveListener::extractData()` (`Framework/API/src/LiveListener.cpp:16-21`,
contract documented at `Framework/API/inc/MantidAPI/LiveListener.h:37-46`)
does not invoke `onAfterExtract()` when `doExtractData()` throws
`NotYet`, so the reset at `SNSLiveEventDataListener.cpp:1566` is
bypassed and the slot set by the prior `onBeforeExtract()` at
`SNSLiveEventDataListener.cpp:1555` survives. After a subsequent
extract that *does* succeed, `lastTransition()` must reflect the
`BeginRun` transition, not a stale / default value.

**Script:**

```cpp
m_server->script({
    // Deliberately omit Geometry here so first extract() takes NotYet path.
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 88,
                                0x0000000100000000ULL),
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},  // gate 1: first extract (NotYet)
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},  // gate 2: second extract (succeeds)
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});`
   (NEW_RUN and banked event sent before first extract.)
1. First extract: `auto ws1 = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
   This may return `nullptr` (NotYet path) — that is expected.
1. `m_server->releaseExtractGate();` *(release gate 1)*
1. `waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});`
   (Geometry + BeamlineInfo + events sent.)
1. Second extract: `auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();` *(release gate 2)*

**Assertions:**

```cpp
TS_ASSERT_DIFFERS(ws2, nullptr);
// lastTransition must be BeginRun — not reset to None by the NotYet path.
TS_ASSERT_EQUALS(m_listener->lastTransition(),
                 API::ILiveListener::RunTransition::BeginRun);
```

(`ws1` is *not* asserted — its value depends on the production
listener's choice of returning `nullptr` vs an empty workspace on the
NotYet path, and either is acceptable behaviour. The invariant under
test is solely about `lastTransition()` after the second extract.)

______________________________________________________________________

## 5. §6.5 — Back-pressure single-slot invariant (2 tests)

Covers `SNSLiveEventDataListener.cpp:651-656, 743-750`.

### 5.1 `test_doubleBeginRun_violatesSingleSlot_throws`

**Purpose:** Sending NEW_RUN(1) followed by NEW_RUN(2) **without** an
intervening `extractData()` violates the single-slot pending-transition
invariant and must surface as an error.

**Script:**

```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 1,
                                0x0000000100000000ULL),
    PKT(bankedEventPacketV1),
    // No extractData() gate here — second NEW_RUN arrives immediately.
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 2,
                                0x0000000200000000ULL),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
   (All packets including second NEW_RUN sent.)
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`

**Assertions:**

```cpp
// The second NEW_RUN without an intervening extract must trigger an
// error.  Either extractData() throws, or runStatus() reports an
// error state.  Accept either form — the exact surface depends on
// which path fires first in production.
bool gotError = (ws == nullptr);
if (!gotError) {
    gotError = (m_listener->listenerState() ==
                API::ILiveListener::ListenerState::Error);
}
TSM_ASSERT("Expected error from double-NEW_RUN without intervening extract",
            gotError);
```

### 5.2 `test_endRunWhilePending_violatesSingleSlot_throws`

**Purpose:** Sending NEW_RUN followed immediately by END_RUN without
an intervening `extractData()` violates the single-slot invariant.

**Script:**

```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 3,
                                0x0000000100000000ULL),
    // No extractData() gate — END_RUN arrives immediately.
    Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 3,
                                0x0000000200000000ULL),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});`
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`

**Assertions:**

```cpp
bool gotError = (ws == nullptr);
if (!gotError) {
    gotError = (m_listener->listenerState() ==
                API::ILiveListener::ListenerState::Error);
}
TSM_ASSERT("Expected error from END_RUN without prior extract", gotError);
```

______________________________________________________________________

## 6. §6.7 — Pause / resume (3 tests)

Covers `SNSLiveEventDataListener.cpp:1629-1635` and `:96-99, 405`.

### 6.1 `test_pauseResume_orthogonalToRunState`

**Purpose:** `runState()` remains `Running` across a PAUSE / RESUME
annotation sequence; the `pause` log property is populated correctly.

**Script:**

```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                0x0000000100000000ULL),
    PKT(bankedEventPacketV1),
    PKT(AnnotationPacketType3),   // Pause
    PKT(bankedEventPacketV1),
    PKT(AnnotationPacketType4),   // Resume
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 8; }, std::chrono::seconds{5});`
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();`

**Assertions:**

```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
// Run state must still be Running after pause/resume.
TS_ASSERT_EQUALS(m_listener->runStatus(),
                 API::ILiveListener::Running);
// The 'pause' time series must have been populated.
const auto &run = ws->run();
TS_ASSERT(run.hasProperty("pause"));
```

### 6.2 `test_pausedEvents_droppedByDefault`

**Purpose:** With `keepPausedEvents=false` (default), events between
PAUSE and RESUME are absent from the extracted workspace.

**Script:**

```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 11,
                                0x0000000100000000ULL),
    // Pre-pause event:
    Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0,
                                  {{100u, 1u}}),
    PKT(AnnotationPacketType3),   // Pause
    // Events during pause (must be dropped):
    Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0,
                                  {{200u, 2u}, {300u, 3u}}),
    PKT(AnnotationPacketType4),   // Resume
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. Ensure `SNSLiveEventDataListener.keepPausedEvents` is unset
   (the fixture saves/restores this in `setUp` / `tearDown` —
   default is `false`).
1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});`
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();`

**Assertions:**

```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
// Only 1 pre-pause event; the 2 mid-pause events must be absent.
TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 1);
```

### 6.3 `test_pausedEvents_keptWhenConfigured`

**Purpose:** With `keepPausedEvents=true`, events between PAUSE and
RESUME are retained in the extracted workspace.

**Script:** same as §6.2.

**Steps:**

1. **Before** `connectListener()`: set the config key:
   ```cpp
   Kernel::ConfigService::Instance().setString(
       "SNSLiveEventDataListener.keepPausedEvents", "1");
   ```
1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});`
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();`

**Assertions:**

```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
// All 3 events (1 pre-pause + 2 mid-pause) must be present.
TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 3);
```

(The fixture's `tearDown()` already restores
`SNSLiveEventDataListener.keepPausedEvents` from the value captured in
`setUp()`, so no extra cleanup is needed here.)

______________________________________________________________________

## 7. TODO

- [ ] Open `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.
- [ ] Append the 6 `test_*` methods inside the existing class, after
  the methods added in `subspec04`, in this order:
  1\. `test_lastTransition_preservedAcrossNotYet` (§4.1)
  2\. `test_doubleBeginRun_violatesSingleSlot_throws` (§5.1)
  3\. `test_endRunWhilePending_violatesSingleSlot_throws` (§5.2)
  4\. `test_pauseResume_orthogonalToRunState` (§6.1)
  5\. `test_pausedEvents_droppedByDefault` (§6.2)
  6\. `test_pausedEvents_keptWhenConfigured` (§6.3)
- [ ] Confirm every test uses `connectListener()`, `waitFor()`, and
  `extractWithTimeout(*m_listener, ...)` per `subspec03`.
- [ ] Confirm the two §5 tests accept *either* a `nullptr` result *or*
  a `ListenerState::Error` — do not tighten to a single surface.
- [ ] Confirm `test_pausedEvents_keptWhenConfigured` sets the config
  key **before** `connectListener()` (so it is in effect when the
  listener constructs).
- [ ] Confirm no edits outside
  `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.
- [ ] Confirm no `TestableSNSListener` reference is introduced.

______________________________________________________________________

## 8. Definition of done for this commit

1. `SNSLiveEventDataListenerTest.h` contains the 6 new `test_*` methods
   listed in §7, appended after the methods from `subspec04`.
1. The fixture's `setUp` / `tearDown` from `subspec03` (which
   save/restore `SNSLiveEventDataListener.keepPausedEvents`) are
   unchanged.
1. No file outside `SNSLiveEventDataListenerTest.h` is modified.
1. No `TestableSNSListener` reference is introduced.
