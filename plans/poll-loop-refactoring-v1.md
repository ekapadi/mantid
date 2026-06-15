# SNSLiveEventDataListener `run()` Refactor Plan

## Goal

Refactor `SNSLiveEventDataListener::run()` to:

1. Replace the current raw blocking socket-read loop with a `Poco::Net::PollSet`-based loop.
1. Treat unexpected server disconnects as fatal errors.
1. Propagate disconnects and socket failures through the existing `m_backgroundException` mechanism using `std::runtime_error`.
1. Preserve all existing parser, run-transition, and back-pressure semantics.

This change is intentionally limited to the `run()` path, closely related shutdown behavior, and the two tests that currently express the desired contract as XFAIL.

______________________________________________________________________

## Current problems

### 1. Orderly server disconnect is not handled

If `m_socket.receiveBytes()` returns `0`, the code does not treat that as a disconnect. It does not set `m_isConnected = false`, does not populate `m_backgroundException`, and does not terminate the thread. That leaves the listener appearing connected when the peer has already closed the socket. Two tests
(`test_LegacyConnectAndDisconnect`, `test_serverDisconnect_setsErrorState` in `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`) currently sit behind `TS_WARN("XFAIL: ...") + return;` preambles because of this.

### 2. Read loop is timeout-driven instead of event-driven

The current implementation blocks in `receiveBytes()` and relies on socket receive timeout behavior to periodically regain control. This makes shutdown and error handling less explicit than they should be.

### 3. Disconnect/error behavior is inconsistent

Thrown network exceptions already become fatal background-thread failures, but orderly peer shutdown does not. A partial hello-send also exits the thread *without* populating `m_backgroundException`, so `listenerState()` does not report `Error`. The refactor should make all unexpected socket termination paths converge on the same failure model.

______________________________________________________________________

## Desired behavior

### Runtime behavior

- Use `Poco::Net::PollSet` to wait for socket readiness with a short timeout (≈100 ms).
- Keep the existing `m_pauseNetRead` and `m_bgThreadCaughtUp` semantics.
- Only call `receiveBytes()` when the socket is reported readable *and* the parser buffer has free space.
- Always call `bufferParse()` on each loop iteration (independently of whether bytes were just read), so a full parse buffer can be drained.
- If `receiveBytes()` returns `0`, treat that as a fatal server disconnect.
- If poll reports a socket error, treat that as fatal.
- If `receiveBytes()` throws `Poco::Net::NetException`, rewrap it as a `std::runtime_error` carrying the NetException name.
- Hello-packet send-failure (short write) flows through the same fatal-error helper.

### Public-state behavior

This preserves current external semantics, and additionally guarantees:

- `listenerState()` transitions `Connected → Error` (no intermediate `Disconnected` flicker) when a peer-close or socket error occurs, because the helper sets `m_backgroundException` before the next observable state read.
- `runState()` and `lastTransition()` rethrow `m_backgroundException` as today.
- `doExtractData()` surfaces the stored background exception during startup waits, as today.

______________________________________________________________________

## Non-goals

- No reconnect or retry logic.
- No `SocketReactor` conversion.
- No redesign of run-transition handling.
- No changes to packet parsing behavior beyond how bytes arrive at `bufferParse()`.
- No changes to the back-pressure invariant (the short-circuit between `m_bgThreadCaughtUp` and `m_pauseNetRead` in the pause loop).

______________________________________________________________________

## Required code changes

### 1. Add `PollSet` include

Add `#include <Poco/Net/PollSet.h>` alongside the other Poco net includes in `SNSLiveEventDataListener.cpp`.

### 2. Introduce a fatal-failure helper local to `run()`

Centralize fatal socket-failure handling. A lambda inside `run()` (or a small private member) that:

- Accepts an error message string.
- Logs it.
- Sets `m_isConnected = false`.
- Stores `std::make_shared<std::runtime_error>(msg)` in `m_backgroundException` if not already set.
- Throws `std::runtime_error(msg)` so the existing outer catch blocks land the thread cleanly.

This helper is used by **all** of the following failure paths so they share identical state-machine outcomes:

- POLL_ERROR returned from the poll set.
- `receiveBytes() == 0` (orderly peer close).
- `Poco::Net::NetException` thrown from `receiveBytes()`.
- Short-write on the hello packet (partial send).

The helper does **not** set `m_stopThread`. The bg thread exits because `run()` returns after the outer catch handler runs; this mirrors the existing exception-driven shutdown of the read loop.

