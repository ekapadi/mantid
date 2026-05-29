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
1. **Ambiguity protocol (§5 resolved).** The two §6.5 tests were
   originally written expecting either an exception or `Error`
   `listenerState()`. Analysis confirmed that the production
   single-slot throws at `SNSLiveEventDataListener.cpp:651-655` and
   `:744-748` are **unreachable over a real socket**: the same
   `rxPacket(RunStatusPkt)` call that occupies `m_pendingTransition`
   also sets `m_pauseNetRead=true`, causing `bufferParse()` to stop
   parsing further packets before the second RunStatus packet is read.
   The two tests have been **redesigned** to verify the observable
   back-pressure consequences instead; the production throws are
   exercised by unit tests in `SNSLiveEventDataListenerNoNetworkTest`
   that feed raw packet bytes through the parser with pre-injected
   state (see `§5` below for details).
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
- **Back-pressure observables** (see `§5`): the single-slot
  pending-transition invariant throws at
  `SNSLiveEventDataListener.cpp:651-655` and `:744-748` are
  unreachable over a real socket (back-pressure via `m_pauseNetRead`
  prevents the second packet from being parsed). The integration tests
  verify the *observable consequences* instead: the back-pressure
  cascade that surfaces as an error (`§5.1`) and the `ReadWait` state
  followed by a clean `EndRun` extract (`§5.2`). The production
  throws are covered by `SNSLiveEventDataListenerNoNetworkTest:: test_rxRunStatusPkt_newRun_throws_when_slot_occupied()` and
  `test_rxRunStatusPkt_endRun_throws_when_slot_occupied()`.
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

## 5. §6.5 — Back-pressure observables (2 tests)

The production single-slot invariant throws at
`SNSLiveEventDataListener.cpp:651-655` (NEW_RUN-side) and `:744-748`
(END_RUN-side) are **unreachable over a real socket**. Both throws
require `m_pendingTransition.has_value()` on entry, but the only calls
that set `m_pendingTransition` (normal-branch NEW_RUN at `:657`, END_RUN
at `:749`) also set `m_pauseNetRead=true` (`:730`, `:772`) in the same
`rxPacket(RunStatusPkt)` invocation. `rxPacket` returns `m_pauseNetRead`
(`:799`); `ADARA::Parser::bufferParse()` stops parsing when `rxPacket`
returns true (guards at `ADARAParser.cpp:111,177`); and the bg-read
loop sleeps at `SNSLiveEventDataListener.cpp:251-256` until
`extractData()` clears the flag. The production code's own comment at
`:737-740` confirms this.

The production throws are covered by unit tests in
`SNSLiveEventDataListenerNoNetworkTest` (see
`test_rxRunStatusPkt_newRun_throws_when_slot_occupied()` and
`test_rxRunStatusPkt_endRun_throws_when_slot_occupied()`) that bypass
the socket by feeding raw packet bytes through the inherited
`ADARA::Parser::bufferParse()` with state pre-injected via
`TestableSNSListener` helpers.

The two integration tests below verify the *observable consequences*
of the back-pressure mechanism instead.

### 5.1 `test_doubleBeginRun_surfacesError_viaBackPressureCascade`

**Purpose:** Two consecutive NEW_RUNs without an intervening
`extractData()` surface as an error via the back-pressure cascade:
NEW_RUN(1) takes the white-lie path (workspace not yet initialised)
and initialises the workspace; NEW_RUN(2) takes the normal path
(`m_workspaceInitialized=true`) and queues `BeginRun`.
`extractData()` dispatches `onBeginRun()`, which clears geometry and
instrument state; with no new geometry arriving, `doExtractData()`
then polls for 10 s and throws `NotYet`. Either a thrown exception
or a `ListenerState::Error` surface is accepted.

The single-slot invariant throw at `:651-655` is NOT what fires here
(see preamble above). The error surfaces via the cascade described.

**Script:**

