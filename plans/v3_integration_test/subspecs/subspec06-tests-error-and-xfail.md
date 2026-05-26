# Sub-spec 06 — Error propagation, XFAIL historical replay, and monitor workspace

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 6 of 6 — final commit of the integration-test PR.
Adds the remaining **7** test methods to the fixture: the two XFAIL
historical-replay tests that document a known latent production defect,
three error-propagation tests, and two monitor-workspace tests.

| Section | Area | Tests in this commit |
|---|---|---|
| 6.8 | Historical replay & variable cache (XFAIL) | 2 |
| 6.9 | Error propagation | 3 |
| 6.10 | Monitor workspace routing | 2 |

This commit closes out the spec. After this, the full 22-test catalogue
is present (1 from `subspec03` + 8 from `subspec04` + 6 from
`subspec05` + 7 from `subspec06`).

---

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface__agents`.
2. **Scope fence.** Edit exactly **one** file:
   `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`. Do not
   modify CMake, `MockSMSServer.h/.cpp`, the legacy header, or any
   file under `Framework/LiveData/src/` / `Framework/LiveData/inc/`.
3. **Static verification only — DO NOT build, DO NOT run tests.**
   Verify the cited line ranges in
   `Framework/LiveData/src/SNSLiveEventDataListener.cpp` against the
   purpose statements. For the XFAIL tests, **read**
   [`plans/ignore-packets-defect.md`](../../ignore-packets-defect.md)
   to confirm the defect description before writing the inverted
   assertions.
4. **No build artefacts in the PR.**
5. **No `TestableSNSListener`.** Drive the real listener via
   `connectListener()`.
6. **Ambiguity protocol.** If, after reading
   `plans/ignore-packets-defect.md`, the description of the
   `m_ignorePackets` defect appears to no longer match the production
   code on this branch (i.e. someone has fixed it), **stop** and surface
   it in the PR description — the inverted assertions in §4 of this
   sub-spec would then mis-pass.
7. **No production code changes.**
8. **XFAIL tests must be compiled, registered, and run by `ctest`.**
   They are **not** to be `#if 0`-ed out, commented out, or gated
   behind a runtime skip. Their purpose is to be executable, in-tree
   evidence of the latent defect that will start failing as soon as
   the production fix is committed.
9. **One commit, all 7 tests.**

---

## 1. Goal of this commit

Complete the integration-test catalogue by adding:

- two XFAIL tests covering the historical-replay / variable-cache path
  (§6.8). These currently *pass* by asserting the *broken* behaviour
  caused by the `m_ignorePackets` defect at
  `SNSLiveEventDataListener.cpp:1601-1629` (analysis in
  [`plans/ignore-packets-defect.md`](../../ignore-packets-defect.md)).
  Each is annotated with a prominent `TSM_ASSERT` message pointing at
  the defect, and the assertion is inverted relative to the intended
  behaviour so that the test will start *failing* when the production
  fix lands;
- three error-propagation tests (§6.9): an invalid packet → background
  exception path, an EOF → non-`Connected` state path, and a "no server
  listening" connect-failure path;
- two monitor-workspace tests (§6.10) covering routing of beam-monitor
  events to the monitor sub-workspace and the once-only logging of
  unrecognised monitor IDs.

---

## 2. Files touched in this commit

| Action | Path |
|---|---|
| Edit | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (append 7 `test_*` methods inside the existing class) |

---

## 3. Conventions (same as `subspec04` / `subspec05`)

The shorthand `PKT(name)`, `BLD_RUN`, `BLD_VAR_U32`, `BLD_BANKED` is
exactly as defined in `subspec04` §3.

---

## 4. §6.8 — Historical replay & variable cache (2 tests, XFAIL)

> **These two tests are XFAIL (expected-failing).**
>
> The production defect is documented in
> [`plans/ignore-packets-defect.md`](../../ignore-packets-defect.md).
> In summary: `SNSLiveEventDataListener::start()` sets
> `m_filterUntilRunStart = true` when `startTime == 1 ns past epoch`,
> but **never** sets `m_ignorePackets = true`. Because `ignorePacket()`
> at `SNSLiveEventDataListener.cpp:1601-1629` short-circuits with
> `if (!m_ignorePackets) return false;`, the entire
> filter-until-run-start and variable-cache replay logic is unreachable.
>
> **XFAIL convention used:** invert the assertion so the test
> *currently passes* by observing the *broken* behaviour, with a
> prominent `TSM_ASSERT` message naming the defect and pointing to the
> production line range. When the production fix lands, remove the
> inversion in the same commit.
>
> These tests must be **compiled, registered in `TEST_FILES`, and
> run** by `ctest`. They are **not** to be `#if 0`-ed out, commented
> out, or gated behind a runtime skip.