### 3. Replace raw receive wait with `PollSet`

Inside `run()`, after the hello packet has been sent:

- Construct a `Poco::Net::PollSet` **once**, before the loop.
- `add(m_socket, Poco::Net::PollSet::POLL_READ)`.
- Reuse it on every iteration.

The loop body, in order:

1. Honor `m_stopThread`.
1. Honor the existing pause loop (gated by both `m_bgThreadCaughtUp` and `m_pauseNetRead`, exactly as today).
1. Call `pollSet.poll(Poco::Timespan(0, 100000))` — 100 ms.
1. If a `POLL_ERROR` is reported for `m_socket`, call the failure helper.
1. If `POLL_READ` is reported **and** `bufferFillLength() > 0`, call `receiveBytes()`:
   - If `bytesRead == 0`: fatal disconnect via helper.
   - If `bytesRead > 0`: call `bufferBytesAppended(bytesRead)`.
   - If `Poco::Net::NetException` is thrown: fatal via helper with NetException-name in the message.
1. **Unconditionally** parse: `m_bgThreadCaughtUp = false` → `bufferParse(...)` → `m_bgThreadCaughtUp = true`.
1. If `packetsParsed == 0`, sleep 10 ms (preserves the current spin-avoidance behavior).

This ordering preserves the invariant that a full parse buffer (`bufferFillLength() == 0`) is drained on the next iteration: the read is skipped but `bufferParse()` still runs.

### 4. Handle orderly disconnect explicitly

On `bytesRead == 0`, call the helper with:

> `SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.`

Do not continue looping.

### 5. Preserve back-pressure semantics

The pause loop:

```cpp
while (m_bgThreadCaughtUp.load(std::memory_order_acquire) &&
       m_pauseNetRead.load(std::memory_order_acquire) &&
       !m_stopThread.load(std::memory_order_acquire)) {
  Poco::Thread::sleep(100);
}
```

remains unchanged. The short-circuit (`m_bgThreadCaughtUp` first) is load-bearing: it ensures the bg thread cannot be paused mid-`bufferParse()`, which would leave shared state (`m_pendingTransition`, `m_pauseNetRead`) inconsistent for the foreground snapshot. Keep the `m_bgThreadCaughtUp` `false/true` bracketing around `bufferParse()` and only around `bufferParse()`.

### 6. Hello-packet send is also fatal-via-helper

Replace the current `g_log.error(...); m_stopThread.store(true, ...);` pattern with a helper call so a short write surfaces through `m_backgroundException` and `listenerState() == Error`.

### 7. Outer exception handlers

The existing `catch` cascade at the bottom of `run()` (ADARA::invalid_packet, std::runtime_error, std::invalid_argument, std::exception, ...) is retained. Helper-thrown `std::runtime_error`s are caught by the existing `std::runtime_error` arm; because the helper already populated `m_backgroundException`, the `if (!m_backgroundException)` guard in that handler will leave the helper's message in place.

### 7a. Partial-read and transient-error handling (critical — do not skip)

The ADARA parser tolerates partial reads natively (`bufferBytesAppended(N)` for any valid `N`; `bufferParse()` consumes whole packets and leaves fragments for the next iteration), but the **transient error cascade around `receiveBytes()` must be preserved** or the new code will treat recoverable conditions as fatal.

**Receive-side cascade — must catch in this order:**

```cpp
int bytesRead = -1;
try {
  bytesRead = m_socket.receiveBytes(bufFillAddr, bufFillLen);
} catch (Poco::TimeoutException &) {
  // POLL_READ false positive (rare on epoll, not zero) or kernel quirk.
  // Treat as transient: skip the read this iteration, fall through to
  // bufferParse(), and let the next poll() decide.  Do NOT log on every
  // hit — the bg loop fires every ~100 ms and would flood the log.
  bytesRead = -1;
} catch (Poco::Net::NetException &e) {
  // ConnectionResetException / ConnectionAbortedException / etc.  All
  // genuinely fatal — connection is gone, helper terminates the loop.
  fatal(std::string("SNSLiveEventDataListener::run(): network read failed: ") + e.name());
}
```

Notes:

- `Poco::TimeoutException` is a sibling of `Poco::Net::NetException` under `Poco::Exception`, **not** a subclass. It is **not** caught by `catch (Poco::Net::NetException&)` and must have its own arm. Without this explicit catch, a single false-positive POLL_READ would terminate the listener.
- Poco auto-retries `EINTR` inside its `do { ::recv(...) } while (rc<0 && lastError()==EINTR)`. No application-level retry is needed for that case.
- `EAGAIN` on a blocking socket surfaces as `TimeoutException` (covered above). On a non-blocking socket it would surface as `bytesRead < 0`; we keep the socket blocking, but a defensive `if (bytesRead < 0) { /* skip append */ }` matches the current code's `if (bytesRead > 0)` filter at zero cost.

**Disconnect-detection precedence around `bytesRead`:**

```cpp
if (bytesRead == 0) {
  fatal("SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.");
}
if (bytesRead > 0) {
  bufferBytesAppended(bytesRead);
}
// bytesRead < 0 (TimeoutException path or non-blocking EAGAIN): fall through to bufferParse().
```

**POLL_READ must be drained before POLL_ERROR fires fatal:**

On Linux UDS a peer close with pending bytes delivers `EPOLLIN | EPOLLHUP` together (Poco reports `POLL_READ | POLL_ERROR`). Checking POLL_ERROR first throws away the trailing bytes — often the final `END_RUN` packet. The loop body must therefore process POLL_READ first; POLL_ERROR fires fatal only when no read was performed (or when a read returned 0 and the helper already terminated us).

```cpp
if (mode & Poco::Net::PollSet::POLL_READ) {
  // ... read path above ...
}
if (mode & Poco::Net::PollSet::POLL_ERROR) {
  // Only reached if POLL_READ branch did not already exit via fatal().
  fatal("SNSLiveEventDataListener::run(): socket poll reported error.");
}
```

**Socket receive timeout: reduce from 30 s to ≤ 1 s.**

`connect()` currently sets `m_socket.setReceiveTimeout(Poco::Timespan(RECV_TIMEOUT, 0))` with `RECV_TIMEOUT = 30`. After this refactor the receive timeout is no longer the primary control flow (PollSet is), but it is still load-bearing as a backstop against false-positive `POLL_READ` events. Thirty seconds is too long for that role — it would pin the bg thread for half a minute and undo the shutdown-latency improvement that motivates the refactor.

Change `RECV_TIMEOUT` to `1` (one second). This bounds worst-case wedge time at one second while still being orders of magnitude longer than any legitimate poll-to-recv interval.

**Hello-send must be a retry loop, not a single check.**

`Poco::Net::StreamSocket::sendBytes()` is documented to return short. On a 20-byte UDS hello this never fires in practice, but treating a short return as fatal is exactly the "single attempt where a retry loop belongs" pattern. Replace:

```cpp
if (m_socket.sendBytes(helloPkt, sizeof(helloPkt)) != sizeof(helloPkt))
  fatal("...");
```

with a write-all loop inside `sendHelloPacket()`:

```cpp
int sendHelloPacket() {
  const auto *p = reinterpret_cast<const uint8_t *>(helloPkt);
  size_t remaining = sizeof(helloPkt);
  while (remaining > 0) {
    int n = m_socket.sendBytes(p, static_cast<int>(remaining));
    if (n <= 0) {
      // n==0 is genuinely fatal (peer closed); n<0 shouldn't happen on blocking.
      return static_cast<int>(sizeof(helloPkt) - remaining);  // short-write signal
    }
    p += n;
    remaining -= n;
  }
  return static_cast<int>(sizeof(helloPkt));
}
```

The caller still checks the return value and triggers the helper if it is short — but now "short" means we couldn't send everything *even with retry*, which is genuinely fatal. `Poco::Net::NetException` from inside the loop propagates to the same outer catch.

This loop also makes the existing `test_partialHelloSend_setsError` plan robust: an override that returns short on the *first* `sendBytes()` call and full on the second would succeed (proving the retry works); an override that returns short on every call would fail with the fatal helper (proving exhaustion is detected).

### 8. Receive timeout in `connect()`

`m_socket.setReceiveTimeout(Poco::Timespan(RECV_TIMEOUT, 0))` is no longer the primary control flow but is still load-bearing as a backstop against false-positive `POLL_READ` events that would otherwise block `receiveBytes()` indefinitely. **Reduce `RECV_TIMEOUT` from 30 to 1 (one second).** This bounds worst-case wedge time to one second while remaining orders of magnitude longer than any legitimate poll-to-recv interval. See §7a for the full rationale.

### 9. Destructor join deadline

