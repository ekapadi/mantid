# SNSLiveEventDataListener `run()` Refactor Plan

## Goal

Refactor `SNSLiveEventDataListener::run()` to:

1. Replace the current raw blocking socket-read loop with a `Poco::Net::PollSet`-based loop.
2. Treat unexpected server disconnects as fatal errors.
3. Propagate disconnects and socket failures through the existing `m_backgroundException` mechanism using `std::runtime_error`.
4. Preserve all existing parser, run-transition, and back-pressure semantics.

This change is intentionally limited to the `run()` path and closely related shutdown behavior.

---

## Current problems

### 1. Orderly server disconnect is not handled
Today, if `m_socket.receiveBytes()` returns `0`, the code does not treat that as a disconnect. It does not set `m_isConnected = false`, does not populate `m_backgroundException`, and does not terminate the thread. That can leave the listener appearing connected when the peer has already closed the socket.

### 2. Read loop is timeout-driven instead of event-driven
The current implementation blocks in `receiveBytes()` and relies on socket receive timeout behavior to periodically regain control. This makes shutdown and error handling less explicit than they should be.

### 3. Disconnect/error behavior is inconsistent
Thrown network exceptions already become fatal background-thread failures, but orderly peer shutdown does not. The refactor should make all unexpected socket termination paths converge on the same failure model.

---

## Desired behavior

### Runtime behavior
- Use `Poco::Net::PollSet` to wait for socket readiness with a short timeout.
- Keep the existing `m_pauseNetRead` and `m_bgThreadCaughtUp` semantics.
- Only call `receiveBytes()` when the socket is reported readable.
- If `receiveBytes()` returns `0`, treat that as fatal server disconnect.
- If poll/read reports a socket error or throws a network exception, treat that as fatal.
- Fatal socket failures must:
  - set `m_isConnected = false`
  - store a `std::runtime_error` in `m_backgroundException` if not already set
  - terminate the background thread cleanly

### Public-state behavior
This should naturally preserve current external semantics:
- `listenerState()` returns `Error` once `m_backgroundException` is set.
- `runState()` and `lastTransition()` continue to rethrow `m_backgroundException`.
- `doExtractData()` continues to surface the stored background exception during startup waits.

---

## Non-goals

- No reconnect or retry logic.
- No `SocketReactor` conversion.
- No redesign of run-transition handling.
- No changes to packet parsing behavior beyond how bytes arrive at `bufferParse()`.

---

## Required code changes

### 1. Add `PollSet` include
Add the POCO header needed for polling.

Suggested include:
- `#include <Poco/Net/PollSet.h>`

### 2. Introduce a small local failure helper in `run()`
Within `run()` (or as a small private helper if preferred), centralize fatal socket failure handling.

Suggested responsibilities:
- accept an error message string
- log it
- set `m_isConnected = false`
- if `m_backgroundException` is empty, store `std::make_shared<std::runtime_error>(msg)`
- stop the loop, preferably by throwing `std::runtime_error(msg)`

This avoids duplicating the same failure-state logic for:
- poll errors
- `receiveBytes() == 0`
- `Poco::Net::NetException`
- partial hello-send failure (optional but recommended for consistency)

### 3. Replace raw receive wait with `PollSet`
Inside `run()`:

- create a `Poco::Net::PollSet`
- add `m_socket` for read readiness
- use a short poll timeout (e.g. 100 ms or 250 ms)

The loop should:
1. honor `m_stopThread`
2. honor the existing `m_pauseNetRead` gate exactly as today
3. call `pollSet.poll(timeout)`
4. if timeout/no events, continue
5. if `m_socket` is readable, call `receiveBytes()`
6. if the socket is in an error state, fail immediately

Important: keep the existing `m_bgThreadCaughtUp = false/true` bracketing around `bufferParse()` unchanged in meaning.

### 4. Handle orderly disconnect explicitly
After `receiveBytes()`:

- if `bytesRead == 0`:
  - create a message like:
    - `"SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream."`
  - propagate as fatal `std::runtime_error`
  - do **not** continue looping

### 5. Continue to parse exactly as before
If `bytesRead > 0`:
- call `bufferBytesAppended(bytesRead)`
- set `m_bgThreadCaughtUp = false` before `bufferParse()`
- call `bufferParse()`
- set `m_bgThreadCaughtUp = true` after parse completes
- preserve the current small sleep when `packetsParsed == 0`

### 6. Keep existing back-pressure semantics
Do not change the meaning of:
- `m_pauseNetRead`
- `m_bgThreadCaughtUp`
- the pause loop before reading
- the interaction with `rxPacket(RunStatusPkt)`
- the foreground snapshot safety assumptions

This refactor is only changing how socket readiness is awaited and how disconnects are surfaced.

### 7. Tighten shutdown behavior if practical
Because the background thread may now be blocked in `PollSet::poll()` rather than `receiveBytes()`, shutdown latency should improve automatically due to the short poll timeout.

