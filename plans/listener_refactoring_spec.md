# Specification: Refactor SNSLiveEventDataListener State Management

## Overview

Separate listener connection state from ADARA run status while preserving backward compatibility. Enable external control of network reading to support stand-alone `LoadLiveData` usage.

---

## 1. Problem Statement

### Current Issues

1. **Conflated State**: `runStatus()` returns both ADARA protocol state and controls network reading
2. **Hidden Side Effects**: `runStatus()` transitions state and clears `m_pauseNetRead` flag
3. **External Control Missing**: Stand-alone `LoadLiveData` cannot control network reading → deadlock
4. **Timing Dependencies**: MonitorLiveData depends on specific return values and side effect timing
5. **Single Responsibility Violation**: One method handles status query, state transitions, and network control

---

## 2. Interface Changes

### 2.1 Base Class (ILiveListener.h)

**New public interface:**

```cpp
enum class ListenerState { Disconnected, Connected, ReadWait, Error };

// Additional state flags needed to infer runStatus() return value
enum class ParserState { Idle, Parsing, Buffered, Complete };

class ILiveListener {
public:
    // ========== NEW METHODS - Answer (1): Methods for inferring runStatus() ==========
    
    /** Pure getter for ADARA protocol run state (no side effects)
     * @return Current ADARA run state: BeginRun/Running/EndRun/NoRun
     */
    virtual RunStatus runState() const = 0;
    
    /** Check if this is first call after a run transition
     * @return true if we've just transitioned to BeginRun or EndRun 
     *         and haven't processed it yet
     */
    virtual bool isFirstCallAfterTransition() const = 0;
    
    /** Check if we have deferred run details pending
     * @return true if we have a RunStatus packet waiting to be processed
     */
    virtual bool hasDeferredRunDetails() const = 0;
    
    /** Get parser buffer state to understand data availability
     * @return Current parser state
     */
    virtual ParserState getParserState() const = 0;
    
    /** Get listener connection and read state
     * @return Disconnected, Connected, ReadWait, or Error
     */
    virtual ListenerState listenerState() const = 0;
    
    // ========== NEW METHODS - Answer (2): Methods to control runStatus side effects ==========
    
    // --- Group 1: Workspace and cache management ---
    
    /** Mark workspace as needing reinitialization (external control)
     * Replicates: m_workspaceInitialized = false
     */
    virtual void setWorkspaceNeedsReinit() = 0;
    
    /** Clear instrument geometry cache
     * Replicates: m_instrumentXML.clear(); m_instrumentName.clear();
     */
    virtual void clearInstrumentCache() = 0;
    
    /** Clear device/variable name mappings
     * Replicates: m_nameMap.clear()
     */
    virtual void clearNameMap() = 0;
    
    /** Clear data start time cache
     * Replicates: m_dataStartTime.clear()
     */
    virtual void clearDataStartTime() = 0;
    
    // --- Group 2: Deferred run details management ---
    
    /** Set run details from deferred packet (external control)
     * Replicates: setRunDetails(*m_deferredRunDetailsPkt)
     * @param runNumber The run number to set
     * @param runStart The run start time to set
     */
    virtual void setRunDetails(uint32_t runNumber, const std::string &runStart) = 0;
    
    /** Clear deferred run details packet
     * Replicates: m_deferredRunDetailsPkt.reset()
     */
    virtual void clearDeferredRunDetails() = 0;
    
    // --- Group 3: Network flow control ---
    
    /** Set read-wait state (pause/resume network reading)
     * Replaces old: m_pauseNetRead = true
     * @param value true to pause reading, false to resume
     */
    virtual void setReadWait(bool value) = 0;
    
    /** Convenience method to clear read-wait
     * Replaces old: m_pauseNetRead = false
     */
    virtual void clearReadWait() = 0;
    
    // ========== EXISTING METHODS (DEPRECATED) ==========
    
    /** Backward-compatible run status (deprecated)
     * 
     * Maintains existing behavior for backward compatibility:
     * - Returns cached status before state transitions
     * - Transitions BeginRun→Running or EndRun→NoRun on FIRST call
     * - Clears all caches and initializes workspace
     * - Calls setRunDetails() for BeginRun
     * - Clears read-wait flag
     * 
     * @deprecated Use combination of new methods above
     */
    [[deprecated("Use runState(), listenerState(), and explicit state management methods")]]
    virtual RunStatus runStatus() = 0;
    
    // ... existing methods unchanged ...
};
```