The destructor uses `m_thread.join(RECV_TIMEOUT * 2 * 1000)` (60 s), sized for the old 30 s receive timeout. With PollSet at 100 ms, normal shutdown is sub-second. Tighten the join deadline to something proportionate (suggest 5 s — generous, but not 60 s) so a wedged bg thread surfaces faster.

______________________________________________________________________

## Suggested control-flow sketch

```cpp
void SNSLiveEventDataListener::run() {
  auto fatal = [this](const std::string &msg) {
    g_log.fatal() << msg << '\n';
    m_isConnected = false;
    if (!m_backgroundException)
      m_backgroundException = std::make_shared<std::runtime_error>(msg);
    throw std::runtime_error(msg);
  };

  try {
    if (!m_isConnected) {
      throw std::runtime_error("SNSLiveEventDataListener::run(): No connection to SMS server.");
    }

    // Send hello packet (unchanged construction).
    // ...
    if (m_socket.sendBytes(helloPkt, sizeof(helloPkt)) != sizeof(helloPkt)) {
      fatal("SNSLiveEventDataListener::run(): short write on client hello packet.");
    }

    Poco::Net::PollSet pollSet;
    pollSet.add(m_socket, Poco::Net::PollSet::POLL_READ);

    while (!m_stopThread.load(std::memory_order_acquire)) {
      // Back-pressure pause loop — unchanged.
      while (m_bgThreadCaughtUp.load(std::memory_order_acquire) &&
             m_pauseNetRead.load(std::memory_order_acquire) &&
             !m_stopThread.load(std::memory_order_acquire)) {
        Poco::Thread::sleep(100);
      }
      if (m_stopThread.load(std::memory_order_acquire))
        break;

      // Wait for readiness (or timeout).  100 ms.
      auto ready = pollSet.poll(Poco::Timespan(0, 100000));

      if (!ready.empty()) {
        auto it = ready.find(m_socket);
        if (it != ready.end()) {
          const int mode = it->second;
          // POLL_READ first: on Linux UDS a peer close with pending bytes
          // delivers EPOLLIN|EPOLLHUP (POLL_READ|POLL_ERROR) together; we
          // must drain the trailing bytes before reporting a fatal poll error.
          if (mode & Poco::Net::PollSet::POLL_READ) {
            unsigned int bufFillLen = bufferFillLength();
            if (bufFillLen) {
              uint8_t *bufFillAddr = bufferFillAddress();
              int bytesRead = -1;
              try {
                bytesRead = m_socket.receiveBytes(bufFillAddr, bufFillLen);
              } catch (Poco::TimeoutException &) {
                // False-positive POLL_READ (rare on epoll, not zero) or a
                // kernel quirk.  Transient — fall through and let the next
                // poll() decide.  Do NOT log per-iteration; the loop runs
                // every ~100 ms and the log would flood.
                bytesRead = -1;
              } catch (Poco::Net::NetException &e) {
                fatal(std::string("SNSLiveEventDataListener::run(): network read failed: ") + e.name());
              }
              if (bytesRead == 0) {
                fatal("SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.");
              }
              if (bytesRead > 0) {
                bufferBytesAppended(bytesRead);
              }
              // bytesRead < 0: TimeoutException path or non-blocking EAGAIN.
              // Fall through to bufferParse().
            }
          }
          if (mode & Poco::Net::PollSet::POLL_ERROR) {
            // Only reached if the POLL_READ branch did not already terminate
            // the loop via fatal(recv==0).
            fatal("SNSLiveEventDataListener::run(): socket poll reported error.");
          }
        }
      }

      // Parse runs unconditionally — required so a full parse buffer drains
      // even when the read was skipped for lack of free space.
      m_bgThreadCaughtUp.store(false, std::memory_order_release);
      std::string bufferParseLog;
      int packetsParsed = bufferParse(bufferParseLog);
      bufferParseLog.clear();
      m_bgThreadCaughtUp.store(true, std::memory_order_release);

      if (packetsParsed == 0) {
        Poco::Thread::sleep(10);
      }
    }
  } catch (ADARA::invalid_packet &e) {
    // existing handler — unchanged
  } catch (std::runtime_error &e) {
    // existing handler — unchanged.  Helper-thrown errors land here with
    // m_backgroundException already populated; the !m_backgroundException
    // guard preserves the helper's message.
  } catch (std::invalid_argument &e) {
    // existing handler — unchanged
  } catch (std::exception &e) {
    // existing handler — unchanged
  } catch (...) {
    // existing handler — unchanged
  }
}
```