If safe within current ownership/lifetime rules, consider also closing or shutting down the socket in the destructor after setting `m_stopThread = true` to accelerate thread exit further. This is optional for the first pass and should only be done if it does not create races with the background thread's use of the socket.

---

## Suggested control-flow sketch

```cpp
void SNSLiveEventDataListener::run() {
  try {
    if (!m_isConnected) {
      throw std::runtime_error("SNSLiveEventDataListener::run(): No connection to SMS server.");
    }

    // Send hello packet as today.

    Poco::Net::PollSet pollSet;
    pollSet.add(m_socket, Poco::Net::PollSet::POLL_READ);

    while (!m_stopThread.load(std::memory_order_acquire)) {
      while (m_bgThreadCaughtUp.load(std::memory_order_acquire) &&
             m_pauseNetRead.load(std::memory_order_acquire) &&
             !m_stopThread.load(std::memory_order_acquire)) {
        Poco::Thread::sleep(100);
      }

      if (m_stopThread.load(std::memory_order_acquire)) {
        break;
      }

      auto ready = pollSet.poll(Poco::Timespan(0, 100000)); // example: 100 ms
      if (ready.empty()) {
        continue;
      }

      auto it = ready.find(m_socket);
      if (it == ready.end()) {
        continue;
      }

      const int mode = it->second;
      if (mode & Poco::Net::PollSet::POLL_ERROR) {
        throw std::runtime_error("SNSLiveEventDataListener::run(): socket poll reported error.");
      }

      if (mode & Poco::Net::PollSet::POLL_READ) {
        unsigned int bufFillLen = bufferFillLength();
        if (!bufFillLen) {
          continue;
        }

        uint8_t *bufFillAddr = bufferFillAddress();
        int bytesRead = m_socket.receiveBytes(bufFillAddr, bufFillLen);

        if (bytesRead == 0) {
          throw std::runtime_error(
            "SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.");
        }

        if (bytesRead > 0) {
          bufferBytesAppended(bytesRead);
          m_bgThreadCaughtUp.store(false, std::memory_order_release);
          std::string bufferParseLog;
          int packetsParsed = bufferParse(bufferParseLog);
          bufferParseLog.clear();
          m_bgThreadCaughtUp.store(true, std::memory_order_release);

          if (packetsParsed == 0) {
            Poco::Thread::sleep(10);
          }
        }
      }
    }
  } catch (...) {
    // preserve the existing exception-to-m_backgroundException behavior,
    // ideally with disconnect paths now flowing through std::runtime_error
  }
}
```

Note: the exact `PollSet` event constants and return types should match the POCO version already in use by the project.

---

## Error-message guidance

Use precise, grep-friendly messages. Suggested messages:

- `SNSLiveEventDataListener::run(): server disconnected while reading ADARA stream.`
- `SNSLiveEventDataListener::run(): socket poll reported error.`
- `SNSLiveEventDataListener::run(): network read failed: <detail>`

These messages will likely be surfaced later via `m_backgroundException`, so they should stand on their own.

---

## Acceptance criteria

1. `run()` no longer blocks directly on a long receive timeout as its primary wait mechanism.
2. `Poco::Net::PollSet` is used to wait for socket readiness.
3. A peer-close (`receiveBytes() == 0`) becomes a fatal `std::runtime_error`.
4. Fatal disconnect/error paths set:
   - `m_isConnected = false`
   - `m_backgroundException` (once)
5. Existing parse sequencing and `m_bgThreadCaughtUp` behavior remain intact.
6. Existing `m_pauseNetRead` back-pressure behavior remains intact.
7. Foreground callers observe the failure through the existing exception/state pathways without any reconnect attempt.

---

## Suggested tests

### Unit/integration scenarios
1. **Orderly disconnect**
   - Simulate peer close after successful connection.
   - Verify background thread exits.
   - Verify `m_isConnected == false`.
   - Verify `m_backgroundException` is set.
   - Verify foreground access surfaces the stored runtime error.

2. **Socket exception during read**
   - Simulate `Poco::Net::NetException` from read path.
   - Verify same fatal-state behavior as orderly disconnect.

3. **No regression in normal packet flow**
   - Existing happy-path parsing still works.
   - `bufferParse()` bracketing with `m_bgThreadCaughtUp` remains correct.

4. **Back-pressure compatibility**
   - When `m_pauseNetRead` is set, verify the background thread stops consuming additional packets until foreground processing clears the pause.

5. **Shutdown**
   - Thread exits promptly after `m_stopThread = true`.
   - No new spin loop on disconnect or empty-read path.

---

## Implementation notes for the coding agent

- Prefer minimal, local changes.
- Do not alter packet semantics or transition semantics.
- Keep the existing exception handlers unless a small cleanup clearly improves consistency.
- If adding a helper, keep it private and tightly scoped to failure reporting.
- Avoid introducing reconnect logic, exponential backoff, or state-machine expansion in this pass.
