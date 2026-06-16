# `SNSLiveEventDataListener` Poll-Loop Refactoring — Implementation Discussion

This document is a retrospective companion to `plans/poll-loop-refactoring-v1.md`.
It records the *why* behind the implementation choices and the non-obvious
findings that emerged during development. Read it when you need to understand why
the code is shaped the way it is, or before touching the `run()` loop or the
disconnect-detection tests.

______________________________________________________________________

## 1. Overview of the refactoring

### Problem

Before this change, `SNSLiveEventDataListener::run()` waited for data with a
raw blocking `receiveBytes()` call and a 30-second socket receive timeout.
The loop did not detect orderly peer-close:

- If the SMS server called `close()`, `receiveBytes()` returned `0`.
- The return value was not checked for `0`.
- The loop continued as if nothing happened.
- `m_isConnected` remained `true`, `listenerState()` returned `Connected`.

Two tests — `test_LegacyConnectAndDisconnect` and
`test_serverDisconnect_setsErrorState` — were guarded with
`TS_WARN("XFAIL: ...")` + `return;` because of this defect.

A secondary problem: a short write on the hello packet used
`g_log.error(...)` + `m_stopThread = true` and let the thread exit
*without* populating `m_backgroundException`. The listener appeared
`Disconnected`, not `Error`, and callers could not recover the failure
cause.

### Solution

The new loop is `Poco::Net::PollSet`-driven:

```
pollSet registered before the loop (once)
    ↕
loop: check m_stopThread
    ↕
    pause-loop (m_bgThreadCaughtUp + m_pauseNetRead, unchanged)
    ↕
    poll(100 ms)
    ↕
    if POLL_READ: receiveBytes() if buffer has free space
        - recv==0  → fatal("server disconnected...")
        - recv>0   → bufferBytesAppended(n)
        - TimeoutException → transient, skip this iteration
        - NetException → fatal("network read failed: <name>")
    ↕
    if POLL_ERROR: fatal("socket poll reported error.")
    ↕
    bufferParse() unconditionally  ← critical, see §3
```

Every fatal failure path goes through a single `fatal` lambda that:

1. Logs the message.
1. Sets `m_isConnected = false`.
1. Stores `make_shared<std::runtime_error>(msg)` in `m_backgroundException`
   (first-writer-wins; see §4).
1. Throws `std::runtime_error(msg)` so the existing outer catch cascade
   lands the thread cleanly.

A new virtual `sendHelloPacket(const void *, int)` wraps the hello-send
with a write-all retry loop and serves as a test seam (see §5).

**Relevant source locations:**

- `Framework/LiveData/src/SNSLiveEventDataListener.cpp` — `run()` at line
  233; `sendHelloPacket()` at line 215.
- `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` —
  `m_pauseNetRead`, `m_bgThreadCaughtUp`, `m_stopThread` (protected atomics).

______________________________________________________________________

## 2. `bufferParse()` must run unconditionally — why this matters

The ADARA parser maintains an internal buffer. `bufferFillLength()` returns
the *free space remaining*. When it returns `0`, the buffer is **full of
unparsed bytes**; the only way to make room for new reads is to parse.

The naive implementation would write:

```cpp
if (mode & POLL_READ) {
    if (bufFillLen) { recv...; bufferBytesAppended(n); }
    bufferParse(...);   // ← WRONG: inside POLL_READ branch
}
```

When the buffer fills, `bufFillLen == 0` on the next iteration. The
`if (bufFillLen)` block is skipped (correctly), but if `bufferParse()` is
also inside the `if (POLL_READ)` block, and POLL_READ is signaled (which
it will be — the kernel sees unread data), the code falls through to
`bufferParse()`. But consider a subtle variant: if `bufferParse()` is only
reached *inside* the `if (bufFillLen)` block, the buffer stays full, the
kernel keeps signaling POLL_READ, and the loop spins with no progress.

The correct implementation calls `bufferParse()` **outside and after** both
the POLL_READ and POLL_ERROR branches, on every iteration:

```cpp
// parse runs unconditionally every iteration
m_bgThreadCaughtUp.store(false, std::memory_order_release);
int packetsParsed = bufferParse(bufferParseLog);
m_bgThreadCaughtUp.store(true, std::memory_order_release);
```

This is exactly what the current code does (lines 343–349 of `run()`), and
it is what the old blocking loop did too. Do not move it inside either event
branch.