______________________________________________________________________

## Error-message guidance

Use precise, grep-friendly messages:

- `SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.`
- `SNSLiveEventDataListener::run(): socket poll reported error.`
- `SNSLiveEventDataListener::run(): network read failed: <NetException name>`
- `SNSLiveEventDataListener::run(): short write on client hello packet.`

These messages are surfaced via `m_backgroundException`; they must stand on their own.

______________________________________________________________________

## Test changes (atomic with the implementation)

These are required, not optional — without them the previously-XFAIL tests will still skip and the new behavior will not be exercised.

### Remove XFAIL guards

In `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`:

- `test_LegacyConnectAndDisconnect` (≈line 233): delete the `TS_WARN("XFAIL: ...")` block and the immediately-following `return;`. The assertions already in the test body express the intended contract.
- `test_serverDisconnect_setsErrorState` (≈line 1035): same treatment.

### Replace post-scriptIndex sleeps with state polls

Both tests currently use `std::this_thread::sleep_for(std::chrono::milliseconds{200});` after waiting on the server-side `scriptIndex`. Project convention is to poll on the listener's observable state rather than blind-sleep. Replace with:

```cpp
waitFor([&] { return m_listener->listenerState() == API::ListenerState::Error; },
        std::chrono::seconds{5});
```

(or `!m_listener->isConnected()` for the variant test).

### Acceptance assertions

- `test_serverDisconnect_setsErrorState`: assert `listenerState() == Error` after the poll completes; assert `isConnected() == false`; assert that a subsequent `extractData()` / `runState()` call rethrows the stored exception.
- `test_LegacyConnectAndDisconnect`: assert `!isConnected()` after the poll completes.

______________________________________________________________________

## New tests required

The two XFAIL conversions above re-enable existing assertions, but they do not exercise every new behavior introduced by the refactor. The tests below close that gap. Each is named with its target behavior so a failure points directly at the regression.

### Unit tests — `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`

These exercise the parts of the contract that do not require a socket. They use the existing `TestableSNSListener` subclass and its `injectBackgroundException(...)` helper. A new injection helper, `setIsConnected(bool)`, must be added to `TestableSNSListener` because `m_isConnected` is currently `private` — promote it to `protected` in the production header (it is already on the boundary between the protected back-pressure state and the private connection state) so the test subclass can drive it.

1. **`test_listenerState_Error_preempts_Disconnected`**
   Inject a background exception **and** set `m_isConnected = false`. Assert `listenerState() == Error`. This pins the `Connected → Error` (no `Disconnected` flicker) invariant by proving the priority ordering in `listenerState()`.

1. **`test_listenerState_Error_preempts_ReadWait`**
   Inject a background exception while `m_pauseNetRead` is true. Assert `listenerState() == Error`. Pins the rule that a disconnect that occurs while the bg thread is in the pause-loop is still reported as `Error`, not `ReadWait`.

1. **`test_backgroundException_is_first_writer_wins`**
   Call `injectBackgroundException("first")`, then `injectBackgroundException("second")`. Assert that `runState()`/`lastTransition()`/`runStatus()` all rethrow a `std::runtime_error` whose `what()` contains `"first"` (not `"second"`). Pins the helper's `if (!m_backgroundException)` guard so a later, more generic outer-catch handler cannot clobber the helper's precise message.
   *(This requires updating `injectBackgroundException` so the second call exercises the same guarded write the helper performs, rather than unconditional reassignment. A second helper `injectBackgroundExceptionIfUnset(...)` may be clearer.)*

1. **`test_fatalHelper_message_contains_run_function_prefix`** *(if a `callFatal(const std::string&)` helper accessor is added to `TestableSNSListener`)*
   Call the helper directly with a known string. Assert it (a) throws `std::runtime_error` with the exact passed message, (b) leaves `m_backgroundException->what()` equal to that message, (c) leaves `m_isConnected == false`. Pins all three side effects of the helper in one place rather than spreading them across integration tests.
   *(If exposing the helper for direct unit testing is judged not worth a friend/subclass hook, drop this test — its coverage is reproduced indirectly by the integration tests.)*

### Integration tests — `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`

These use `MockSMSServer` and the existing `PktDisconnect{}`, `PktWaitForExtract{}` script entries. All follow the project's `waitFor(...)` polling convention — no `sleep_for(...)` after `scriptIndex()` gates.

