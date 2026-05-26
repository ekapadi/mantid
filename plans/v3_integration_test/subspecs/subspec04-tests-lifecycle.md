# Sub-spec 04 — Lifecycle tests (legacy contract + connection + run lifecycle)

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 4 of 6. Adds **8** test methods to the fixture created in
`subspec03`, covering the legacy behavioural contract (remainder),
connection and mid-run join, and the single + full run lifecycle.

| Section | Area | Tests in this commit |
|---|---|---|
| 6.1 | Legacy behavioural contract (de-flaked) | 3 (the 4th, `test_LegacyConstruction_initialState`, was added in `subspec03`) |
| 6.2 | Connection & mid-run join | 2 |
| 6.3 | Single & full run lifecycle | 3 |

Total: 8 new `test_*` methods added to
`Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.

---

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface__agents`.
2. **Scope fence.** This commit edits exactly **one** file:
   `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`. Do not
   modify CMake, `MockSMSServer.h/.cpp`, the legacy header, or any file
   under `Framework/LiveData/src/` / `Framework/LiveData/inc/`.
3. **Static verification only — DO NOT build, DO NOT run tests.**
   Cross-reference the cited line ranges in
   `Framework/LiveData/src/SNSLiveEventDataListener.cpp` to confirm the
   purpose statements are accurate. Verify that each test uses the
   `connectListener()` helper, `waitFor()`, and `extractWithTimeout()`
   from `subspec03` consistently.
4. **No build artefacts in the PR.**
5. **No `TestableSNSListener`.** Every test must drive the *real*
   `SNSLiveEventDataListener` via `connectListener()`.
6. **Ambiguity protocol.** If a referenced ADARA fixture array does not
   exist in `Framework/LiveData/test/ADARAPackets.h` on this branch, or
   a referenced builder does not exist in `MockSMSServer.h` (from
   `subspec02`), **stop** and surface the gap in the PR description.
7. **No production code changes.**
8. **One commit, all 8 tests.** Do not split across multiple commits.
   Do not add tests from `subspec05` / `subspec06`.

---

## 1. Goal of this commit

Make the integration suite cover its primary happy-path scenarios:

- the disconnected → connected → disconnected baseline transitions
  carried over from the legacy network suite (de-flaked by replacing
  10 000-iteration polling loops with bounded `waitFor` calls);
- the "white-lie" path on mid-run join
  (`SNSLiveEventDataListener.cpp:654-697`), where NEW_RUN arrives before
  Geometry / BeamlineInfo / DeviceDescriptor;
- the full NEW_RUN → events → END_RUN cycle, including RunInfo packet
  propagation of proposal ID and run title
  (`SNSLiveEventDataListener.cpp:1185-1263`).

---

## 2. Files touched in this commit

| Action | Path |
|---|---|
| Edit | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (add 8 `test_*` methods inside the existing class) |

---

## 3. Conventions used in the scripts below

The shorthand below is used in every test script. The `PKT(name)` macro
was defined in `subspec03` §4.2; the `BLD_*` shorthand expands as
follows when reading this sub-spec (do **not** introduce these as
macros in code — they are documentation aliases):

- `PKT(name)` = `std::vector<uint8_t>(name, name + sizeof(name))` for
  the named array in `ADARAPackets.h`.
- `BLD_RUN(status, num, pulseId)` =
  `Testing::buildRunStatusPkt(status, num, pulseId)`.
- `BLD_VAR_U32(dev, pv, val, pid)` =
  `Testing::buildVariableU32Pkt(dev, pv, val, pid)`.
- `BLD_VAR_DBL(dev, pv, val, pid)` =
  `Testing::buildVariableDoublePkt(dev, pv, val, pid)`.
- `BLD_BANKED(pid, charge, events)` =
  `Testing::buildBankedEventPkt(pid, charge, events)`.

`pulseId` values are 64-bit: `(seconds << 32) | nanos`. Example: pulse
at t = 1 s = `0x0000000100000000ULL`.

Each test method below specifies its **Purpose**, the **Script** the
server is given, the **Steps** the test code runs, and the
**Assertions**. Implement each method literally with that ordering.