The `m_bgThreadCaughtUp` `false/true` bracketing around `bufferParse()` is
also load-bearing: it prevents the foreground from snapshotting state while
the bg thread is mid-parse (see §6).

______________________________________________________________________

## 3. The epoll surprise — `EPOLLHUP` maps to `POLL_READ`, not `POLL_ERROR`

This was the most surprising finding during implementation, and the one most
likely to mislead a future maintainer.

### What we expected

Clean server `close()` → Linux delivers `EPOLLERR` → Poco maps to
`POLL_ERROR` → our `if (mode & POLL_ERROR)` branch fires → `fatal(...)`.

### What actually happens

On Linux (both TCP and Unix domain sockets), a clean `close()` by the peer
delivers **`EPOLLIN | EPOLLHUP`** (event mask `0x11`), *not* `EPOLLERR`.

Poco's `PollSet` maps these kernel flags as follows:

| kernel epoll flag | Poco PollSet flag        |
| ----------------- | ------------------------ |
| `EPOLLIN`         | `POLL_READ`              |
| `EPOLLHUP`        | `POLL_READ` (not ERROR!) |
| `EPOLLERR`        | `POLL_ERROR`             |
| `EPOLLOUT`        | `POLL_WRITE`             |

This means `PktDisconnect{}` — which calls `m_clientSocket.close()` in
`MockSMSServer` — **never** triggers the `POLL_ERROR` branch. Disconnect
detection fires through the `POLL_READ` branch when `receiveBytes()`
returns `0`.

### Empirical verification

A minimal Python reproducer confirms the mask:

```python
import socket, select, os

a, b = socket.socketpair()
ep = select.epoll()
ep.register(a.fileno(), select.EPOLLIN | select.EPOLLHUP | select.EPOLLERR)
b.close()
events = ep.poll(1.0)
# events → [(fd, 0x11)]  ← EPOLLIN(0x01) | EPOLLHUP(0x10), NOT EPOLLERR(0x08)
print(hex(events[0][1]))  # 0x11
```

Both cases — "close with data still in the buffer" and "close after drain"
— return `0x11`.

### Implications for the test suite

- `PktDisconnect{}` tests exercise the `recv()==0` (zero-byte-read) branch,
  NOT `POLL_ERROR`.
- `PktAbruptClose{}` (raw `::close(fd)` without draining the kernel send
  buffer) is the closest we can get to `POLL_ERROR` deterministically, but
  even then `EPOLLIN|EPOLLHUP` may still arrive together (mask `0x11`), in
  which case the zero-byte-read branch fires first. `test_pollError_setsError_via_abruptClose`
  is best-effort coverage of the `POLL_ERROR` branch; it asserts on state,
  not on which branch fired.
- The test comment in `test_pollError_setsError_via_abruptClose` acknowledges
  this: if the kernel delivers `POLLIN|POLLHUP`, the error message will come
  from the `recv()==0` path instead of the POLL_ERROR path, and that is
  acceptable.

### POLL_READ must be processed before POLL_ERROR

When a peer closes with pending bytes still in the kernel's receive buffer
(the common case when `PktDisconnect{}` follows data packets), epoll delivers
`EPOLLIN|EPOLLHUP` in a single `epoll_wait` return. If we check `POLL_ERROR`
first and call `fatal()`, we discard those trailing bytes — often the final
`END_RUN` packet. The loop therefore processes `POLL_READ` first:

```cpp
if (mode & POLL_READ) { /* read path */ }
if (mode & POLL_ERROR) { /* only reached if POLL_READ did not already call fatal() */ }
```

______________________________________________________________________

## 4. First-writer-wins on `m_backgroundException`

The `fatal` lambda stores the exception only if one isn't already set:

```cpp
if (!m_backgroundException)
    m_backgroundException = std::make_shared<std::runtime_error>(msg);
```

This matters because the outer catch handlers at the bottom of `run()` also
contain `if (!m_backgroundException)` guards. A `std::runtime_error` thrown
by `fatal()` is caught by the `catch (std::runtime_error &e)` arm; without
the guard, that handler would overwrite `fatal()`'s precise message with a
more generic one.

With the guard in place, the first message to reach `m_backgroundException`
wins. Callers who read `runState()` or `lastTransition()` get the original,
specific message (e.g. `"SNSLiveEventDataListener: server disconnected while reading ADARA stream."`) rather than a re-wrapped copy.

______________________________________________________________________

## 5. `sendHelloPacket()` virtual seam