### 2.2 SNSLiveEventDataListener Implementation

**New private members:**

```cpp
class SNSLiveEventDataListener : public ILiveListener {
private:
    // ========== NEW STATE VARIABLES ==========
    
    /** ADARA protocol state (DAS tells us) */
    ILiveListener::RunStatus m_adaraRunStatus{NoRun};
    
    /** Listener internal transition state (for extractData() logic) */
    ILiveListener::RunStatus m_runStatus{NoRun};
    
    /** Listener connection and read state */
    ListenerState m_listenerState{ListenerState::Disconnected};
    
    /** Read-wait flag (replaces m_pauseNetRead) */
    bool m_readWait{true};
    
    /** Connection flag */
    bool m_isConnected{false};
    
    // ========== EXISTING MEMBERS (REMOVED/REPLACED) ==========
    
    // REMOVED: ILiveListener::RunStatus m_status{RunStatus::NoRun};
    // REMOVED: bool m_pauseNetRead{false};
    // REPLACED WITH: m_adaraRunStatus, m_runStatus, m_readWait, m_listenerState
    
    // ========== EXISTING MEMBERS (UNCHANGED) ==========
    
    std::shared_ptr<std::runtime_error> m_backgroundException;
    bool m_workspaceInitialized{false};
    std::shared_ptr<ADARA::RunStatusPkt> m_deferredRunDetailsPkt;
    // ... other members unchanged ...
};
```

**New public methods:**

```cpp
public:
    // ========== NEW METHODS ==========
    
    /** Pure getter for ADARA protocol state (DAS perspective) */
    RunStatus runState() const override {
        if (m_backgroundException) throw(*m_backgroundException);
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_adaraRunStatus;
    }
    
    /** Get listener connection + read state */
    ListenerState listenerState() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_listenerState;
    }
    
    /** Set read-wait (external control of network reading) */
    void setReadWait(bool value) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readWait = value;
        
        if (value) {
            m_listenerState = ListenerState::ReadWait;
        } else if (m_isConnected) {
            m_listenerState = ListenerState::Connected;
        } else {
            m_listenerState = ListenerState::Disconnected;
        }
    }
    
    void clearReadWait() override { setReadWait(false); }
    
    // ========== DEPRECATED METHOD (BACKWARD-COMPATIBLE) ==========
    
    /** Backward-compatible runStatus() - delegates to new state machine */
    RunStatus runStatus() override {
        // Re-throw background exceptions (existing behavior)
        if (m_backgroundException) throw(*m_backgroundException);
        
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Return cached status before transition (existing behavior)
        RunStatus rv = m_runStatus;
        
        // Perform one-time transitions (existing logic)
        if (m_runStatus == BeginRun || m_runStatus == EndRun) {
            if (m_runStatus == BeginRun) {
                if (m_deferredRunDetailsPkt) {
                    setRunDetails(*m_deferredRunDetailsPkt);
                    m_deferredRunDetailsPkt.reset();
                }
                m_runStatus = Running;
            } else if (m_runStatus == EndRun) {
                // Clear instrument/geometry information (forces reload on next run)
                m_instrumentXML.clear();
                m_instrumentName.clear();
                m_dataStartTime = Types::Core::DateAndTime(); // Only at EndRun
                m_runStatus = NoRun;
            }
        }
        
        // Clear read-wait (existing behavior)
        if (m_readWait) {
            m_readWait = false;
            m_listenerState = ListenerState::Connected;
        }
        
        return rv;
    }
    
    // ========== EXISTING METHODS (MINOR UPDATES) ==========
    
    bool isConnected() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_isConnected;
    }
    
    std::shared_ptr<Workspace> extractData() override {
        if (m_backgroundException) throw(*m_backgroundException);
        
        // Wait for data with timeout (existing logic)
        int wait_count = 0;
        while (!m_workspaceInitialized && wait_count < 40) {
            Poco::Thread::sleep(250);
            wait_count++;
        }
        
        if (!m_workspaceInitialized) {
            throw Exception::NotYet("Workspace not yet initialized");
        }
        
        if (m_runStatus == NoRun && m_ignorePackets) {
            throw Exception::NotYet("Waiting for a run to start.");
        }
        
        // ... existing extractData logic unchanged ...
    }
};
```