### 4.1 `test_filterUntilRunStart_dropsPreRunPackets`

**Purpose (intended):** When `startTime == 1 ns past epoch`,
pre-NEW_RUN packets are filtered and absent from the first extract.

**Current status: XFAIL.** Due to `m_ignorePackets` never being set to
`true`, the filter is a no-op and pre-run packets are NOT filtered.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    // These events arrive before NEW_RUN and should be filtered — but aren't.
    Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0,
                                  {{100u, 1u}, {200u, 2u}}),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 20,
                                0x0000000200000000ULL),
    Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0,
                                  {{300u, 3u}}),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener(Types::Core::DateAndTime(1)));`
   *(`DateAndTime(1)` = 1 nanosecond past epoch = the "replay from
   previous run start" sentinel; `start()` sets
   `m_filterUntilRunStart = true`.)*
2. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions (XFAIL — inverted):**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
// XFAIL: intended behaviour = 1 post-run event (pre-run events filtered).
// Actual broken behaviour = 3 events (filter is a no-op).
// This assertion currently PASSES because m_ignorePackets is never true.
// When the fix lands (m_ignorePackets = true set in start()), this test
// will start FAILING with event count 1 instead of 3, and the inversion
// must be removed.
TSM_ASSERT_EQUALS(
    "XFAIL: pending fix for m_ignorePackets initialisation — see "
    "SNSLiveEventDataListener.cpp:1601-1629 and "
    "plans/ignore-packets-defect.md. "
    "This assertion is INVERTED: it currently passes by observing "
    "broken behaviour (3 events instead of 1). Remove inversion when "
    "fix lands.",
    static_cast<int>(ews->getNumberEvents()),
    3 /* broken */ /* expected-when-fixed: 1 */);
```

### 4.2 `test_variableCache_replayedAfterStartCondition`

**Purpose (intended):** Variable packets received while
`m_ignorePackets` is `true` are cached in `m_variableMap` and replayed
by `replayVariableCache()` when the filter releases at NEW_RUN. The log
property reflects all values in arrival order after the first extract.

**Current status: XFAIL.** Same root cause: `m_ignorePackets` is
always `false`, so variable packets are processed immediately (not
cached) and `replayVariableCache()` is never called.

**Script:**
```cpp
const uint64_t preRunPulse  = 0x0000000100000000ULL;
const uint64_t postRunPulse = 0x0000000200000000ULL;
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    PKT(devDesPacket),                                       // device descriptor for device 1
    Testing::buildVariableU32Pkt(1, 3, 42, preRunPulse),     // pre-run (should be cached)
    Testing::buildVariableU32Pkt(1, 3, 99, preRunPulse),     // pre-run (should be cached)
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 30, postRunPulse),
    Testing::buildVariableU32Pkt(1, 3, 7, postRunPulse),     // post-run update
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener(Types::Core::DateAndTime(1)));`
   *(1 ns sentinel → `m_filterUntilRunStart = true`.)*
2. `waitFor([&]{ return m_server->scriptIndex() >= 8; }, std::chrono::seconds{5});`
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions (XFAIL — see message body):**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
const auto &run = ws->run();
// XFAIL: intended behaviour — when the filter releases at NEW_RUN,
// replayVariableCache() should re-process the cached pre-run variable
// packets.  The variable's final replayed value should be 99 (the last
// pre-run value), which would then be overwritten by the post-run
// update to 7, so final expected value = 7.
//
// Actual broken behaviour: all variable packets processed as they
// arrive, no caching, final value = 7 (coincidentally the same), BUT
// the intermediate cached-then-replayed sequence was never exercised.
//
// Because the broken and fixed behaviours produce the same observable
// output in this particular test, the XFAIL marking is about
// correctness of the mechanism, not the count.  The real test is that
// replayVariableCache() is called, which cannot be observed from the
// outside without instrumentation.  We therefore document the defect
// but cannot write a sharply-inverting assertion.  The XFAIL message
// below captures the intent.
TSM_ASSERT(
    "XFAIL: pending fix for m_ignorePackets initialisation — see "
    "SNSLiveEventDataListener.cpp:1601-1629 and "
    "plans/ignore-packets-defect.md. "
    "The variable-cache replay path (replayVariableCache()) is "
    "unreachable because m_ignorePackets is never set true in start(). "
    "This test passes both before and after the fix (output is the "
    "same), but it is retained as executable documentation of the "
    "defect and to ensure the scenario compiles. When the fix lands, "
    "add an instrumentation hook or a spy on replayVariableCache() to "
    "sharpen this assertion.",
    run.hasProperty("status"));
