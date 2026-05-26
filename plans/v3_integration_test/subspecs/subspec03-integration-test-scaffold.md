# Sub-spec 03 — Integration test scaffold (fixture + helpers + CMake registration)

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 3 of 6. Creates the new `SNSLiveEventDataListenerTest.h`
with the fixture skeleton, hang-protection helpers, Windows stub, the
`connectListener()` private helper, and a **single** placeholder test
(`test_LegacyConstruction_initialState`). Registers it in `TEST_FILES`
and sets the `ctest` `TIMEOUT 120` property.

After this commit the suite compiles on Linux/macOS, has exactly one
registered test method (which does not touch the server), and is the
foundation onto which `subspec04`–`subspec06` graft the remaining
21 test methods.

---

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface__agents`; **do not** rebase
   onto `main` / `master`.
2. **Scope fence.** Touch only the three files in §2 below.
   Do **not** modify any file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/`. Do not modify `MockSMSServer.h/.cpp` in
   this commit.
3. **Static verification only — DO NOT build, DO NOT run tests.**
   Verify by reading the resulting file, checking that all
   `Mantid::LiveData::Testing::*` symbols referenced are declared in
   `MockSMSServer.h` (from `subspec02`), and that `connect()` is called
   with the explicit UDS `SocketAddress` form per §4 of this sub-spec.
4. **No build artefacts in the PR.**
5. The renamed legacy file is still NOT registered. In this commit you
   **rewrite** the `# Needs fixing …` placeholder comment in
   `Framework/LiveData/CMakeLists.txt` with the new
   `SNSLiveEventDataListenerTest.h` entry per §6 — but do not register
   `SNSLiveEventDataListenerLegacyTest.h`.
6. **Ambiguity protocol.** Stop and ask in the PR description if you
   find an existing entry for `SNSLiveEventDataListenerTest.h` in
   `TEST_FILES` (none should exist — only a comment placeholder).
7. **Real integration test.** Do not use `TestableSNSListener`. Do not
   bypass the UDS connect. The fixture must drive the *real*
   `SNSLiveEventDataListener`.
8. **No production code changes.**
9. **Single placeholder test only in this commit.** Do not add tests
   from `subspec04`/`05`/`06` here. The point of this commit is for the
   suite to compile and register; behavioural coverage starts in
   `subspec04`.

---

## 1. Goal of this commit

Stand up an empty-but-buildable integration test header that:

- compiles cleanly to an empty `CxxTest::TestSuite` on Windows
  (`#else` branch — no `MockSMSServer` use);
- on Linux/macOS, defines a fixture (`SNSLiveEventDataListenerTest`)
  with `setUp` / `tearDown` that own a `Poco::TemporaryFile`, a
  `MockSMSServer`, a `TestWatchdog`, and the listener under test;
- provides two free helper functions in the same header —
  `waitFor(pred, timeout, poll)` and
  `extractWithTimeout(listener, timeout)` — that close the remaining
  hang-protection gap left by `subspec02`;
- declares a single `connectListener()` private helper used by every
  subsequent test method;
- registers exactly **one** placeholder test method that does **not**
  start the server, so the suite compiles, links, and registers under
  `ctest` after this commit.

---

## 2. Files touched in this commit

| Action | Path |
|---|---|
| Create | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` |
| Edit | `Framework/LiveData/CMakeLists.txt` (`TEST_FILES` — replace comment placeholder) |
| Edit | `Framework/LiveData/test/CMakeLists.txt` (add `set_tests_properties(... TIMEOUT 120)`) |

No edits to `MockSMSServer.h/.cpp` or the renamed legacy header.

---

## 3. Header file structure

`SNSLiveEventDataListenerTest.h` must follow exactly this top-level
structure:

```cpp
// (licence/copyright header — copy from a neighbouring test file in
//  Framework/LiveData/test/ and update the year as needed)
#pragma once
#ifndef _WIN32

// INTEGRATION TEST.  Drives a real SNSLiveEventDataListener against an
// in-process MockSMSServer over a Unix-domain socket.  Does NOT require
// SMS or any external network resource.  Linux/macOS only — compiles
// to an empty suite on Windows.
//
// For the deferred-run-details invariant see
// SNSLiveEventDataListenerNoNetworkTest.h::test_*.

// ... see §4 and §5 of this sub-spec ...

#else
// Windows stub
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {};
#endif // !_WIN32
```

The Windows stub must be reachable to `cxxtest`; the `cxxtest`
preprocessor reads the header on Windows too and needs to see a class
with no `test_*` methods. The single declaration above suffices.

---

## 4. Linux/macOS body — required content

### 4.1 Includes

```cpp
#include <cxxtest/TestSuite.h>

#include "MantidLiveData/SNSLiveEventDataListener.h"
#include "MantidAPI/ILiveListener.h"
#include "MantidAPI/Workspace_fwd.h"
#include "MantidKernel/ConfigService.h"
#include "MantidTypes/Core/DateAndTime.h"

#include "ADARAPackets.h"     // every byte-array fixture (§5)
#include "MockSMSServer.h"    // from subspec02