---

## 3. State Machine Implementation

### 3.1 State Variables and Transitions

**ADARA Protocol State (`m_adaraRunStatus`):**
- Tracks what DAS is telling us
- Set only in packet handlers (`rxPacket()`)
- Pure data source for `runState()`
- No transitions in foreground methods

**Listener Transition State (`m_runStatus`):**
- Tracks listener internal logic state
- Used by `extractData()` to determine behavior
- Transitioned by `runStatus()` for backward compatibility
- Also set by packet handlers for consistency

**Listener Connection State (`m_listenerState`):**
- Tracks connection + read-wait status
- Values: Disconnected, Connected, ReadWait, Error
- Controlled by packet handlers and external API

**Read-Wait Flag (`m_readWait`):**
- Controls whether background thread reads network
- Independent of run state
- Can be set/cleared externally via `setReadWait()`

### 3.2 Packet Handler Updates

**In `run()` network loop (line 251):**

```cpp
void SNSLiveEventDataListener::run() {
    // ... existing setup ...
    
    while (!m_stopThread) {
        // Wait if read-wait flag is set
        if (m_readWait) {
            Poco::Thread::sleep(250);
            continue;
        }
        
        // Existing packet reading logic...
        rxPacket(pkt);
    }
}
```

**In `rxPacket(ADARA::RunStatusPkt)` (lines 646-737):**

```cpp
bool SNSLiveEventDataListener::rxPacket(const ADARA::RunStatusPkt &pkt) {
    if (pkt.status() == NEW_RUN) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Track ADARA protocol state
        m_adaraRunStatus = BeginRun;
        
        // Track listener transition state
        if (m_workspaceInitialized) {
            m_runStatus = BeginRun;
            m_readWait = true;  // Pause until foreground processes
        } else {
            // Workspace not ready - skip BeginRun to prevent deadlock
            m_runStatus = Running;
        }
        return true;
    }
    
    if (pkt.status() == END_RUN) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Track ADARA protocol state
        m_adaraRunStatus = EndRun;
        
        // Track listener transition state
        m_runStatus = EndRun;
        m_readWait = true;  // Pause until foreground processes
        return true;
    }
    
    // Other packet types unchanged...
}
```

### 3.3 Connection State Management

**In `connect()` method:**

```cpp
bool SNSLiveEventDataListener::connect(const Poco::Net::SocketAddress &address) {
    // ... existing connection logic ...
    
    if (successfully connected) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isConnected = true;
        m_listenerState = ListenerState::Connected;
        m_readWait = false;  // Start reading immediately
    }
    
    return success;
}
```

**In error handling:**

```cpp
// In catch blocks within run() thread
std::lock_guard<std::mutex> lock(m_mutex);
m_listenerState = ListenerState::Error;
m_backgroundException = std::make_shared<std::runtime_error>(error);
```

---

## 4. Algorithm Migration Strategy

### 4.1 LoadLiveData (Stand-Alone Usage)

**Current behavior:** Deadlock if used stand-alone with current runStatus

**New implementation:**

```cpp
void LoadLiveData::exec() {
    auto listener = getListener();
    
    // NEW: Ensure network reading is active
    listener->clearReadWait();
    
    bool dataNotYetGiven = true;
    while (dataNotYetGiven) {
        try {
            chunkWS = listener->extractData();
            dataNotYetGiven = false;
        } catch (Exception::NotYet &ex) {
            // NEW: Can diagnose read-wait state
            if (listener->listenerState() == ListenerState::ReadWait) {
                listener->clearReadWait();  // Explicit unblock
            }
            
            g_log.warning() << "Listener not ready: " << ex.what() << "\n";
            sleepAndCheckInterrupt(10);
        }
    }
    
    // ... existing processing unchanged ...
}
```

**Benefits:**
- No deadlock in stand-alone usage
- External control over network reading
- Better diagnostics via `listenerState()`

### 4.2 MonitorLiveData (Polling Usage)

**Current behavior:** Calls `runStatus()` for transition detection

**Migration path:**

**Phase 1 - No changes (backward compatible):**

```cpp
void MonitorLiveData::exec() {
    auto listener = getListener();
    
    // ... inside polling loop ...
    
    // No changes needed - works exactly as before
    ILiveListener::RunStatus status = listener->runStatus();
    
    if (status == BeginRun || status == EndRun) {
        // Apply transition behavior
    }
}
```