---

## 4. §6.1 — Legacy behavioural contract (3 tests, the remainder)

The placeholder fourth member of this group,
`test_LegacyConstruction_initialState`, was added in `subspec03` and
must remain unchanged.

These tests are ported from the now-renamed
`SNSLiveEventDataListenerLegacyTest.h`. The 10 000-iteration polling
loop and `use_count()` race are replaced by `waitFor(pred, 5s)`.

### 4.1 `test_LegacyConnectAndDisconnect`

**Purpose:** `connect()` returns true; the server observing EOF causes
the listener to transition to a disconnected state.

**Script:**
```cpp
m_server->script({ Testing::PktDisconnect{} });
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return !m_listener->isConnected(); }, std::chrono::seconds{5});`

**Assertions:**
```cpp
TS_ASSERT(!m_listener->isConnected());
```

### 4.2 `test_LegacyExtractEmptyWorkspace`

**Purpose:** `extractData()` before any run returns a workspace of the
expected type (even if empty).

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});`
   (Wait until server has sent Geometry + BeamlineInfo before extract.)
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions:**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::NoRun);
```

### 4.3 `test_LegacyConnectionStatusTransitions`

**Purpose:** `listenerState()` transitions from `Connecting` →
`Connected` after a successful handshake sequence.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});`

**Assertions:**
```cpp
// After receiving Geometry and BeamlineInfo, listener is Connected.
TS_ASSERT_EQUALS(m_listener->listenerState(),
                 API::ILiveListener::ListenerState::Connected);
m_server->releaseExtractGate();
```

---

## 5. §6.2 — Connection & mid-run join (2 tests)

### 5.1 `test_connect_succeeds_over_uds`

**Purpose:** The listener can connect and exchange packets over a Unix
domain socket.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});`

**Assertions:**
```cpp
TS_ASSERT(m_server->clientConnected());
TS_ASSERT_LESS_THAN(0u, m_server->bytesSent());
```

### 5.2 `test_midRunJoin_doesNotWipeWorkspaceInit`

**Purpose:** Covers the "white-lie" path at
`SNSLiveEventDataListener.cpp:654-697`. When the listener joins
mid-run (receives NEW_RUN *before* Geometry / BeamlineInfo /
DeviceDescriptor packets), the first `extractData()` must return a
properly initialised workspace and `runState()` must be `Running`.

**Script:**
```cpp
// NEW_RUN arrives BEFORE geometry/beamline metadata — mid-run join.
m_server->script({
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                /*runNum=*/100,
                                /*pulseId=*/0x0000000100000000ULL),
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});`
   (Wait for geometry, beamline info, and event packet to be
   delivered.)
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions:**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
// The listener must report Running after a NEW_RUN + events.
TS_ASSERT_EQUALS(m_listener->runStatus(),
                 API::ILiveListener::Running);
// Workspace must not be uninitialised (no empty instrument).
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
TS_ASSERT_DIFFERS(ews->getInstrument()->getName(), std::string{});
```

(Add `#include "MantidDataObjects/EventWorkspace.h"` to the test header
if not already present — it is consumed here for the first time.)

---

## 6. §6.3 — Single & full run lifecycle (3 tests)

### 6.1 `test_singleRun_extractsEventsAndRunNumber`

**Purpose:** A complete NEW_RUN → events → `extractData()` cycle
surfaces the events in the workspace with the correct run number.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 42,
                                0x0000000100000000ULL),
    Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                  /*chargePc=*/1000.0,
                                  {{/*tof=*/100u, /*pixel=*/1u}}),
    Testing::PktWaitForExtract{},
    Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 42,
                                0x0000000200000000ULL),
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});`
3. `auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
4. `m_server->releaseExtractGate();`

**Assertions:**
```cpp
TS_ASSERT_DIFFERS(ws, nullptr);
auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
TS_ASSERT_DIFFERS(ews, nullptr);
TS_ASSERT_EQUALS(
    ews->run().getPropertyValueAsType<int>("run_number"), 42);
TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
```

### 6.2 `test_fullRun_beginExtractEndExtract`