1. **`test_serverDisconnect_messageContainsRunPrefix`**
   Build the standard geometry / beamline / NEW_RUN preamble, then `PktDisconnect{}`. After `waitFor([&]{ return m_listener->listenerState() == Error; }, 5s)`, drive `extractData()` and capture the thrown `std::runtime_error`. Assert `what()` contains the substring `"SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream"`. Pins the error-message guidance in the plan.

1. **`test_serverDisconnect_during_ReadWait_setsError`**
   Script: geometry / beamline / NEW_RUN / `PktWaitForExtract{}` (so the listener pauses) / `PktDisconnect{}`. The listener is in `ReadWait` when the server closes. Assert that without ever releasing the extract gate, `waitFor([&]{ return m_listener->listenerState() == Error; }, 5s)` succeeds within a couple of poll periods. Pins that back-pressure does **not** mask a disconnect — the bg thread must be polling, not deep-sleeping inside `receiveBytes()`.

1. **`test_serverDisconnect_before_initialization_setsError`**
   Script: `PktDisconnect{}` with no preceding packets. Listener cannot complete `initWorkspacePart2()` because no geometry/beamline ever arrives. Assert `extractData()` throws `std::runtime_error` (not `Exception::NotYet`) and that `listenerState() == Error`. Pins that a pre-init disconnect surfaces through `m_backgroundException` and is observable by foreground waits in `doExtractData()`.

1. **`test_serverDisconnect_extractData_rethrows_runtime_error`**
   Full preamble (geometry / beamline / NEW_RUN with a single banked event) then `PktDisconnect{}`. After `waitFor` on `Error`, call `extractData()`. Assert it throws `std::runtime_error` (not `Exception::NotYet`, not a plain `std::exception` with the wrong message). Pins the exception type travelling out of the foreground path so callers can `catch (std::runtime_error&)` on disconnect.

1. **`test_serverDisconnect_runState_rethrows_runtime_error`**
   Same script as above. After `waitFor` on `Error`, call `runState()`. Assert it throws `std::runtime_error` carrying the disconnect message. Pins the rethrow path used by `MonitorLiveData` and the algorithm-side caller.

1. **`test_pollLoop_continues_to_parse_when_buffer_full`** *(may be tricky to provoke deterministically; mark `if (kSkipFlaky)` if the parser buffer is large enough that filling it requires hundreds of packets)*
   Script: geometry / beamline / NEW_RUN, then a long burst of `buildBankedEventPkt(...)` entries with no `PktWaitForExtract` between them, totalling more bytes than the parser's internal buffer (`ADARA::Parser` default buffer size — currently ~1 MiB; size the burst from `bufferFillLength()` at start-of-run if needed). No extract is performed during the burst. Assert that after the burst the listener is still in `Connected` (or `ReadWait` if back-pressure kicked in), **not** stuck or `Error`, and that a subsequent `extractData()` returns a workspace with all events present. Pins the unconditional-`bufferParse()` invariant: if the new loop only parsed when bytes were read, the buffer would fill and the loop would spin without progress.

1. **`test_shutdownLatency_after_normal_connect`**
   Full preamble. Measure `auto t0 = steady_clock::now(); m_listener.reset(); auto dt = now - t0;`. Assert `dt < 1500 ms` (generous bound — the new poll timeout is 100 ms; the tightened join deadline is a couple of seconds). Pins the new sub-second shutdown contract, which depends on both the PollSet timeout and the destructor-join change.

1. **`test_disconnect_then_reconstruct_listener_does_not_leak_state`**
   Run a full disconnect cycle to `Error`, destroy the listener, construct a fresh `SNSLiveEventDataListener`, and assert its initial state is the same as a fresh-out-of-the-box listener (no static or process-global state leaked through). Pins that the new failure helper does not introduce process-scoped side effects (e.g. via a logger registration or a static PollSet).
   *(Optional — drop if the existing per-test fixture already exercises construction order well enough.)*

### Production seam and test-infrastructure changes required by these tests

Two of the failure paths cannot be deterministically exercised from outside the listener without small additions to the production surface and to `MockSMSServer`. These changes are part of the refactor, not optional.

**Production seam: `virtual int sendHelloPacket();`**

The hello packet send (`m_socket.sendBytes(helloPkt, sizeof(helloPkt))`) cannot be made short by anything the server does — on UDS the kernel always accepts a 20-byte write atomically. Either the call succeeds in full or it throws a `NetException` (a different branch).