**Phase 2 - Optional optimization (future):**

```cpp
void MonitorLiveData::exec() {
    auto listener = getListener();
    
    // Could optimize by querying pure state first
    ILiveListener::RunStatus status = listener->runState();
    
    if (status == BeginRun || status == EndRun) {
        // Only call runStatus() when transitions detected
        // to maintain backward-compatible side effects
        status = listener->runStatus();
    }
}
```

### 4.3 StartLiveData (Lifecycle Management)

**No changes required** - uses base class functionality

---

## 5. Unit Test Requirements

### 5.1 Core State Machine Tests

**Test file:** `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`

```cpp
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {
public:
    void test_initial_state() {
        SNSLiveEventDataListener listener;
        TS_ASSERT_EQUALS(listener.runState(), NoRun);
        TS_ASSERT_EQUALS(listener.listenerState(), Disconnected);
        TS_ASSERT_EQUALS(listener.runStatus(), NoRun);  // Deprecated but functional
    }
    
    void test_adara_state_tracking() {
        // Simulate NEW_RUN packet arrival
        listener.handleNewRunPacket();
        TS_ASSERT_EQUALS(listener.runState(), BeginRun);  // ADARA state
        TS_ASSERT_EQUALS(listener.runStatus(), BeginRun); // Legacy returns same
        
        // After runStatus() call, transitions occur internally
        listener.runStatus();
        TS_ASSERT_EQUALS(listener.runState(), BeginRun);  // ADARA unchanged
        TS_ASSERT_EQUALS(listener.runStatus(), Running);    // Legacy transitions
    }
    
    void test_run_status_side_effects() {
        // Test that runStatus() transitions state exactly once
        listener.handleNewRunPacket();
        
        RunStatus s1 = listener.runStatus();
        TS_ASSERT_EQUALS(s1, BeginRun);
        
        RunStatus s2 = listener.runStatus();
        TS_ASSERT_EQUALS(s2, Running);  // Has transitioned
        
        // Verify read-wait cleared
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);
    }
    
    void test_run_state_no_side_effects() {
        // runState() is pure - no transitions
        listener.handleNewRunPacket();
        
        RunStatus s1 = listener.runState();
        RunStatus s2 = listener.runState();
        TS_ASSERT_EQUALS(s1, s2);  // Pure getter
        
        // Verify no transitions occurred
        TS_ASSERT_EQUALS(listener.runState(), BeginRun);
    }
    
    void test_read_wait_external_control() {
        listener.setReadWait(true);
        TS_ASSERT_EQUALS(listener.listenerState(), ReadWait);
        
        listener.clearReadWait();
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);
    }
    
    void test_exception_propagation() {
        // Simulate background exception
        listener.injectBackgroundException("Network error");
        
        TS_ASSERT_THROWS(listener.runState(), std::runtime_error);
        TS_ASSERT_THROWS(listener.runStatus(), std::runtime_error);
        TS_ASSERT_THROWS(listener.extractData(), std::runtime_error);
    }
};
```

### 5.2 Algorithm Integration Tests

**Test file:** `Framework/LiveData/test/LoadLiveDataTest.h`

```cpp
void test_standalone_no_deadlock() {
    // Test that stand-alone LoadLiveData doesn't deadlock
    LoadLiveData alg;
    alg.setRethrows(true);
    
    // Configure for SNS listener
    alg.setProperty("Instrument", "SNS_Live");
    alg.setProperty("Connection", "localhost:31415");
    
    // Should not hang
    TS_ASSERT_THROWS_NOTHING(alg.execute());
    TS_ASSERT(alg.isExecuted());
}
```

**Test file:** `Framework/LiveData/test/MonitorLiveDataTest.h`

```cpp
void test_backward_compatible_behavior() {
    // Verify that existing MonitorLiveData usage patterns still work
    MonitorLiveData alg;
    alg.setRethrows(true);
    
    // Setup listener and execute
    alg.setProperty("Instrument", "SNS_Live");
    alg.execute();
    
    // Verify run transitions detected correctly
    // (Mock listener should produce BeginRun/EndRun sequence)
    TS_ASSERT_EQUALS(alg.getPropertyValue("RunTransitionBehavior"), "Rename");
}
```

