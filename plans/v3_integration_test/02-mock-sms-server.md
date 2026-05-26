# Sub-spec 02 — MockSMSServer

**Part of:** [`00-index.md`](00-index.md) (master)  
**Agent read-order:** Read after [`01-uds-transport.md`](01-uds-transport.md).

Packet-builder details are in a separate sub-spec:
[`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md).

---

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
#pragma once
#ifndef _WIN32

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <condition_variable>
#include <string>
#include <variant>
#include <vector>

#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Thread.h>

namespace Mantid::LiveData::Testing {

// One step in the server's playback script.
struct PktGarbage   { std::vector<uint8_t> bytes; };
struct PktDisconnect{};
struct PktWaitForExtract {};  // gate: blocks until the test signals
                              // (used to deterministically interleave
                              //  extractData() with packet delivery).

using ScriptEntry = std::variant<
    std::vector<uint8_t>,   // raw packet bytes (preferred — built by the
                            // helpers in 03-adara-packet-fixtures.md)
    PktGarbage,
    PktDisconnect,
    PktWaitForExtract>;

class MockSMSServer {
public:
    // path: absolute UDS path (must not exist when start() is called).
    explicit MockSMSServer(std::string path);
    ~MockSMSServer();  // joins server thread; closes sockets; unlinks path.

    MockSMSServer(const MockSMSServer&) = delete;
    MockSMSServer& operator=(const MockSMSServer&) = delete;

    // Begin listening.  Returns immediately; accept() happens on bg thread.
    // Must be called BEFORE the listener calls connect().
    void start();

    // Append script entries.  ALL entries must be queued before the
    // listener's background thread begins reading (i.e. before start()).
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
    // rather than hanging.
    void setWatchdog(std::chrono::seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// RAII watchdog: if not disarmed within the deadline, calls
/// g_log.fatal and std::abort().  Arm at the top of every test body
/// that drives the listener; disarmed by fixture tearDown.
class TestWatchdog {
public:
    explicit TestWatchdog(std::chrono::seconds deadline,
                          std::string testName);
    ~TestWatchdog(); // disarms if still armed
    void disarm();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Mantid::LiveData::Testing

#endif // !_WIN32
```

### 4.2 Lifetime / threading rules

- **Constructor** only initialises members; it does **not** bind or listen.
- **`start()`** creates the `Poco::Net::ServerSocket` bound to
  `Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_path)`,
  sets backlog=1, then launches the background thread.
  The background thread calls `accept()` (blocking) for exactly one client
  connection, then drains the script sequentially.
- **Destructor** sets a stop flag, shuts down the sockets (to break any
  blocked `accept` or `send`), joins the thread with a bounded timeout (5 s);
  if the join fails, calls `g_log.fatal` and `std::abort()` — a hung test
  process is worse than a crashed one.
- The destructor calls `::unlink(m_path.c_str())` (not `std::filesystem::remove`)
  so that on Linux the socket file is removed even if `accept()` never
  completed.
- The `Poco::TemporaryFile` that owns the path lifetime lives in the **test
  fixture** and is destroyed *after* the `MockSMSServer`, so the path's
  lifetime strictly outlives the server.

### 4.3 Driving modes

The server thread iterates the script vector.  For each entry:

- `std::vector<uint8_t>` → `m_clientSocket.sendBytes(data.data(), data.size())`.
  Advance `m_bytesSent` and `m_scriptIndex`.
- `PktGarbage` → send the arbitrary bytes (used to verify
  `ADARA::invalid_packet` propagation, §6.9 in
  [`04-test-scenarios.md`](04-test-scenarios.md)).
- `PktDisconnect` → `m_clientSocket.close()`; mark "client gone"; stop.
- `PktWaitForExtract` → acquire the condition-variable mutex and wait on
  the CV until `releaseExtractGate()` is called or the watchdog fires;
  then continue with the next script entry.

No `Poco::Thread::sleep` between entries by default. Inter-entry delays, if
ever needed, can be expressed as a separate `PktDelay{ms}` entry — left
out of v1 to keep the surface minimal.

### 4.4 Thread-safety contract

- `script()` / `scriptAppend()` are only safe to call **before** `start()`.
  The script vector is read-only once the background thread is running.
- `releaseExtractGate()` is safe to call from the foreground test thread
  at any time after `start()`.
- `bytesSent()` / `scriptIndex()` / `clientConnected()` are protected by a
  `std::mutex`.

### 4.5 Include requirements

`MockSMSServer.cpp` must include:

```cpp
#include "MockSMSServer.h"
#include "ADARAPackets.h"           // binary exemplar arrays
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/NetException.h>
#include <Poco/Thread.h>
#include "MantidKernel/Logger.h"    // g_log.fatal for abort path
#include <sys/socket.h>             // ::unlink
#include <unistd.h>
```

`MockSMSServer.h` must only include lightweight headers (no Poco sockets —
those go in the `.cpp` via the `Impl` pimpl).

### 4.6 `waitFor` and `extractWithTimeout` helpers

These free functions live in `SNSLiveEventDataListenerTest.h` (not in
`MockSMSServer.h`) because they reference CxxTest macros:

```cpp
/// Spin-wait up to @p timeout, polling every @p poll, until @p pred
/// returns true.  Calls TS_FAIL and returns false on timeout.
template <typename Pred>
bool waitFor(Pred pred,
             std::chrono::milliseconds timeout = std::chrono::seconds{5},
             std::chrono::milliseconds poll    = std::chrono::milliseconds{10});

/// Wraps listener.extractData() in std::async.  On timeout (default 10 s)
/// calls TS_FAIL and returns nullptr.
std::shared_ptr<API::Workspace>
extractWithTimeout(SNSLiveEventDataListener& listener,
                   std::chrono::seconds timeout = std::chrono::seconds{10});
```

These replace every bare `extractData()` call and every bare
`Poco::Thread::sleep` in assertions.

### 4.7 Hang protection — four layers

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*
> This section describes the runtime behaviour the maintainer will observe
> when invoking `ctest` during PR review.

A test that hangs at runtime blocks the entire `ctest` invocation.
Defence in depth:

1. **`waitFor(pred, timeout=5s, poll=10ms)`** — replaces every bare sleep.
   On timeout calls `TS_FAIL` with a descriptive message and returns false.
2. **`extractWithTimeout(listener, timeout=10s)`** — wraps `extractData()`
   in `std::async` + `std::future::wait_for`. On timeout calls `TS_FAIL`
   and returns `nullptr`. This protects against mutex deadlocks in
   `onBeforeExtract` / `onBeginRun` / `onEndRun`.
3. **`MockSMSServer` self-watchdog** — if the script is not exhausted within
   `setWatchdog()` seconds (default 30 s), the server closes its client
   socket so the listener observes EOF and surfaces the failure via its
   normal exception path (`SNSLiveEventDataListener.cpp:274-278, 336-342`).
4. **`TestWatchdog` RAII helper** — constructed at the top of every test
   that drives the listener; arms a background thread that, if not disarmed
   within 60 s, calls `g_log.fatal` and `std::abort()`. Disarmed by
   the fixture's `tearDown`.

These limits are deliberately loose (5 / 10 / 30 / 60 s) — they exist to
catch *bugs*, not to enforce performance. Healthy tests complete in < 1 s.

### 4.8 Suite-wide ctest timeout

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*

The CMake edit (see §7 in [`00-index.md`](00-index.md)):
```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```
sets the last-resort guard: `ctest` kills the binary after 120 s.
If it fires, `TestWatchdog` failed — that is itself a bug.