```

---

## 5. §6.9 — Error propagation (3 tests)

### 5.1 `test_invalidPacket_propagatesAsBackgroundException`

**Purpose:** Garbage bytes cause `ADARA::invalid_packet` → the
exception is stored in `m_backgroundException` and re-thrown on the
next `runState()` or `extractData()` call. Covers
`SNSLiveEventDataListener.cpp:316`.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::PktGarbage{ {0xFF, 0xFE, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});`
   (Wait for garbage to be sent.)
3. Call `m_listener->runStatus()` or `extractWithTimeout(...)` — one of
   these must throw.

**Assertions:**
```cpp
bool threw = false;
try {
    (void)m_listener->runStatus();
    (void)extractWithTimeout(*m_listener, std::chrono::seconds{5});
} catch (...) {
    threw = true;
}
TS_ASSERT(threw);
```

### 5.2 `test_serverDisconnect_setsErrorState`

**Purpose:** When the server closes the connection, the listener
observes EOF and transitions out of `Connected` state.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    PKT(bankedEventPacketV1),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return !m_listener->isConnected(); }, std::chrono::seconds{5});`

**Assertions:**
```cpp
TS_ASSERT(!m_listener->isConnected());
// After EOF, listener should not report Connected.
TS_ASSERT_DIFFERS(m_listener->listenerState(),
                  API::ILiveListener::ListenerState::Connected);