The hello packet send (`m_socket.sendBytes(helloPkt, sizeof(helloPkt))`)
cannot be made to fail from the server side: on UDS the kernel accepts a
20-byte write atomically. Testing the short-write failure path requires an
in-process override.

The solution is a small protected virtual:

```cpp
// in SNSLiveEventDataListener.h (protected):
virtual int sendHelloPacket(const void *buf, int size);
```

The default implementation contains a write-all retry loop (in case of a
genuine partial send on a slow/full socket). A test subclass can override
it to return a short count on the first call, validating that the partial-
send detection works.

`TestableSNSListener` in `SNSLiveEventDataListenerNoNetworkTest.h` and the
inline `ShortWriteListener` in `test_partialHelloSend_setsError` both use
this seam.

______________________________________________________________________

## 6. `Poco::TimeoutException` is not a `Poco::Net::NetException`

These two classes are siblings under `Poco::Exception`, not parent/child.
A `catch (Poco::Net::NetException &)` does **not** catch `Poco::TimeoutException`.

This matters in the receive path:

```cpp
try {
    bytesRead = m_socket.receiveBytes(bufFillAddr, bufFillLen);
} catch (Poco::TimeoutException &) {
    // Spurious POLL_READ — transient, skip this iteration.
    bytesRead = -1;
} catch (Poco::Net::NetException &e) {
    fatal(std::string("SNSLiveEventDataListener: network read failed: ") + e.name());
}
```

Without the explicit `Poco::TimeoutException` arm, a single spurious
POLL_READ event (rare on epoll but theoretically possible) would terminate
the listener with a generic `std::exception` message rather than recovering.

Since we now use `PollSet` to gate readiness, `TimeoutException` from
`receiveBytes()` should be very rare; but it is not impossible (false-positive
POLL_READ is allowed by the POSIX/Linux spec as a platform hint rather than a
guarantee). Catching it as transient is the correct response.

______________________________________________________________________

## 7. `m_bgThreadCaughtUp` bracketing is load-bearing

`m_bgThreadCaughtUp` is used by `doExtractData()` to decide whether it is
safe to snapshot the listener's run-transition state:

```cpp
// foreground (doExtractData):
while (m_bgThreadCaughtUp.load()) { /* pause loop */ }
```

The bg thread sets it `false` before entering `bufferParse()` and `true`
after:

```cpp
m_bgThreadCaughtUp.store(false, ...);
bufferParse(...);
m_bgThreadCaughtUp.store(true, ...);
```

This prevents the foreground from snapshotting `m_pendingTransition` and
`m_pauseNetRead` mid-parse, when they may be in a transient state from
`rxPacket()` calls inside `bufferParse()`.

The bracketing must remain exclusively around `bufferParse()`. Moving it
outward (e.g. to surround the entire poll + read + parse sequence) would
block the foreground unnecessarily. Moving it inward or removing it would
allow the foreground to snapshot mid-parse.

______________________________________________________________________

## 8. The "rapid disconnect" race in algorithm-integration tests

Enabling real disconnect detection exposed a race in three algorithm-level
tests that used `PktDisconnect{}` as a convenient script terminator.

### What broke

`test_LoadLiveData_standalone_no_deadlock`,
`test_MonitorLiveData_workspace_renaming_unchanged`, and
`test_MonitorLiveData_BeginRun_post_rename` all ended their server scripts
with `PktDisconnect{}`. Before the refactor this was harmless — EOF was never
detected. After the refactor the bg thread detected EOF before the foreground
algorithm could call `extractData()` to commit the final run transition.

### Why it's racy

On Linux UDS, `EPOLLIN|EPOLLHUP` (mask `0x11`) is delivered in a **single**
`epoll_wait` return even when bytes remain unread. The kernel returns data on
the first `recv()` call and `0` on the very next one. There is no kernel-
guaranteed gap between "last byte received" and "EOF detected" — only the
latency of one loop iteration (~10–100 ms). A MonitorLiveData retry loop
running at 50 ms intervals has no reliable window in which to observe the
final data packet without also racing the EOF.

### Two mitigation patterns

**Pattern A — double gate (for single-extract tests):**

Wrap the final data + `PktDisconnect{}` in a second `PktWaitForExtract{}`
gate:

```
…data packets…
PktWaitForExtract{}    ← gate 1: holds connection while algorithm extracts
PktWaitForExtract{}    ← gate 2: holds connection during assertion, released after
PktDisconnect{}
```