Extract the call into a small protected virtual:

```cpp
// in SNSLiveEventDataListener.h, protected:
virtual int sendHelloPacket();  // returns bytes sent, or throws NetException
```

The default implementation does exactly what the current code does. `TestableSNSListener` overrides it to return `sizeof(helloPkt) - 1`, which lets the partial-send test below be fully deterministic.

**MockSMSServer extension: `PktAbruptClose{}`**

The existing `PktDisconnect{}` closes the server socket *after* all preceding script entries have been fully sent. To exercise the `POLL_ERROR` branch deterministically we need a close that fires with **no data pending in the client's kernel receive buffer**, so the client's poll wakes with `EPOLLHUP` alone (Poco maps this to `POLL_ERROR`). Add:

```cpp
struct PktAbruptClose {};  // closes server socket immediately; no
                           // preceding sends are drained from the queue.
```

Implementation: in `MockSMSServer::Impl`, on encountering this script entry, call `::close(client_fd)` *without* first draining any prior bytes that haven't already been flushed to the kernel. This is a few lines next to the existing `PktDisconnect` handling.

### Tests enabled by the seams above

9. **`test_partialHelloSend_setsError`** *(unit-flavored integration; uses `TestableSNSListener` + real `MockSMSServer`)*
   `TestableSNSListener` overrides `sendHelloPacket()` to return `sizeof(helloPkt) - 1`. Script the server with `PktWaitForExtract{}` (so it stays alive long enough for the assertion). After `start()`, `waitFor([&]{ return m_listener->listenerState() == Error; }, 5s)`. Assert the captured `m_backgroundException->what()` contains `"short write on client hello packet"`. Pins the partial-hello-send failure path end-to-end.

1. **`test_pollError_setsError_via_abruptClose`**
   Script: `PktAbruptClose{}` as the *only* entry. The server accepts the connection, optionally consumes the hello, and closes without sending any bytes back. Client's poll wakes with `EPOLLHUP` → Poco reports `POLL_ERROR` → helper fires with the `"socket poll reported error"` message.
   Assert `listenerState() == Error` within 5 s. Assert the captured message contains `"socket poll reported error"`. Pins the `POLL_ERROR` branch specifically (distinct from the `recv()==0` branch covered by the disconnect tests, which always have some prior data in flight).
   *(If the kernel/Poco actually delivers `POLLIN|POLLHUP` here on a particular platform, the message will instead come from the `recv()==0` branch. Decide at implementation time whether to accept either message or to switch the assertion to a state-only check. Either way, `MockSMSServer.PktAbruptClose` is the right primitive — it minimizes pending data so `POLL_ERROR` is the more likely branch.)*

### Partial-read and transient-error tests

These pin the §7a contract — the most regression-prone area of the refactor.

11. **`test_partialRead_streamReassembled` (integration)**
    Extend `MockSMSServer` to support a `PktChunked{vector<uint8_t> bytes, size_t chunkSize}` script entry that writes a single logical packet to the socket in `chunkSize`-byte chunks, with a small `usleep` between chunks (e.g. 5 ms). Script: geometry / beamline / NEW_RUN preamble, then `PktChunked{buildBankedEventPkt(...), chunkSize=8}` for a banked-event packet (~hundreds of bytes), then `PktWaitForExtract{}` and `PktDisconnect{}`. Each chunk arrives on a separate POLL_READ wakeup, so the listener must accumulate fragments and only parse when the whole packet is present. Assert the extracted workspace contains all expected events. Pins the partial-read accumulation contract in the new loop.

01. **`test_partialHelloSend_retriesAndSucceeds` (unit-flavored integration)**
    Using the `sendHelloPacket()` seam, the `TestableSNSListener` override returns a short count on the first call (e.g. half the packet) and the full remainder on the second. Drive `connect()` + `start()` against a real `MockSMSServer`. Assert that **no** background exception is set, that the listener reaches the normal `Connected` state, and that `MockSMSServer::bytesSent()` (or equivalent server-side receive counter) reflects the full hello packet. Pins the §7a retry-loop semantics.

01. **`test_partialHelloSend_exhaustionIsFatal` (unit-flavored integration)**
    Same seam, but the override returns a short count on **every** call. Assert `listenerState() == Error` within 5 s with the expected `"short write on client hello packet"` message. Pins that exhaustion of the retry loop is correctly surfaced as fatal.

