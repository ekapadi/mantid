# SNSLiveEventDataListener — UDS Integration Test Spec

> **This file is the historical monolith. It has been superseded by the
> sub-specs in this directory. The authoritative, up-to-date specification
> is in the following files:**
>
> | Sub-spec                                       | File                                                         |
> | ---------------------------------------------- | ------------------------------------------------------------ |
> | Master index + §0 agent instructions + CMake   | [`00-index.md`](00-index.md)                                 |
> | §3 UDS transport (corrected socket API)        | [`01-uds-transport.md`](01-uds-transport.md)                 |
> | §4 MockSMSServer API + threading               | [`02-mock-sms-server.md`](02-mock-sms-server.md)             |
> | §4.3 ADARA packet fixture reuse rules          | [`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md) |
> | §5 fixture skeleton + §6 all 22 test scenarios | [`04-test-scenarios.md`](04-test-scenarios.md)               |
>
> **Key corrections applied in the sub-specs (not in this file):**
>
> 1. **UDS socket**: The `"unix:path"` config-string syntax used in this file
>    is **invalid** Poco. The correct API is
>    `Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, path)`.
>    See [`01-uds-transport.md §3.4`](01-uds-transport.md).
>
> 1. **ADARA packet builders**: Every packet type required by the integration
>    scenarios has an existing fixture in `ADARAPackets.h`. Builders
>    `#include "ADARAPackets.h"` and patch fields in fixture copies; they do
>    not hand-roll byte layouts from scratch.
>    See [`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md).
>
> 1. **Test scenarios**: Each test in the sub-spec has explicit step-by-step
>    script, steps, and assertions — no bare line-number references.
>    See [`04-test-scenarios.md`](04-test-scenarios.md).
>
> 1. **XFAIL tests**: The two §6.8 tests are marked XFAIL with inverted
>    assertions that document the `m_ignorePackets` latent defect.
>    See [`04-test-scenarios.md §6.8`](04-test-scenarios.md) and
>    [`plans/ignore-packets-defect.md`](../ignore-packets-defect.md).
>
> 1. **Integration test discipline**: This suite drives the *real*
>    `SNSLiveEventDataListener`. Do **not** use `TestableSNSListener`.
>    See [`00-index.md §0 item 9`](00-index.md).
>
> The body of this file is retained for historical context only.
> Do not implement from this file — use the sub-specs above.

______________________________________________________________________

**Branch:** `EWM15431_live-listener-interface`
**Scope:** `Framework/LiveData/test/` and `Framework/LiveData/CMakeLists.txt` only.
**Status:** Superseded — see sub-specs above.

______________________________________________________________________

## 0. Agent execution instructions

Read this section first; it constrains *how* the spec is to be implemented.

1. **Base branch.** Base the PR on `EWM15431_live-listener-interface`. Do **not**
   base on `main` / `master`.

1. **Scope fence.** Touch only the files explicitly named in §2, §4, and §7.
   Do **not** modify any file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/`. Do not "fix" comments, reformat headers, or
   touch unrelated tests.

1. **Static verification only — DO NOT build, DO NOT run tests.** A full
   Mantid build takes hours and you cannot launch the test binary in the
   correct environment. Verify your changes by:

   - reading the relevant production code on this branch;
   - cross-referencing against the line ranges cited in §1 and §6;
   - checking that file names, `#include`s, `namespace` usages, and CMake
     edits are internally consistent;
   - hand-tracing the threading / lifetime rules in §4.2 and §5.3.

   The maintainer will perform the build and runtime verification by hand
   as part of PR review. Anywhere this spec mentions `cmake`, `ctest`,
   compiler output, or test pass/fail status, that instruction is for the
   *reviewer* — **not** for you.

1. **No build artefacts in the PR.** Do not commit `build/`, `CMakeFiles/`,
   generated headers, or compiler output.

1. **Legacy file.** The current `SNSLiveEventDataListenerTest.h` is renamed
   and **retained** as historical reference. It must remain in the tree but
   must **not** appear in `TEST_FILES`. Do not delete it.

1. **Ambiguity protocol.** If the spec is wrong, contradictory, or
   under-specified, **stop** and surface the question in the PR description.
   Do not invent a resolution silently.

1. **Flake check (§9 item 8).** Skip unless explicitly requested. It is a
   manual operator check, not a gating CI step.

1. **No production code changes.** If you believe production code must
   change for a test to pass, you have misread either the spec or the
   production contract — stop and ask.

______________________________________________________________________

## 1. Goal

Replace the unregistered, network-dependent
`Framework/LiveData/test/SNSLiveEventDataListenerTest.h` with a hermetic
integration suite that drives a real `SNSLiveEventDataListener` instance
against an in-process `MockSMSServer` over a Unix-domain socket (UDS). The
suite verifies, in addition to the legacy behavioural contract:

- The v3 single-slot pending-transition invariant (sub-spec 05) at
  `SNSLiveEventDataListener.cpp:646-652, 738-743`.
- The C1 fix — `m_lastTransition` survives an `extractData()` NotYet path
  (sub-spec 06) at `SNSLiveEventDataListener.cpp:1515-1516`.
- The deferred-run-details invariant (sub-spec 07) at
  `SNSLiveEventDataListener.cpp:1562-1568` — coverage delegated to the
  existing no-network unit suite; see §6.6.
- The white-lie path on mid-run join at `SNSLiveEventDataListener.cpp:654-697`.
- Pause/resume orthogonality (`onRunPause`) at
  `SNSLiveEventDataListener.cpp:1590-1596` and the
  `SNSLiveEventDataListener.keepPausedEvents` config gate at
  `SNSLiveEventDataListener.cpp:96-99, 400-402`.

No production code is modified. The listener already accepts a UDS
`Poco::Net::SocketAddress`; see `connect()` at
`SNSLiveEventDataListener.cpp:141-156`.

______________________________________________________________________

## 2. File rearrangement

| Action | Path                                                                                                                      | Notes                                                                                                                                                                                                                                                                                                                        |
| ------ | ------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Rename | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` → `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` | Retained as historical reference. **Not** registered in `TEST_FILES`. Add a one-line header comment: *"Legacy network-dependent test, retained for reference only; superseded by `SNSLiveEventDataListenerTest.h` (integration) and `SNSLiveEventDataListenerNoNetworkTest.h` (unit)."*                                      |
| Create | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`                                                                  | New UDS-driven integration suite. File-level Doxygen header **must** state: *"INTEGRATION TEST. Drives a real `SNSLiveEventDataListener` against an in-process `MockSMSServer` over a Unix-domain socket. Does NOT require SMS or any external network resource. Linux/macOS only — compiles to an empty suite on Windows."* |
| Create | `Framework/LiveData/test/MockSMSServer.h`                                                                                 | Declaration of `MockSMSServer` and the `ScriptEntry` variant.                                                                                                                                                                                                                                                                |
| Create | `Framework/LiveData/test/MockSMSServer.cpp`                                                                               | Implementation (Poco socket plumbing, packet builders, script driver).                                                                                                                                                                                                                                                       |

Keep the existing `SNSLiveEventDataListenerNoNetworkTest.h` unchanged.

______________________________________________________________________

## 3. Transport: Unix-domain socket

### 3.1 Rationale

- TCP loopback (`127.0.0.1`) is blocked or filtered on several Mantid CI
  runners — historically the reason the legacy suite was disabled.
- The SNS production deployment is Linux-only, so a Linux-only integration
  test is appropriate. SNS-listener testing on Windows has no operational
  value.
- The listener already supports UDS through its `Poco::Net::SocketAddress`
  argument (`SNSLiveEventDataListener.cpp:141-156`); no production change is
  required.

### 3.2 Platform guard

Wrap the entire body of `SNSLiveEventDataListenerTest.h` (after the `#pragma once`) and the body of `MockSMSServer.{h,cpp}` in:

```cpp
#ifndef _WIN32
  // ... entire suite ...
#else
  // Suite is intentionally empty on Windows — see file header.
  class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {};
#endif
```

Precedent: search the framework for `#ifndef _WIN32` in `Framework/*/test/`
for existing skipped-on-Windows suites.

### 3.3 Socket path

Use **`Poco::TemporaryFile`** to obtain a unique, tempdir-respecting path.
This is the framework-wide convention (`SaveGSSTest`, `SaveGDATest`,
`SaveOpenGenieAsciiTest`, `DownloadFileTest`, `InternetHelperTest`,
`LoadEMU`, `LoadBBY`, `PatchBBY`, `ScriptRepositoryImpl`,
`InternetHelper::downloadFile`). It internally uses `Poco::Path::temp()`,
which honours `$TMPDIR` on Unix and `%TEMP%` on Windows, and provides RAII
deletion.

```cpp
Poco::TemporaryFile m_sockFileHandle; // owns the path lifetime
const std::string m_sockPath = m_sockFileHandle.path();
```

`sun_path` length guard (defence in depth — `Poco::Path::temp()` is normally
short enough, but `$TMPDIR` can be set arbitrarily):

```cpp
if (m_sockPath.size() >= 100) {
    TS_SKIP("UDS path too long for sun_path on this platform: " + m_sockPath);
}
```

The `TemporaryFile` is created but the file is `remove()`d **before** the
server binds, because `bind()` requires the path to not exist.

______________________________________________________________________

## 4. `MockSMSServer` — header + implementation

**Two files, not header-only.** Header-only would (a) recompile the Poco
socket plumbing and packet builders into every test translation unit, and
(b) risk ODR collisions with the file-scope `const unsigned char`
exemplars in `Framework/LiveData/test/ADARAPackets.h`. The framework
precedent for test helpers with non-trivial implementation is
`KafkaTesting.h` + `TestDataListener.cpp` etc., registered via
`TESTHELPER_SRCS` in the existing
`Framework/LiveData/test/CMakeLists.txt:10`.

### 4.1 `MockSMSServer.h`

Public surface:

```cpp
namespace Mantid::LiveData::Testing {

// One step in the server's playback script.
struct PktGarbage   { std::vector<uint8_t> bytes; };
struct PktDisconnect{};
struct PktWaitForExtract {};  // gate: blocks until the test signals
                              // (used to deterministically interleave
                              //  extractData() with packet delivery).

using ScriptEntry = std::variant<
    std::vector<uint8_t>,   // raw packet bytes (preferred — built by the
                            // helpers in §4.3)
    PktGarbage,
    PktDisconnect,
    PktWaitForExtract>;

class MockSMSServer {
public:
    // path: absolute UDS path (must not exist).
    explicit MockSMSServer(std::string path);
    ~MockSMSServer();  // joins server thread; closes sockets; unlinks path.

    MockSMSServer(const MockSMSServer&) = delete;
    MockSMSServer& operator=(const MockSMSServer&) = delete;

    // Begin listening. Returns immediately; accept() happens on the bg thread.
    void start();

    // Append script entries. Safe to call before or after start(), but
    // ALL entries must be queued before the listener calls connect().
    void script(std::initializer_list<ScriptEntry> entries);
    void scriptAppend(ScriptEntry entry);

    // Release the next PktWaitForExtract gate. Called by the test fixture
    // immediately after extractData() returns.
    void releaseExtractGate();

    // Diagnostics for assertions.
    bool clientConnected() const;
    std::size_t bytesSent() const;
    std::size_t scriptIndex() const;  // how many entries have been delivered

    // Self-watchdog deadline (default 30 s).  If the script is not exhausted
    // by then, the server closes its sockets so the client observes EOF
    // rather than hanging.  See §5.1.
    void setWatchdog(std::chrono::seconds);

private:
    // ... Poco::Net::ServerSocket m_listenSocket;
    // ... Poco::Net::StreamSocket m_clientSocket;
    // ... Poco::Thread m_thread + Poco::Runnable adapter
    // ... std::mutex / std::condition_variable for the gate
};

} // namespace Mantid::LiveData::Testing
```

### 4.2 Lifetime / threading rules

- Constructor only initialises members; it does **not** bind or listen.
- `start()` creates the `Poco::Net::ServerSocket` on a
  `Poco::Net::SocketAddress(path)`, then launches the background thread,
  which `accept()`s exactly one client and then drains the script.
- Destructor sets a stop flag, shuts down the sockets (to break a blocked
  `accept` or `send`), joins the thread with a bounded timeout (5 s; if it
  fails, `g_log.fatal` and `std::abort()` — a hung test process is worse
  than a crashed one).
- The destructor `unlink()`s the socket path (the `Poco::TemporaryFile` that
  owned the path is destroyed by the fixture *after* the server, so the
  path's lifetime strictly outlives the server).

### 4.3 Packet builders

Provide non-member helpers in the `Testing` namespace that return
`std::vector<uint8_t>` ready to push into a `ScriptEntry`. These are thin
wrappers around the binary exemplars in
`Framework/LiveData/test/ADARAPackets.h` with patchable fields:

| Helper                                                                                      | Mutates                      | Notes                                             |
| ------------------------------------------------------------------------------------------- | ---------------------------- | ------------------------------------------------- |
| `buildGeometryPkt(const std::string& xml)`                                                  | XML payload + length         | Used to inject minimal valid instrument geometry. |
| `buildBeamlineInfoPkt(const std::string& longName)`                                         | long name                    |                                                   |
| `buildRunStatusPkt(ADARA::RunStatus::Enum, uint32_t runNumber, uint64_t pulseId)`           | status, run number, pulse id | NEW_RUN / END_RUN / STATE.                        |
| `buildRunInfoPkt(const std::string& proposalId, const std::string& title)`                  | XML payload                  |                                                   |
| `buildBankedEventPkt(uint64_t pulseId, double pulseChargePc, std::span<const PixelTof>)`    | pulse id, charge, events     |                                                   |
| `buildBeamMonitorPkt(uint64_t pulseId, uint32_t monitorId, std::span<const uint32_t> tofs)` |                              |                                                   |
| `buildAnnotationPkt(ADARA::MarkerType, uint32_t scanIndex, const std::string& comment)`     |                              | PAUSE / RESUME / SCAN_START / SCAN_STOP.          |
| `buildDeviceDescriptorPkt(uint32_t devId, std::span<const PVDesc>)`                         | XML                          |                                                   |
| `buildVariableU32Pkt(uint32_t devId, uint32_t pvId, uint32_t value, uint64_t pulseId)`      |                              |                                                   |
| `buildVariableDoublePkt(...)`                                                               |                              |                                                   |
| `buildHeartbeatPkt(uint64_t pulseId)`                                                       | timestamp                    |                                                   |

Each builder asserts at construction that the produced payload length field
matches the actual byte count.

### 4.4 Driving modes

The server thread iterates the script. For each entry:

- `std::vector<uint8_t>` → `m_clientSocket.sendBytes(...)`.
- `PktGarbage` → send arbitrary bytes (used to verify
  `ADARA::invalid_packet` propagation, §6.9).
- `PktDisconnect` → `m_clientSocket.close()`; mark "client gone"; stop.
- `PktWaitForExtract` → block on the condition variable until the fixture
  calls `releaseExtractGate()` or the watchdog fires.

No `Poco::Thread::sleep` between entries by default. Inter-entry delays, if
ever needed, can be expressed as a separate `PktDelay{ms}` entry — left
out of v1 to keep the surface minimal.

______________________________________________________________________

## 5. Test fixture

### 5.1 Hang protection — four layers

> **Notes for reviewer:** *\[ agent should not run `ctest`! \]*
> This section describes the runtime behaviour the maintainer will observe
> when invoking `ctest` during PR review. The coding agent does not run
> the suite — its job is to write the four protection layers, not to
> verify them at runtime.

A test that hangs at runtime blocks the entire `ctest` invocation.
Defence in depth:

1. **`waitFor(pred, timeout = 5s, poll = 10ms)`** — replaces every
   `Poco::Thread::sleep` in the legacy assertions. On timeout it calls
   `TS_FAIL` with a descriptive message and returns false (so the calling
   test can attempt cleanup).
1. **`extractWithTimeout(listener, timeout = 10s)`** — wraps
   `listener.extractData()` in `std::async(std::launch::async, ...)` and
   `std::future::wait_for`. On timeout it `TS_FAIL`s and returns
   `nullptr`. This protects against the worst-case regression: a deadlock
   in `onBeforeExtract` / `onBeginRun` / `onEndRun` due to a mutex bug
   would otherwise wedge the foreground thread forever.
1. **`MockSMSServer` self-watchdog (§4.1)** — if the script is not
   exhausted within `setWatchdog()` (default 30 s), the server closes its
   client socket. The listener will then observe EOF from its
   `m_socket.receiveBytes` call and surface the failure through its
   normal exception path (`SNSLiveEventDataListener.cpp:274-278, 336-342`).
1. **`TestWatchdog` RAII helper** — constructed at the top of every test
   that drives the listener; arms a background thread that, if not
   disarmed within 60 s, calls `g_log.fatal` and `std::abort()`. A clean
   crash of the test binary is strictly preferable to wedging `ctest` on
   the reviewer's machine. The watchdog is disarmed by the fixture's
   `tearDown`.

These limits are deliberately loose (5 / 10 / 30 / 60 s) — they exist to
catch *bugs*, not to enforce performance. Healthy tests complete in \<1 s.

### 5.2 Suite-wide ctest timeout

> **Notes for reviewer:** *\[ agent should not run `ctest`! \]*
> The `set_tests_properties(... TIMEOUT 120)` CMake edit *is* part of the
> agent's deliverable (see §7). What this section describes — the
> behaviour of `ctest` itself enforcing the 120 s ceiling — is observable
> only at the reviewer's runtime and is not something the agent needs to
> verify.

Add a CMake property registration after the existing `MonitorLiveDataTest`
serial-run line (see §7):

```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```

This is the last-resort guard: `ctest` itself kills the test binary after
120 s. It should never be hit in practice; if it is, the `TestWatchdog`
in §5.1(4) failed and that is itself a bug worth investigating.

### 5.3 Fixture skeleton

```cpp
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {
public:
    static SNSLiveEventDataListenerTest *createSuite()  { return new SNSLiveEventDataListenerTest(); }
    static void destroySuite(SNSLiveEventDataListenerTest *s) { delete s; }

    void setUp() override {
        // Save config so we can restore in tearDown — see ConfigObserverTest.h
        // and ConfigPropertyObserverTest.h for the precedent.
        auto &cfg = Kernel::ConfigService::Instance();
        m_savedTestAddress       = cfg.getString("SNSLiveEventDataListener.testAddress");
        m_savedKeepPausedEvents  = cfg.getString("SNSLiveEventDataListener.keepPausedEvents");

        m_sockFileHandle = std::make_unique<Poco::TemporaryFile>();
        m_sockPath = m_sockFileHandle->path();
        if (m_sockPath.size() >= 100) {
            TS_SKIP("UDS path too long for sun_path on this platform: " + m_sockPath);
        }
        // bind() requires the path to NOT exist:
        std::filesystem::remove(m_sockPath);

        cfg.setString("SNSLiveEventDataListener.testAddress", "unix:" + m_sockPath);
        // ^ The listener already constructs SocketAddress from this string;
        //   see connect() at SNSLiveEventDataListener.cpp:143-149.

        m_server = std::make_unique<Testing::MockSMSServer>(m_sockPath);
        m_watchdog = std::make_unique<Testing::TestWatchdog>(std::chrono::seconds{60},
                                                              /*test_name=*/ __func__);
    }

    void tearDown() override {
        // Strict destruction order: listener -> server -> tempfile.
        m_listener.reset();
        m_server.reset();
        m_sockFileHandle.reset();
        m_watchdog.reset();  // disarm last

        auto &cfg = Kernel::ConfigService::Instance();
        cfg.setString("SNSLiveEventDataListener.testAddress",      m_savedTestAddress);
        cfg.setString("SNSLiveEventDataListener.keepPausedEvents", m_savedKeepPausedEvents);
    }

    // ... tests ...

private:
    std::string m_savedTestAddress;
    std::string m_savedKeepPausedEvents;
    std::unique_ptr<Poco::TemporaryFile> m_sockFileHandle;
    std::string m_sockPath;
    std::unique_ptr<Testing::MockSMSServer> m_server;
    std::unique_ptr<SNSLiveEventDataListener> m_listener;
    std::unique_ptr<Testing::TestWatchdog> m_watchdog;
};
```

Notes:

- **No listener constructed in the fixture constructor.** Each test
  creates its own listener after queuing the script, to give the test
  full control over the connection ordering. Some tests (§6.9) want to
  drive `connect()` against a server that is *not* yet listening.
- **Strict tearDown order** is required: the listener's background thread
  holds a `Poco::Net::StreamSocket` that may still be reading; destroy
  the listener (which sets `m_stopThread` and joins) before destroying
  the server (which would close the socket out from under it).

______________________________________________________________________

## 6. Test catalogue

22 tests, organised by area. Each cites the production line range whose
invariant it covers. All tests use the §5.1 timeout primitives — no bare
`extractData()` calls, no bare `Poco::Thread::sleep` in assertions.

### 6.1 Legacy behavioural contract (preserved, de-flaked) — 4 tests

Ported from `SNSLiveEventDataListenerLegacyTest.h` with the
10 000-iteration polling loop and `use_count()` race replaced by
`waitFor(pred, 5s)`:

- `test_LegacyConstruction_initialState`
- `test_LegacyConnectAndDisconnect`
- `test_LegacyExtractEmptyWorkspace`
- `test_LegacyConnectionStatusTransitions`

### 6.2 Connection & mid-run join — 2 tests

- `test_connect_succeeds_over_uds`
- `test_midRunJoin_doesNotWipeWorkspaceInit` — covers the "white-lie" path
  at `SNSLiveEventDataListener.cpp:654-697`. Script: NEW_RUN before any
  Geometry / BeamlineInfo / DeviceDescriptor packets, *then* the
  initialisation packets, then BankedEvent. Verifies that
  `extractData()` returns a properly initialised workspace and that
  `runState()` is `Running` without the `m_pauseNetRead` deadlock that
  the comment block warns against.

### 6.3 Single & full run lifecycle — 3 tests

- `test_singleRun_extractsEventsAndRunNumber`
- `test_fullRun_beginExtractEndExtract`
- `test_runNumber_proposalId_title_propagate` — checks RunInfo packet
  handling (`SNSLiveEventDataListener.cpp:1185-1263`).

### 6.4 C1 fix — `lastTransition` survives NotYet — 1 test

- `test_lastTransition_preservedAcrossNotYet` — covers
  `SNSLiveEventDataListener.cpp:1515-1516`. Script forces `extractData()`
  to take the NotYet path (no Geometry yet) and asserts `lastTransition()`
  still reports the value set by the previous successful extract.

### 6.5 Back-pressure single-slot invariant — 2 tests

Covers `SNSLiveEventDataListener.cpp:646-652, 738-743`.

- `test_doubleBeginRun_violatesSingleSlot_throws` — script:
  NEW_RUN(1), Geometry, BeamlineInfo, BankedEvent, NEW_RUN(2) **without
  an intervening** `extractData()`. Asserts the second NEW_RUN raises
  `std::runtime_error` from the background thread (surfaced via
  `m_backgroundException` on next `runState()` / `extractData()` call).
- `test_endRunWhilePending_violatesSingleSlot_throws` — analogous for
  NEW_RUN followed by END_RUN without intervening extract.

### 6.6 Deferred-run-details invariant — **delegated**

Sub-spec 07 (`m_deferredRunDetailsPkt` non-null at `onBeginRun()`,
`SNSLiveEventDataListener.cpp:1562-1568`) is **already covered** by
`SNSLiveEventDataListenerNoNetworkTest.h`. We do **not** duplicate that
coverage here; instead, this section in the test file's header comment
contains a one-line cross-reference: *"For the deferred-run-details
invariant see `SNSLiveEventDataListenerNoNetworkTest.h::test_*`."*

### 6.7 Pause / resume — 3 tests

Covers `SNSLiveEventDataListener.cpp:1590-1596` and `:96-99, 400-402`.

- `test_pauseResume_orthogonalToRunState` — runState remains `Running`
  across PAUSE/RESUME annotation packets; `isPaused()` flips correctly.
- `test_pausedEvents_droppedByDefault` — `keepPausedEvents=false` (the
  default); events received between PAUSE and RESUME do **not** appear in
  the extracted workspace.
- `test_pausedEvents_keptWhenConfigured` — sets
  `SNSLiveEventDataListener.keepPausedEvents=true` in `setUp`-override;
  events between PAUSE and RESUME **do** appear.

### 6.8 Historical replay & variable cache — 2 tests

- `test_filterUntilRunStart_dropsPreRunPackets` — covers
  `SNSLiveEventDataListener.cpp:198-205, 1611-1615`. Starts listener with
  `startTime = 1ns past epoch` (the "replay from previous run start"
  sentinel), feeds pre-NEW_RUN events, asserts they are absent from the
  first extract.
- `test_variableCache_replayedAfterStartCondition` — covers
  `SNSLiveEventDataListener.cpp:1625-1638`. Variable packets received
  while `m_ignorePackets` is true are cached, then replayed when the
  filter releases; verifies the log property reflects all values in
  arrival order.

### 6.9 Error propagation — 3 tests

- `test_invalidPacket_propagatesAsBackgroundException` — script:
  Geometry, then `PktGarbage{}`. Asserts subsequent `runState()` or
  `extractData()` throws (from `m_backgroundException` set at
  `SNSLiveEventDataListener.cpp:316`).
- `test_serverDisconnect_setsErrorState` — script:
  Geometry, BankedEvent, `PktDisconnect{}`. Asserts
  `listenerState() == ListenerState::Error` within `waitFor()`.
- `test_connectFailure_returnsFalse` — server is **not** started;
  asserts `listener.connect(addr) == false` and `isConnected() == false`,
  without throwing.

### 6.10 Monitor workspace routing — 2 tests

Covers `SNSLiveEventDataListener.cpp:465-525, 1345-1362`.

- `test_beamMonitorEvents_routedToMonitorWorkspace`
- `test_invalidMonitorId_logsOnceAndContinues` — verifies the
  `m_badMonitors` dedup at `SNSLiveEventDataListener.cpp:515-519`.

______________________________________________________________________

## 7. CMake registration

Edit `Framework/LiveData/CMakeLists.txt`:

```cmake
# In TEST_FILES (alphabetical), replace the existing comment line:
#   # Needs fixing to not rely on network. SNSLiveEventDataListenerTest.h
# with:
SNSLiveEventDataListenerTest.h
# (SNSLiveEventDataListenerLegacyTest.h is intentionally NOT registered —
#  retained as historical reference.)
```

Edit `Framework/LiveData/test/CMakeLists.txt`:

```cmake
# Add MockSMSServer.cpp to TESTHELPER_SRCS (currently line 10):
set(TESTHELPER_SRCS
    KafkaTesting.h KafkaTestThreadHelper.h
    TestDataListener.cpp TestGroupDataListener.cpp
    MockSMSServer.cpp     # <-- added
)
```

And the ctest timeout from §5.2, near the existing `MonitorLiveDataTest`
serial-run property:

```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```

______________________________________________________________________

## 8. Out of scope

- **No production code change.** Sub-specs 05/06/07 are already
  implemented on this branch; this work only verifies them.
- **No replacement of the unit tests** in
  `SNSLiveEventDataListenerNoNetworkTest.h`. The integration suite
  complements, does not duplicate.
- **No Windows port.** SNS is Linux; the Windows precedent for skipped
  suites is followed.
- **No `m_deferredRunDetailsPkt` invariant test** in the integration
  suite — covered by the no-network unit suite (§6.6).

______________________________________________________________________

## 9. Definition of done

Items 1–4 and 7 are the **agent's deliverables** — verifiable by static
inspection of the PR.

1. `SNSLiveEventDataListenerLegacyTest.h` exists, is not registered, and
   contains the required header comment.
1. `SNSLiveEventDataListenerTest.h`, `MockSMSServer.h`,
   `MockSMSServer.cpp` exist with the contents described in §3–§6.
1. `Framework/LiveData/CMakeLists.txt` registers
   `SNSLiveEventDataListenerTest.h` in `TEST_FILES` and **not**
   `SNSLiveEventDataListenerLegacyTest.h`.
1. `Framework/LiveData/test/CMakeLists.txt` lists `MockSMSServer.cpp` in
   `TESTHELPER_SRCS` and sets the `TIMEOUT 120` property.
1. No file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/` is modified by the PR.

______________________________________________________________________

> **Notes for reviewer:** *\[ agent should not run `ctest`! \]*
> Items 5, 6, and 8 below are runtime-verifiable only, by the maintainer,
> after a local build. The coding agent has been explicitly instructed
> (§0 item 3) **not** to build or run tests. Do not interpret the absence
> of build/test evidence in the PR as an oversight — it is the
> contracted workflow.

5. *(Reviewer-verified.)* On Linux/macOS, all 22 tests pass under
   `ctest -R LiveDataTest`.
1. *(Reviewer-verified.)* On Windows, the suite compiles cleanly to an
   empty `CxxTest::TestSuite` and `ctest -R LiveDataTest` passes (no SNS
   tests registered).
1. *(Reviewer-only, optional, not CI-gated.)* A deliberate-deadlock
   injection check — temporarily insert a
   `std::this_thread::sleep_for(std::chrono::hours{1})` in
   `onBeforeExtract` — must cause the affected test to fail-fast via
   the `extractWithTimeout` guard within ~10 s, not hang. Revert before
   commit. Operator-run only; **never** to be performed by the coding
   agent.

______________________________________________________________________

## 10. References

Production code (this branch, `EWM15431_live-listener-interface`):

- [`Framework/LiveData/src/SNSLiveEventDataListener.cpp`](../../Framework/LiveData/src/SNSLiveEventDataListener.cpp)
  — see line ranges cited in §1 and §6.

Existing test files:

- [`Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`](../../Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h)
  — companion unit suite.
- [`Framework/LiveData/test/ADARAPackets.h`](../../Framework/LiveData/test/ADARAPackets.h)
  — binary exemplars for the §4.3 builders.
- [`Framework/LiveData/test/KafkaTesting.h`](../../Framework/LiveData/test/KafkaTesting.h)
  - `TestDataListener.cpp` — the `TESTHELPER_SRCS` precedent.

Config / tempfile precedent:

- [`Framework/Kernel/test/ConfigObserverTest.h`](../../Framework/Kernel/test/ConfigObserverTest.h)
  - `ConfigPropertyObserverTest.h` — save / restore config in
    `setUp` / `tearDown`.
- `Poco::TemporaryFile` usage across `Framework/*` — see `SaveGSSTest.h`,
  `SaveGDATest.h`, `SaveOpenGenieAsciiTest.h`, `DownloadFileTest.h`,
  `InternetHelperTest.h`, `InternetHelper::downloadFile`,
  `LoadEMU::exec`, `LoadBBY::createInstrument`, `PatchBBY`,
  `ScriptRepositoryImpl::download_file`.

Companion design docs:

- `plans/listener_refactoring_v3.md` — the v3 refactor this suite verifies.
- `plans/v3_subspecs/` — sub-specs 05/06/07 referenced in §1.