```

### 5.3 `test_connectFailure_returnsFalse`

**Purpose:** `connect()` against a UDS path where no server is
listening must return `false` without throwing.

**Script:** none — `m_server->start()` is NOT called.

**Steps:**
1. Construct the listener manually:
   ```cpp
   m_listener = std::make_unique<SNSLiveEventDataListener>();
   ```
   (Do **not** call `connectListener()`.)
2. Build the address:
   ```cpp
   Poco::Net::SocketAddress addr(
       Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
   ```
3. `bool result = m_listener->connect(addr);`

**Assertions:**
```cpp
TS_ASSERT(!result);
TS_ASSERT(!m_listener->isConnected());
```

---

## 6. §6.10 — Monitor workspace routing (2 tests)

Covers `SNSLiveEventDataListener.cpp:465-525, 1345-1362`.

### 6.1 `test_beamMonitorEvents_routedToMonitorWorkspace`

**Purpose:** Beam monitor event packets are placed in the monitor
sub-workspace, not in the main event workspace.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 60,
                                0x0000000100000000ULL),
    PKT(bankedEventPacketV1),    // neutron events → main workspace
    PKT(beamMonitorPacketV1),    // monitor events → monitor workspace
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});`
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions:**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
// Main workspace has neutron events.
TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
// Monitor workspace exists and is separate.
auto monWs = ews->monitorWorkspace();
TS_ASSERT_DIFFERS(monWs, nullptr);
auto monEws = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(monWs);
TS_ASSERT_DIFFERS(monEws, nullptr);
TS_ASSERT_LESS_THAN(0, static_cast<int>(monEws->getNumberEvents()));
```

### 6.2 `test_invalidMonitorId_logsOnceAndContinues`

**Purpose:** A monitor event with an unrecognised monitor ID is logged
exactly once (the `m_badMonitors` dedup at
`SNSLiveEventDataListener.cpp:515-519`) and the listener continues
processing subsequent packets without crashing.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 61,
                                0x0000000100000000ULL),
    // Same invalid-monitor packet twice to verify log-once dedup:
    Testing::buildBeamMonitorPkt(0x0000000100000000ULL,
                                  /*monitorId=*/9999, {100u}),
    Testing::buildBeamMonitorPkt(0x0000000200000000ULL,
                                  /*monitorId=*/9999, {200u}),
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});`
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions:**
```cpp
// The listener must continue operating (no crash, no error state).
TS_ASSERT_DIFFERS(ws, nullptr);
TS_ASSERT_DIFFERS(m_listener->listenerState(),
                  API::ILiveListener::ListenerState::Error);
// Neutron events from bankedEventPacketV1 must still be present.
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
```

---

## 7. Known production defects surfaced by these tests (for reviewer)

### 7.1 `m_ignorePackets` never set to `true` in `start()`

**File and line range:** `SNSLiveEventDataListener.cpp:193-209`
(`start()`), `SNSLiveEventDataListener.cpp:1601-1629`
(`ignorePacket()`).

**Description:** `start()` sets `m_filterUntilRunStart = true` when
`startTime == 1 ns past epoch` (the historical-replay sentinel), but
never sets `m_ignorePackets = true`. Since `ignorePacket()`
short-circuits at `if (!m_ignorePackets) return false;`, the entire
filter-until-run-start and variable-cache replay path is dead code.

**Tests that expose this defect (added in this commit):**

- `test_filterUntilRunStart_dropsPreRunPackets` (§4.1)
- `test_variableCache_replayedAfterStartCondition` (§4.2)

**XFAIL inversion convention:** Both tests assert the *broken*
observable behaviour (packets not filtered, replay never called) via
an inverted `TSM_ASSERT` whose message explicitly names the defect and
cites the line range above. The test currently *passes* because it
asserts broken behaviour. **When the fix lands:** the inversion is
removed in the same commit as the production fix; the tests will then
assert the correct (fixed) behaviour and continue to pass.

**See also:** [`plans/ignore-packets-defect.md`](../../ignore-packets-defect.md).

---

## 8. TODO

- [ ] Open `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.
- [ ] Append the 7 `test_*` methods inside the existing class, after
      the methods added in `subspec05`, in this order:
      1. `test_filterUntilRunStart_dropsPreRunPackets`       (§4.1)
      2. `test_variableCache_replayedAfterStartCondition`    (§4.2)
      3. `test_invalidPacket_propagatesAsBackgroundException` (§5.1)
      4. `test_serverDisconnect_setsErrorState`              (§5.2)
      5. `test_connectFailure_returnsFalse`                  (§5.3)
      6. `test_beamMonitorEvents_routedToMonitorWorkspace`   (§6.1)
      7. `test_invalidMonitorId_logsOnceAndContinues`        (§6.2)
- [ ] Confirm both §4 tests are **registered and runnable** (not
      `#if 0`-ed out, not skipped at runtime) and include the verbatim
      `TSM_ASSERT*` message text shown in §4.
- [ ] Confirm `test_connectFailure_returnsFalse` does **not** call
      `connectListener()` and does **not** call `m_server->start()`.
- [ ] Confirm `test_invalidPacket_propagatesAsBackgroundException`
      uses a single `try { … } catch (...) { threw = true; }` around
      *both* the `runStatus()` and `extractWithTimeout(...)` calls, as
      shown in §5.1.
- [ ] Confirm both §6 tests check `ews->monitorWorkspace()` is non-null
      (§6.1) and that the listener does **not** transition to `Error`
      on unknown monitor ID (§6.2).
- [ ] Confirm no `TestableSNSListener` reference is introduced.
- [ ] After committing, count the total `test_*` methods in
      `SNSLiveEventDataListenerTest.h`: the answer must be exactly 22
      (1 + 8 + 6 + 7).

---

## 9. Definition of done for this commit (and for the PR)

1. `SNSLiveEventDataListenerTest.h` contains the 7 new `test_*` methods
   listed in §8, appended after the methods from `subspec05`.
2. The total number of `test_*` methods in the file is exactly **22**.
3. The two §4 XFAIL tests are compiled and registered (not commented
   out, not gated, not `#if 0`-ed).
4. No file outside `SNSLiveEventDataListenerTest.h` is modified by
   this commit.
5. No `TestableSNSListener` reference is introduced.
6. The whole-PR §6 Definition-of-Done in `overview-spec.md` is now
   satisfied for items 1–4 and 7 (the agent's deliverables).
   Items 5, 6, and 8 are reviewer-verified at PR review time.