01. **`test_falsePositivePollRead_doesNotKillListener` (optional, requires seam)**
    Requires exposing a `virtual SocketModeMap doPoll(PollSet&, Timespan);` seam in production (one virtual call site, low cost) so that `TestableSNSListener` can return a synthetic `POLL_READ` mode while the real socket has nothing pending. The first iteration would receive `TimeoutException` from `receiveBytes()`; subsequent iterations would resume normally. Assert that after several such spurious wakeups the listener is still `Connected` (not `Error`). Pins that `TimeoutException` is treated as transient.
    *(If the `doPoll` seam is judged too invasive, this case is covered indirectly by `test_pollLoop_continues_to_parse_when_buffer_full` and by sheer code review.)*

01. **`test_pollRead_andError_together_drainsFirst` (integration)**
    Extend `MockSMSServer` with `PktBurstThenClose{vector<vector<uint8_t>> packets}` which writes all packets back-to-back without flushing between them, then closes immediately (so the kernel delivers `EPOLLIN|EPOLLHUP` together on the client side). Script the burst to include a final `END_RUN` `RunStatusPkt`. Assert that after `waitFor(listenerState == Error)` the extracted workspace shows the `END_RUN` was received (e.g. via `lastTransition() == EndRun` history). Pins that the loop drains POLL_READ before declaring POLL_ERROR fatal.
    *(Implementation note: on some kernels the flags arrive in separate `epoll_wait` returns; the test should accept "data observed before error" rather than requiring a single combined event.)*

### What is **still** intentionally not tested

- **NetException from `sendBytes()` during the hello packet** (as opposed to a short return). Provoking ECONNRESET *between* connect and the first send requires either a kernel-level race or a custom socket; both are more brittle than the value they add. The helper's `NetException` rewrap is exercised by the read-side disconnect tests (same `NetException` catch in the loop body), so the hello-send NetException branch is covered by code-path identity, not by a direct test.
- **Real receive timeout (1 s) firing under load.** The receive timeout is a backstop against false-positive POLL_READ; provoking it would require either intentional kernel manipulation or test #14's `doPoll` seam. Covered by test #14 if the seam is added.

______________________________________________________________________

## Acceptance criteria

1. `run()` no longer blocks directly on a long receive timeout as its primary wait mechanism.
1. `Poco::Net::PollSet` is used to wait for socket readiness.
1. A peer-close (`receiveBytes() == 0`) becomes a fatal `std::runtime_error` via the helper.
1. Fatal disconnect/error paths set:
   - `m_isConnected = false`
   - `m_backgroundException` (once; first-writer-wins, matching existing handlers)
1. `listenerState()` transitions `Connected → Error` on disconnect with no intermediate `Disconnected` flicker.
1. `bufferParse()` is invoked on every loop iteration, preserving drain behavior when the parse buffer is full.
1. `m_bgThreadCaughtUp` `false/true` bracketing surrounds `bufferParse()` and only `bufferParse()`; the pause-loop short-circuit is unchanged.
1. `m_pauseNetRead` back-pressure behavior is unchanged.
1. Hello-packet short write surfaces through `m_backgroundException` (not just a log line + thread exit).
1. Both previously-XFAIL tests pass and exercise the new behavior via state polls rather than blind sleeps.
1. Destructor `m_thread.join(...)` deadline is tightened so a normal shutdown completes promptly.

______________________________________________________________________

## Critical files

- `Framework/LiveData/src/SNSLiveEventDataListener.cpp` — `run()` (the loop) and `connect()` (for the receive-timeout note).
- `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` — for the existing atomic invariants on `m_pauseNetRead`, `m_bgThreadCaughtUp`, and `m_stopThread`.
- `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` — XFAIL preambles and `sleep_for` blocks at the two test bodies named above.

______________________________________________________________________

## Verification

1. **Unit tests.** Build the LiveData target and run `SNSLiveEventDataListenerTest`. Both previously-XFAIL tests should pass.
1. **Regression sweep.** Run the full LiveData test target; in particular confirm no regression in the `PktWaitForExtract`-gated tests that rely on `m_bgThreadCaughtUp` semantics.
1. **Shutdown latency.** Run a long-lived fixture and observe destructor teardown — it should complete in under a second, not 60 s.
1. **Manual smoke (optional).** Point the listener at a stub SMS server, kill the server mid-stream, and confirm via `listenerState()` that the listener transitions to `Error` within ~100–200 ms.