#include <Poco/TemporaryFile.h>
#include <Poco/Net/SocketAddress.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace Mantid;
using namespace Mantid::LiveData;
```

Do **not** introduce `using namespace Mantid::LiveData::Testing` at file
scope; refer to its symbols qualified (`Testing::MockSMSServer`,
`Testing::buildRunStatusPkt`, …) to keep call sites unambiguous.

### 4.2 Shorthand macro for verbatim fixture forwarding

For brevity in later sub-specs' test scripts, define **at the top of the
Linux/macOS branch** of `SNSLiveEventDataListenerTest.h`:

```cpp
// Construct a std::vector<uint8_t> from a fixture array in ADARAPackets.h.
#define PKT(name) std::vector<uint8_t>((name), (name) + sizeof(name))
```

Tests in `subspec04`/`05`/`06` use `PKT(geometryPacketV0)` etc. This
macro is part of *this* commit so later commits can compile.

### 4.3 Free helper functions

These are *file-static* (or anonymous namespace) so they don't leak.

```cpp
namespace {

/// Spin-wait up to @p timeout, polling every @p poll, until @p pred
/// returns true.  On timeout calls TS_FAIL and returns false.
template <typename Pred>
bool waitFor(Pred pred,
             std::chrono::milliseconds timeout = std::chrono::seconds{5},
             std::chrono::milliseconds poll    = std::chrono::milliseconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            TS_FAIL("waitFor timed out");
            return false;
        }
        std::this_thread::sleep_for(poll);
    }
    return true;
}

