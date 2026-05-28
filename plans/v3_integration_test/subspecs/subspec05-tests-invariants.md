# Sub-spec 05 — Invariant tests (C1 fix + single-slot back-pressure + pause/resume)

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 5 of 6. Adds **8** test methods to the fixture, covering
the v3-refactor invariants that are *visible* over the wire: the C1
fix, the single-slot back-pressure invariant, consecutive-NEW_RUN
malformed-stream detection, and pause/resume orthogonality (including
the `keepPausedEvents` config gate).

| Section | Area                                      | Tests in this commit |
| ------- | ----------------------------------------- | -------------------- |
| 6.4     | C1 fix — `lastTransition` survives NotYet | 2                    |
| 6.5     | Back-pressure / consecutive-NEW_RUN error | 3                    |
| 6.7     | Pause / resume                            | 3                    |

§6.6 (`m_deferredRunDetailsPkt`) is **deliberately not** in this
commit — it is delegated to `SNSLiveEventDataListenerNoNetworkTest.h`
and only referenced from the file-header doc comment that was added in
`subspec03`.

Total: 8 new `test_*` methods added to
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
   **Update (post-subspec05):** Consecutive NEW_RUN packets without an
   intervening END_RUN are now detected earlier, at the deferred-packet
   stash site in `rxPacket(RunStatusPkt)` (`:706-725`), where
   `haveRunNumber==true` triggers a `std::runtime_error`. The bg-thread
   catch at `:318-326` stores it in `m_backgroundException`;
   `doExtractData()` re-throws at `:1444-1450`. This path IS reachable
   over a real socket and is exercised by the two §5.1 integration tests
   (`test_consecutiveNewRun_surfacesRuntimeError` and
   `test_repeatedNewRun_sameRunNumber_surfacesRuntimeError`). NotYet is
   the normal-operation signal for "workspace not yet initialised" — not
   a consequence of consecutive NEW_RUNs — and is covered by the
   redesigned §6.4 tests.
1. **Production code change required.** `SNSLiveEventDataListener.cpp`
   `:706-725` — the silent warning was promoted to `std::runtime_error`
   to make the consecutive-NEW_RUN path surface cleanly.
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

## 4. §6.4 — C1 fix + NotYet behaviour (2 tests)

NotYet is the normal-operation signal for "workspace not yet
initialised": `doExtractData()` polls `m_workspaceInitialized` for
10 s and throws if still false. The usual trigger is receiving NEW_RUN
before Geometry / Beamline arrive, or receiving a new BeginRun-dispatch
before new geometry arrives after `onBeginRun()` has cleared the flag.

### 4.1 `test_lastTransition_preservedAcrossNotYet`

**Purpose:** The C1 structural guarantee
(`API::LiveListener::extractData()`, `Framework/API/src/LiveListener.cpp:16-21`):
when `doExtractData()` throws NotYet, `onAfterExtract()` is NOT
invoked, so the `m_lastTransition` reset at
`SNSLiveEventDataListener.cpp:1566` is bypassed and the slot set in
`onBeforeExtract()` at `:1555` survives.

Drive via the realistic SMS connect handshake: `STATE(runNumber=0)`
(the RunStatus packet SMS sends on initial connect; comment at
`:778-787`) plus Geometry + Beamline initialises the workspace without
setting `run_number` (`haveRunNumber==false`). `NEW_RUN(10)` then hits
the normal branch (`m_workspaceInitialized==true`, `!haveRunNumber`) →
`m_pendingTransition=BeginRun`; `m_deferredRunDetailsPkt` stashed.
First extract dispatches `onBeginRun()` (succeeds), sets
`m_lastTransition=BeginRun`, clears `m_workspaceInitialized`.
`doExtractData()` polls 10 s — no new geometry while gate 1 holds —
throws NotYet. C1: `onAfterExtract()` not called → `m_lastTransition`
stays `BeginRun`. Gate 1 released; new Geometry+Beamline+event arrive;
second extract succeeds.

**Script:**