### 5.3 Concurrency Tests

```cpp
void test_concurrent_state_access() {
    SNSLiveEventDataListener listener;
    
    // Background thread simulating packet arrival
    std::thread bg([&listener]() {
        for (int i = 0; i < 100; ++i) {
            listener.handleRandomPacket();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // Foreground thread querying state
    std::thread fg([&listener]() {
        for (int i = 0; i < 100; ++i) {
            RunStatus s1 = listener.runState();
            ListenerState ls = listener.listenerState();
            RunStatus s2 = listener.runStatus();
            
            // Verify no crashes or data races
            TS_ASSERT(s1 == NoRun || s1 == BeginRun || s1 == Running || s1 == EndRun);
            TS_ASSERT(ls == Disconnected || ls == Connected || ls == ReadWait || ls == Error);
        }
    });
    
    bg.join();
    fg.join();
}
```

---

## 6. Implementation Plan

### Phase 1: Interface Definition (No Breaking Changes)
- Add new methods to `ILiveListener` base class
- Add new private members to `SNSLiveEventDataListener`
- Mark `runStatus()` as deprecated (but maintain behavior)

### Phase 2: State Machine Refactoring
- Separate `m_status` into `m_adaraRunStatus` and `m_runStatus`
- Replace `m_pauseNetRead` with `m_readWait` and `m_listenerState`
- Update packet handlers to set both state variables
- Implement `runState()`, `listenerState()`, `setReadWait()`, `clearReadWait()`
- Re-implement `runStatus()` using new state variables

### Phase 3: Testing & Validation
- Write unit tests for new methods
- Verify existing behavior unchanged (test suite passes)
- Add integration tests for stand-alone usage
- Test concurrent access patterns

### Phase 4: Optional Algorithm Updates (Future)
- Update `LoadLiveData` to use `clearReadWait()` (prevents deadlock)
- Optionally optimize `MonitorLiveData` to use `runState()` for polling
- Update documentation with migration guide

---

## 7. Backward Compatibility Guarantee

**Existing behavior will be preserved exactly:**

- `runStatus()` return values unchanged
- `runStatus()` side effects unchanged (state transitions, read-wait clearing)
- `extractData()` behavior unchanged
- Exception propagation unchanged
- MonitorLiveData, LoadLiveData, StartLiveData algorithms work identically

**New API is additive only** - no existing interfaces removed or modified.

---

## 8. Future Deprecation Path

After migration and validation:

1. Update algorithms to use new methods
2. Mark old API with stronger deprecation warnings
3. Eventually remove `runStatus()` in next major version
4. Keep pure getters (`runState()`, `listenerState()`) as permanent API

---

## 9. Critical Implementation Notes

### Mutex Synchronization

All state variable access must be protected by `m_mutex`:
- `m_adaraRunStatus`
- `m_runStatus`
- `m_listenerState`
- `m_readWait`
- `m_isConnected`

Exception: background exception pointer is thread-safe via shared_ptr

### Initialization Order

Read-wait starts as `false` (not paused) to match existing behavior where the background thread begins reading immediately. The read-wait flag is only set to `true` when BeginRun or EndRun packets arrive, triggering a pause until foreground processing completes.

### Exception Handling

All new methods must check `m_backgroundException` first for consistency with existing behavior pattern.

---

## 10. Files to Modify

### Core Implementation
1. `Framework/API/inc/MantidAPI/ILiveListener.h` - Add new virtual methods
2. `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` - Add new members
3. `Framework/LiveData/src/SNSLiveEventDataListener.cpp` - Implement new methods

### Algorithm Updates (Future)
4. `Framework/LiveData/src/LoadLiveData.cpp` - Add `clearReadWait()` call
5. `Framework/LiveData/src/MonitorLiveData.cpp` - Option to optimize polling

### Tests
6. `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` - New unit tests
7. `Framework/LiveData/test/LoadLiveDataTest.h` - Integration tests
8. `Framework/LiveData/test/MonitorLiveDataTest.h` - Backward compatibility tests

### Documentation
9. `docs/source/algorithms/LoadLiveData-v1.rst` - Update with clearReadWait usage
10. `dev-docs/source/Standards/ILiveListenerInterface.rst` - Document new API

---

**Version:** 1.0
**Date:** 2024
**Owner:** Mantid Development Team