/// Wraps listener.extractData() in std::async.  On timeout (default
/// 10 s) calls TS_FAIL and returns nullptr.  Protects against mutex
/// deadlocks in onBeforeExtract / onBeginRun / onEndRun.
inline std::shared_ptr<API::Workspace>
extractWithTimeout(SNSLiveEventDataListener& listener,
                   std::chrono::seconds timeout = std::chrono::seconds{10}) {
    auto fut = std::async(std::launch::async,
                          [&]{ return listener.extractData(); });
    if (fut.wait_for(timeout) == std::future_status::timeout) {
        TS_FAIL("extractData() timed out — possible deadlock");
        return nullptr;
    }
    return fut.get();
}

} // namespace
```

These two helpers — together with the `MockSMSServer` watchdog and
`TestWatchdog` from `subspec02` — complete the **four** hang-protection
layers described in `overview-spec` §1.

### 4.4 Fixture class skeleton

```cpp
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {
public:
    static SNSLiveEventDataListenerTest *createSuite()
        { return new SNSLiveEventDataListenerTest(); }
    static void destroySuite(SNSLiveEventDataListenerTest *s) { delete s; }

    void setUp() override {
        m_sockFileHandle = std::make_unique<Poco::TemporaryFile>();
        m_sockPath = m_sockFileHandle->path();
        if (m_sockPath.size() >= 100) {
            TS_SKIP("UDS path too long for sun_path: " + m_sockPath);
            return;
        }
        // bind() requires the path to NOT exist:
        std::filesystem::remove(m_sockPath);

        // Save current config; restore in tearDown.
        auto &cfg = Kernel::ConfigService::Instance();
        m_savedKeepPausedEvents =
            cfg.getString("SNSLiveEventDataListener.keepPausedEvents");

        m_server = std::make_unique<Testing::MockSMSServer>(m_sockPath);
        m_watchdog = std::make_unique<Testing::TestWatchdog>(
            std::chrono::seconds{60}, "SNSLiveEventDataListenerTest");
    }

    void tearDown() override {
        // Strict destruction order: listener -> server -> sockfile.
        // The listener's bg thread holds a socket that may be reading;
        // join it before destroying the server.
        m_listener.reset();
        m_server.reset();
        m_sockFileHandle.reset();
        m_watchdog.reset();  // disarm last

        auto &cfg = Kernel::ConfigService::Instance();
        cfg.setString("SNSLiveEventDataListener.keepPausedEvents",
                      m_savedKeepPausedEvents);
    }

    // ----- placeholder test (only test in this commit) -----
    void test_LegacyConstruction_initialState() {
        // Construct the listener WITHOUT calling connectListener();
        // this test never opens the socket.  See subspec04 §6.1 for
        // the rationale — this test must observe the disconnected
        // initial state.
        m_listener = std::make_unique<SNSLiveEventDataListener>();
        TS_ASSERT(!m_listener->isConnected());
        TS_ASSERT_EQUALS(m_listener->runStatus(),
                         API::ILiveListener::NoRun);
    }

    // ----- (additional test_* methods added in subspec04 / 05 / 06) -----

private:
    // Each behavioural test calls this AFTER queuing the server script.
    // Returns true on success.  Builds the UDS SocketAddress via the
    // AddressFamily::UNIX_LOCAL enum form — NOT a "host:port" string.
    bool connectListener(
        Types::Core::DateAndTime startTime = Types::Core::DateAndTime()) {
        m_listener = std::make_unique<SNSLiveEventDataListener>();
        Poco::Net::SocketAddress udsAddr(
            Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
        if (!m_listener->connect(udsAddr)) return false;
        m_listener->start(startTime);
        return true;
    }

    std::string m_savedKeepPausedEvents;
    std::unique_ptr<Poco::TemporaryFile> m_sockFileHandle;
    std::string m_sockPath;
    std::unique_ptr<Testing::MockSMSServer> m_server;
    std::unique_ptr<SNSLiveEventDataListener> m_listener;
    std::unique_ptr<Testing::TestWatchdog> m_watchdog;
};
```

### 4.5 Fixture notes (must be preserved verbatim as the explanatory
header in the test file)

Add these as a `/// Notes:` comment block immediately above the class:

- **No listener constructed in the fixture constructor.** Each test
  calls `connectListener()` *after* queuing the script. Some tests
  (e.g. `test_connectFailure_returnsFalse` in `subspec06`) want to drive
  `connect()` against a server that is *not* yet listening, so the
  fixture must not eagerly connect.
- **`connectListener()` calls `connect()` first, then `start()`.**
  This is the required lifecycle: the socket must be connected before
  the background thread can begin reading.
- **The UDS address is built via
  `Poco::Net::AddressFamily::UNIX_LOCAL`** using the two-argument
  `SocketAddress(AddressFamily::UNIX_LOCAL, path)` constructor. Do
  **not** set the `SNSLiveEventDataListener.testAddress` config key;
  that key supports only TCP `"host:port"` form.
- **Destruction order in `tearDown` is strict:** listener → server →
  sockfile → watchdog. Reordering risks the listener's bg thread
  reading from a destroyed socket.

---

## 5. CMake registration

### 5.1 `Framework/LiveData/CMakeLists.txt`

Locate the existing comment placeholder in `TEST_FILES`:

```cmake
# Needs fixing to not rely on network. SNSLiveEventDataListenerTest.h
```

Replace it with:

```cmake
SNSLiveEventDataListenerTest.h
# (SNSLiveEventDataListenerLegacyTest.h is intentionally NOT registered —
#  retained as historical reference.)
```

Keep `TEST_FILES` alphabetised. Do **not** add an entry for
`SNSLiveEventDataListenerLegacyTest.h`.

### 5.2 `Framework/LiveData/test/CMakeLists.txt`

Near the existing `set_tests_properties(LiveDataTest_MonitorLiveDataTest
PROPERTIES RUN_SERIAL TRUE)` line (currently line 32), add:

```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*
> The `TIMEOUT 120` setting *is* part of the agent's deliverable. The
> runtime behaviour of `ctest` enforcing this ceiling is observable
> only at review time.

Do not touch `TESTHELPER_SRCS` in this commit — that line was already
modified in `subspec02`.

---

## 6. TODO

- [ ] Create `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`
      with:
      - the copyright/licence header,
      - `#pragma once`,
      - the platform-guarded structure in §3,
      - the integration-test file-level doc comment in §3,
      - the includes in §4.1,
      - the `PKT(name)` macro in §4.2,
      - the anonymous-namespace `waitFor` and `extractWithTimeout` in
        §4.3,
      - the fixture class skeleton in §4.4 (with the placeholder test
        `test_LegacyConstruction_initialState`),
      - the explanatory notes in §4.5.
- [ ] Edit `Framework/LiveData/CMakeLists.txt`: replace the
      `# Needs fixing …` comment in `TEST_FILES` with the entry shown
      in §5.1, preserving alphabetical order.
- [ ] Edit `Framework/LiveData/test/CMakeLists.txt`: append the
      `set_tests_properties(... TIMEOUT 120)` block shown in §5.2 near
      the existing `RUN_SERIAL` property.
- [ ] Confirm that the only `test_*` method in this commit is
      `test_LegacyConstruction_initialState`. All other §6.x tests are
      added in `subspec04`–`subspec06`.
- [ ] Confirm `connect()` is called with the
      `Poco::Net::AddressFamily::UNIX_LOCAL` form (no `"unix:..."`
      string).
- [ ] Confirm `TestableSNSListener` does **not** appear in this header.

---

## 7. Definition of done for this commit

1. `SNSLiveEventDataListenerTest.h` exists under
   `Framework/LiveData/test/` with the structure in §3–§4 and exactly
   one `test_*` method (`test_LegacyConstruction_initialState`).
2. `Framework/LiveData/CMakeLists.txt` lists
   `SNSLiveEventDataListenerTest.h` in `TEST_FILES` (alphabetical) and
   does not list `SNSLiveEventDataListenerLegacyTest.h`.
3. `Framework/LiveData/test/CMakeLists.txt` contains the
   `set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
   PROPERTIES TIMEOUT 120)` line.
4. The Windows branch (`#else`) defines a class
   `SNSLiveEventDataListenerTest : public CxxTest::TestSuite {}` and
   nothing else.
5. No file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/` is modified by this commit.
6. No edits to `MockSMSServer.h/.cpp` or the legacy test header in this
   commit.