```cpp
m_server->script({
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 1,
                                0x0000000100000000ULL), // white-lie → inits ws
    Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}}),
    // No extractData() gate — second NEW_RUN arrives without prior extract.
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 2,
                                0x0000000200000000ULL), // normal → m_pendingTransition=BeginRun
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
   (All packets including second NEW_RUN sent.)
1. 100 ms sleep to allow bg thread to parse the packets.
1. Extract with 15 s timeout + try/catch: the 15 s window lets the
   internal 10 s NotYet poll fire first and propagate via `fut.get()`
   without triggering `TS_FAIL`.

**Assertions:**

```cpp
bool gotError = false;
try {
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{15});
    gotError = (ws == nullptr) || (m_listener->listenerState() ==
                                   API::ListenerState::Error);
} catch (const std::exception &) { gotError = true; }
TSM_ASSERT("double-NEW_RUN without intervening extract must surface an error "
           "(onBeginRun clears geometry state; NotYet after 10 s poll)",
           gotError);
```

### 5.2 `test_newRunEndRun_backPressureProducesReadWaitThenCleanEndRun`

**Purpose:** Verify the observable consequence of white-lie NEW_RUN +
END_RUN: the END_RUN sets `m_pauseNetRead=true` (since no pending
BeginRun is consumed first), producing `listenerState()==ReadWait`;
the subsequent `extractData()` succeeds and reports `EndRun`.

The single-slot invariant throw at `:744-748` is NOT triggered (see
preamble above): `m_pendingTransition` is `nullopt` when END_RUN
arrives because the preceding NEW_RUN took the white-lie path, which
does not set `m_pendingTransition`.

**Script:**

```cpp
m_server->script({
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 3,
                                0x0000000100000000ULL),
    Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 3,
                                0x0000000200000000ULL),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_listener->listenerState() == API::ListenerState::ReadWait; }, std::chrono::seconds{5});`
   (poll for the observable back-pressure consequence of END_RUN)
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`

**Assertions:**

```cpp
TS_ASSERT_EQUALS(m_listener->listenerState(), API::ListenerState::ReadWait);
TS_ASSERT_DIFFERS(ws, nullptr);
TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::EndRun);
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

- [x] `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` —
  6 test methods appended after subspec04, in this order:
  1\. `test_lastTransition_preservedAcrossNotYet` (§4.1)
  2\. `test_doubleBeginRun_surfacesError_viaBackPressureCascade` (§5.1)
  3\. `test_newRunEndRun_backPressureProducesReadWaitThenCleanEndRun` (§5.2)
  4\. `test_pauseResume_orthogonalToRunState` (§6.1)
  5\. `test_pausedEvents_droppedByDefault` (§6.2)
  6\. `test_pausedEvents_keptWhenConfigured` (§6.3)
- [x] `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h` —
  two new unit tests added to exercise the production single-slot throws
  directly via `callBufferParse()`:
  - `test_rxRunStatusPkt_newRun_throws_when_slot_occupied`
  - `test_rxRunStatusPkt_endRun_throws_when_slot_occupied`
- [ ] Confirm every test uses `connectListener()`, `waitFor()`, and
  `extractWithTimeout(*m_listener, ...)` per `subspec03`.
- [ ] Confirm `test_pausedEvents_keptWhenConfigured` sets the config
  key **before** `connectListener()` (so it is in effect when the
  listener constructs).
- [ ] Confirm no production code changes outside
  `Framework/LiveData/test/`.

______________________________________________________________________

## 8. Definition of done for this commit

1. `SNSLiveEventDataListenerTest.h` contains the 6 `test_*` methods
   listed in §7 with the names as given there.
1. `SNSLiveEventDataListenerNoNetworkTest.h` contains
   `test_rxRunStatusPkt_newRun_throws_when_slot_occupied` and
   `test_rxRunStatusPkt_endRun_throws_when_slot_occupied`, which
   exercise the production throws at `:651-655` and `:744-748`
   via `TestableSNSListener::callBufferParse()`.
1. The fixture's `setUp` / `tearDown` from `subspec03` (which
   save/restore `SNSLiveEventDataListener.keepPausedEvents`) are
   unchanged.
1. No production code changes.