```cpp
m_server->script({
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildRunStatusPkt(ADARA::RunStatus::STATE, 0,
                                0x0000000100000000ULL), // SMS connect handshake
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                0x0000000200000000ULL), // normal → m_pendingTransition=BeginRun
    Testing::PktWaitForExtract{},                       // gate 1 (index 4 → scriptIndex 5)
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildBankedEventPkt(0x0000000300000000ULL, 1000.0, {{100u, 1u}}),
    Testing::PktWaitForExtract{},  // gate 2 (index 8 → scriptIndex 9)
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
1. First extract with 15 s timeout (internal 10 s poll fires first):
   ```cpp
   try { extractWithTimeout(*m_listener, std::chrono::seconds{15}); }
   catch (const std::exception &) { /* NotYet — expected */ }
   ```
1. Assert `m_lastTransition == BeginRun`.
1. `m_server->releaseExtractGate();` *(gate 1)*
1. `waitFor([&]{ return m_server->scriptIndex() >= 9; }, std::chrono::seconds{5});`
1. `auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();` *(gate 2)*

**Assertions:**

```cpp
TS_ASSERT(m_listener->lastTransition().has_value());
TS_ASSERT_EQUALS(*m_listener->lastTransition(), API::ILiveListener::BeginRun);
TS_ASSERT_DIFFERS(ws2, nullptr);
```

### 4.2 `test_notYet_whenGeometryDelayed` *(new)*

**Purpose:** Verify that the listener correctly throws NotYet when
NEW_RUN arrives before Geometry / Beamline, that it remains in a sane
state (no back-pressure, no stored error), and that it recovers
cleanly when Geometry subsequently arrives.

**Script:**

```cpp
m_server->script({
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 20,
                                0x0000000100000000ULL), // before geometry
    Testing::PktWaitForExtract{},                       // gate 1 (index 1 → scriptIndex 2)
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0, {{100u, 1u}}),
    Testing::PktWaitForExtract{},  // gate 2 (index 5 → scriptIndex 6)
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});`
1. First extract with 15 s timeout — must throw NotYet.
1. Assert `listenerState() != Error`.
1. `m_server->releaseExtractGate();` *(gate 1)*
1. `waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});`
1. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
1. `m_server->releaseExtractGate();` *(gate 2)*

**Assertions:**

```cpp
TSM_ASSERT("first extract must throw NotYet when Geometry has not yet arrived", gotNotYet);
TS_ASSERT_DIFFERS(m_listener->listenerState(), API::ListenerState::Error);
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 1);
```

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

### 5.1 `test_consecutiveNewRun_surfacesRuntimeError`

**Purpose:** Two consecutive NEW_RUN packets (different run numbers)
without an intervening END_RUN are detected at the deferred-packet
stash site in `rxPacket(RunStatusPkt)` (`:706-725`): the first NEW_RUN
takes the white-lie path (sets `run_number` via `setRunDetails()`);
the second hits the normal branch with `haveRunNumber==true` and throws
`std::runtime_error`. The bg-thread catch at `:318-326` stores it in
`m_backgroundException`; `doExtractData()` re-throws at `:1444-1450`.
The listener must not wedge: `m_pauseNetRead` is NOT set before the
throw (`:729-731` follows `:706-725`), so a second extract re-throws
the stored exception promptly.

**Script:**

```cpp
m_server->script({
    Testing::buildGeometryPkt(kMinimalIDF()),
    Testing::buildBeamlineInfoPkt(kInstrumentName),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                0x0000000100000000ULL), // white-lie → inits ws
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 11,
                                0x0000000200000000ULL), // throws at :706-725
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**

1. `TS_ASSERT(connectListener());`
1. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
1. 200 ms sleep to allow bg thread to process NEW_RUN(11) and store the exception.

**Assertions:**

```cpp
TS_ASSERT_THROWS(extractWithTimeout(*m_listener, std::chrono::seconds{5}),
                 const std::runtime_error &);
// Second call must also re-throw promptly (not block on m_pauseNetRead).
TS_ASSERT_THROWS(extractWithTimeout(*m_listener, std::chrono::seconds{2}),
                 const std::runtime_error &);
```

### 5.1b `test_repeatedNewRun_sameRunNumber_surfacesRuntimeError` *(new)*

**Purpose:** Same scenario as §5.1 but with the same run number
repeated — the operational case of an operator restarting the same run
without an intervening END_RUN. Verifies the detection is independent
of whether the run numbers match.

**Script:** identical to §5.1 but with run number 10 repeated for both
NEW_RUN packets.

**Assertions:** `TS_ASSERT_THROWS(... const std::runtime_error &)` (single check).

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

- [x] `Framework/LiveData/src/SNSLiveEventDataListener.cpp` —
  `:706-725` silent warning promoted to `std::runtime_error` (detects
  consecutive NEW_RUN without END_RUN at packet-parse time).
- [x] `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` —
  8 test methods appended after subspec04, in this order:
  1\. `test_lastTransition_preservedAcrossNotYet` (§4.1, rewritten with STATE(0) handshake)
  2\. `test_notYet_whenGeometryDelayed` (§4.2, new)
  3\. `test_consecutiveNewRun_surfacesRuntimeError` (§5.1, renamed from doubleBeginRun)
  4\. `test_repeatedNewRun_sameRunNumber_surfacesRuntimeError` (§5.1b, new)
  5\. `test_newRunEndRun_backPressureProducesReadWaitThenCleanEndRun` (§5.2, unchanged)
  6\. `test_pauseResume_orthogonalToRunState` (§6.1)
  7\. `test_pausedEvents_droppedByDefault` (§6.2)
  8\. `test_pausedEvents_keptWhenConfigured` (§6.3, known failure — deferred)
- [x] `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h` —
  two unit tests exercise the production single-slot throws via `callBufferParse()`:
  - `test_rxRunStatusPkt_newRun_throws_when_slot_occupied`
  - `test_rxRunStatusPkt_endRun_throws_when_slot_occupied`
- [ ] Confirm every test uses `connectListener()`, `waitFor()`, and
  `extractWithTimeout(*m_listener, ...)` per `subspec03`.
- [ ] Confirm `test_pausedEvents_keptWhenConfigured` sets the config
  key **before** `connectListener()` (so it is in effect when the
  listener constructs).
- [x] Production code change required at `:706-725` (see §0.6 note).

______________________________________________________________________

## 8. Definition of done for this commit

1. `SNSLiveEventDataListener.cpp` `:706-725` — `haveRunNumber` branch
   throws `std::runtime_error` instead of logging a warning.
1. `SNSLiveEventDataListenerTest.h` contains the 8 `test_*` methods
   listed in §7 with the names as given there.
1. `SNSLiveEventDataListenerNoNetworkTest.h` contains
   `test_rxRunStatusPkt_newRun_throws_when_slot_occupied` and
   `test_rxRunStatusPkt_endRun_throws_when_slot_occupied`, which
   exercise the production throws at `:651-655` and `:744-748`
   via `TestableSNSListener::callBufferParse()`.
1. The fixture's `setUp` / `tearDown` from `subspec03` (which
   save/restore `SNSLiveEventDataListener.keepPausedEvents`) are
   unchanged.