**Purpose:** Full run lifecycle: NEW_RUN → extract at `Running` →
END_RUN → extract at `RunEnded`.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 55,
                                0x0000000100000000ULL),
    PKT(bankedEventPacketV1),
    Testing::PktWaitForExtract{},      // gate 1
    Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 55,
                                0x0000000300000000ULL),
    Testing::PktWaitForExtract{},      // gate 2
    Testing::PktDisconnect{},
});
m_server->start();
```

**Steps:**
1. `TS_ASSERT(connectListener());`
2. `waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});`
3. First extract: `auto ws1 = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
   Status check: `TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);`
4. `m_server->releaseExtractGate();`  *(release gate 1)*
5. `waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});`
   *(wait for END_RUN to be sent)*
6. Second extract: `auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});`
7. `m_server->releaseExtractGate();`  *(release gate 2)*

**Assertions:**
```cpp
TS_ASSERT_DIFFERS(ws1, nullptr);
TS_ASSERT_DIFFERS(ws2, nullptr);
TS_ASSERT_EQUALS(m_listener->runStatus(),
                 API::ILiveListener::RunEnded);
```

### 6.3 `test_runNumber_proposalId_title_propagate`

**Purpose:** Checks RunInfo packet handling at
`SNSLiveEventDataListener.cpp:1185-1263`. Proposal ID and run title
must appear in workspace run properties.

**Script:**
```cpp
m_server->script({
    PKT(geometryPacketV0),
    PKT(beamlineInfoPacketV1),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 77,
                                0x0000000100000000ULL),
    Testing::buildRunInfoPkt("IPTS-12345", "My Test Title"),
    PKT(bankedEventPacketV1),
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
const auto &run = ws->run();
TS_ASSERT_EQUALS(
    run.getPropertyValueAsType<std::string>("experiment_identifier"),
    std::string{"IPTS-12345"});
TS_ASSERT_EQUALS(
    run.getPropertyValueAsType<std::string>("run_title"),
    std::string{"My Test Title"});
```

---

## 7. TODO

- [ ] Open `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.
- [ ] Inside the existing
      `class SNSLiveEventDataListenerTest : public CxxTest::TestSuite`,
      after the existing `test_LegacyConstruction_initialState`, add
      the 8 `test_*` methods specified in §4–§6 above, in the order
      listed:
      1. `test_LegacyConnectAndDisconnect`           (§4.1)
      2. `test_LegacyExtractEmptyWorkspace`          (§4.2)
      3. `test_LegacyConnectionStatusTransitions`    (§4.3)
      4. `test_connect_succeeds_over_uds`            (§5.1)
      5. `test_midRunJoin_doesNotWipeWorkspaceInit`  (§5.2)
      6. `test_singleRun_extractsEventsAndRunNumber` (§6.1)
      7. `test_fullRun_beginExtractEndExtract`       (§6.2)
      8. `test_runNumber_proposalId_title_propagate` (§6.3)
- [ ] Add any additional `#include`s needed by the new code
      (`MantidDataObjects/EventWorkspace.h` is the only new one likely
      required — see §5.2). Place new includes alphabetically next to
      existing ones in the file.
- [ ] Confirm every test:
      - calls `m_server->script({...})` and `m_server->start()` (in
        that order),
      - then calls `TS_ASSERT(connectListener())`,
      - uses `waitFor(...)` rather than a bare sleep,
      - uses `extractWithTimeout(*m_listener, ...)` rather than a bare
        `m_listener->extractData()`.
- [ ] Confirm no `TestableSNSListener` mention appears in the diff.
- [ ] Confirm no edits outside
      `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`.

---

## 8. Definition of done for this commit

1. `SNSLiveEventDataListenerTest.h` contains the 8 new `test_*` methods
   in the order in §7.
2. Every new test uses `connectListener()`, `waitFor()`,
   `extractWithTimeout()`, and `Testing::*` helpers consistently as
   shown in §4–§6.
3. No file outside
   `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` is modified
   by this commit.
4. The `setUp()` / `tearDown()` / private member section / placeholder
   test from `subspec03` are unchanged.
5. No `TestableSNSListener` reference is introduced.