Test code waits for `scriptIndex >= N` (guaranteeing all data is in the
network buffer), extracts and asserts, then releases gate 2. The connection
stays alive throughout extraction. Used in `test_LoadLiveData_standalone_no_deadlock`.

**Pattern B — no disconnect (for loop-based monitor tests):**

Drop `PktDisconnect{}` entirely from the script. The server's background
thread exits its script loop, but `m_clientSocket` remains alive in
`MockSMSServer::Impl`. `tearDown()` calls `m_server.reset()` which destructs
the server and closes the socket cleanly — after the algorithm has already
committed all transitions. Used in `test_MonitorLiveData_workspace_renaming_unchanged`
and `test_MonitorLiveData_BeginRun_post_rename`.

### Rule of thumb for future tests

Only append `PktDisconnect{}` to a script when the test is **specifically
asserting on the `Error` state transition**. If `PktDisconnect{}` is just
tidying up after the useful part of the test, use tearDown cleanup instead.

______________________________________________________________________

## 9. Other implementation subtleties

### `m_pauseNetRead` is a protected member

`m_pauseNetRead` cannot be written directly from a test that holds a
`SNSLiveEventDataListener *` (it's protected). `TestableSNSListener` in
`SNSLiveEventDataListenerNoNetworkTest.h` gained a small helper:

```cpp
void setPauseNetRead(bool value) {
    m_pauseNetRead.store(value, std::memory_order_release);
}
```

This is the only way to drive back-pressure from outside the class in tests.

### XFAIL guards removed atomically

Both `test_LegacyConnectAndDisconnect` and `test_serverDisconnect_setsErrorState`
previously had:

```cpp
TS_WARN("XFAIL: disconnect detection not yet implemented.");
return;
```

These were removed atomically with the implementation. Their `sleep_for(200ms)`
waits were replaced with `waitFor([&]{ return listenerState() == Error; }, 5s)`
polls, per project convention (see `feedback_test_sync.md`).

### Destructor join timeout is not yet tightened

`SNSLiveEventDataListener::~SNSLiveEventDataListener()` uses
`m_thread.join(RECV_TIMEOUT * 2 * 1000)`, where `RECV_TIMEOUT` was `30`,
giving a 60-second join window. `RECV_TIMEOUT` was reduced to `1` (one
second) as part of this refactor (bounding a false-positive `receiveBytes()`
block to one second), so the join deadline is now `2000 ms`. Actual shutdown
with PollSet at 100 ms is sub-second in normal operation.

### `setReceiveTimeout()` left as defence-in-depth

`connect()` still calls `m_socket.setReceiveTimeout(Poco::Timespan(RECV_TIMEOUT, 0))`.
With PollSet governing readiness, this timeout is effectively unreachable in
normal operation. It is kept as a backstop against a platform delivering a
persistent spurious POLL_READ that escapes the `TimeoutException` catch — in
that case `receiveBytes()` blocks for at most one second before returning.

### Error message prefix convention

Per user convention, error messages use the prefix
`"SNSLiveEventDataListener: "` (no `::run()`). The `::run()` decoration was
explicitly removed because end-users read these messages in the Mantid log
and the function name is irrelevant to them:

```
SNSLiveEventDataListener: server disconnected while reading ADARA stream.
SNSLiveEventDataListener: socket poll reported error.
SNSLiveEventDataListener: network read failed: <NetException name>
SNSLiveEventDataListener: short write on client hello packet.
```

______________________________________________________________________

## 10. Cross-references

| Topic                                                                    | File / location                                                              |
| ------------------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| `run()` loop                                                             | `Framework/LiveData/src/SNSLiveEventDataListener.cpp:233`                    |
| `sendHelloPacket()`                                                      | Same file, line 215                                                          |
| `fatal` lambda                                                           | Same file, line 237                                                          |
| Protected atomics                                                        | `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`           |
| `MockSMSServer` (`PktDisconnect`, `PktAbruptClose`, `PktWaitForExtract`) | `Framework/LiveData/test/MockSMSServer.h` / `MockSMSServer.cpp`              |
| `TestableSNSListener::setPauseNetRead`                                   | `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`            |
| Disconnect tests §6.9                                                    | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`                     |
| Algorithm race patterns                                                  | `Framework/LiveData/test/SNSLiveEventDataListenerAlgorithmIntegrationTest.h` |
| Implementation plan                                                      | `plans/poll-loop-refactoring-v1.md`                                          |
| Original plan critique                                                   | `plans/poll-loop-refactoring.md`                                             |
