# Specification: Refactor SNSLiveEventDataListener State Management
# Specification: Refactor SNSLiveEventDataListener State Management

## Overview

Separate listener connection state from ADARA run status while preserving backward compatibility. Enable external control of network reading to support stand-alone `LoadLiveData` usage and allow fine-grained control over all state transitions currently embedded in `runStatus()`.

---

## 1. Problem Statement

### Current Issues

1. **Conflated State**: `runStatus()` returns both ADARA protocol state and controls network reading
2. **Hidden Side Effects**: `runStatus()` transitions state, clears caches, reinitializes workspace, and clears `m_pauseNetRead` flag
3. **No External Control**: Stand-alone `LoadLiveData` cannot control network reading → deadlock
4. **Timing Dependencies**: `MonitorLiveData` depends on specific return values and side effect timing
5. **Single Responsibility Violation**: One method handles status query, state transitions, workspace management, and network control
6. **Incomplete State Decomposition**: Current spec lacks methods to fully replicate all `runStatus()` side effects externally

---

## 2. Interface Changes

### 2.1 Base Class (ILiveListener.h)

**New public interface:**

```cpp
enum class ListenerState { Disconnected, Connected, ReadWait, Error };

class ILiveListener {
public:
    // ========== STATE QUERY METHODS - Pure getters (no side effects) ==========
    
    /** Get the ADARA protocol run state (pure getter)
     * @return Current run state from DAS: BeginRun/Running/EndRun/NoRun
     */
    virtual RunStatus runState() const = 0;
    
    /** Get the listener's internal transition state
     * @return Transition state used by extractData() logic
     */
    virtual RunStatus listenerRunStatus() const = 0;
    
    /** Check if listener is in a transition state (BeginRun or EndRun)
     * @return true if runStatus would perform side effects
     */
    virtual bool isInTransitionState() const = 0;
    
    /** Get listener connection and read state
     * @return Current connection state
     */
    virtual ListenerState listenerState() const = 0;
    
    /** Check if we have deferred run details pending
     * @return true if deferred packet exists
     */
    virtual bool hasDeferredRunDetails() const = 0;
    
    // ========== NETWORK FLOW CONTROL ==========
    
    /** Set read-wait state (pause/resume network reading)
     * Replaces old: m_pauseNetRead = true/false
     * @param value true to pause reading, false to resume
     */
    virtual void setReadWait(bool value) = 0;
    
    /** Convenience method to clear read-wait
     * Replaces old: m_pauseNetRead = false
     */
    virtual void clearReadWait() = 0;
    
    // ========== CACHE MANAGEMENT (Granular Control) ==========
    
    /** Mark workspace as needing reinitialization
     * Sets: m_workspaceInitialized = false
     */
    virtual void markWorkspaceNeedsReinit() = 0;
    
    /** Reinitialize workspace (calls initWorkspacePart1())
     * Creates new empty EventWorkspace with required properties
     * Required after run transitions before extractData() can succeed
     */
    virtual void reinitializeWorkspace() = 0;
    
    /** Clear instrument geometry cache
     * Sets: m_instrumentXML.clear(), m_instrumentName.clear()
     */
    virtual void clearInstrumentCache() = 0;
    
    /** Clear data start time cache
     * Sets: m_dataStartTime = Types::Core::DateAndTime()
     * Note: Only call this at EndRun, not BeginRun
     */
    virtual void clearDataStartTime() = 0;
    
    /** Clear device/variable name mappings
     * Sets: m_nameMap.clear()
     */
    virtual void clearNameMap() = 0;
    
    /** Clear required logs list
     * Sets: m_requiredLogs.clear()
     */
    virtual void clearRequiredLogs() = 0;
    
    /** Clear monitor logs list
     * Sets: m_monitorLogs.clear()
     */
    virtual void clearMonitorLogs() = 0;
    
    /** Clear variable packet cache
     * Sets: m_variableMap.clear()
     */
    virtual void clearVariableCache() = 0;
    
    // ========== RUN DETAILS MANAGEMENT ==========
    
    /** Set run details from deferred packet or external configuration
     * Replicates: setRunDetails() with packet data
     * @param runNumber The run number to set
     * @param runStart ISO8601 string of run start time
     */
    virtual void setRunDetails(uint32_t runNumber, const std::string &runStart) = 0;
    
    /** Clear deferred run details packet
     * Sets: m_deferredRunDetailsPkt.reset()
     */
    virtual void clearDeferredRunDetails() = 0;
    
    // ========== STATE TRANSITION CONTROL ==========
    
    /** Transition the listener's run status state
     * Replaces internal transitions in runStatus()
     * @param newStatus New status to set (Running, NoRun, etc)
     */
    virtual void transitionRunStatus(RunStatus newStatus) = 0;
    
    // ========== HIGH-LEVEL FACADE METHODS (Recommended) ==========
    
    /** Execute all BeginRun transition side effects
     * Facade that calls in correct order:
     * - markWorkspaceNeedsReinit()
     * - clearInstrumentCache()
     * - clearNameMap()
     * - clearRequiredLogs()
     * - clearMonitorLogs()
     * - reinitializeWorkspace()
     * - setRunDetails() [if hasDeferredRunDetails()]
     * - clearDeferredRunDetails()
     * - transitionRunStatus(Running)
     * - clearReadWait()
     */
    virtual void onBeginRunTransition() = 0;
    
    /** Execute all EndRun transition side effects
     * Facade that calls in correct order:
     * - markWorkspaceNeedsReinit()
     * - clearInstrumentCache()
     * - clearDataStartTime()
     * - clearNameMap()
     * - clearRequiredLogs()
     * - clearMonitorLogs()
     * - reinitializeWorkspace()
     * - transitionRunStatus(NoRun)
     * - clearReadWait()
     */
    virtual void onEndRunTransition() = 0;
    
    // ========== EXISTING DEPRECATED METHOD (Backward Compatible) ==========
    
    /** Backward-compatible run status (deprecated)
     * 
     * Maintains existing behavior exactly:
     * - Returns cached status before state transitions
     * - Transitions BeginRun→Running or EndRun→NoRun on FIRST call
     * - Clears all caches via reinitializeWorkspace()
     * - Calls setRunDetails() for BeginRun
     * - Clears read-wait flag
     * 
     * Implementation delegates to granular methods above
     * 
     * @deprecated Use combination of new state query and transition methods
     */
    [[deprecated("Use runState(), listenerRunStatus(), onBeginRunTransition(), onEndRunTransition()")]]
    virtual RunStatus runStatus() = 0;
    
    // ========== EXISTING UNCHANGED METHODS ==========
    virtual std::string name() const = 0;
    virtual bool supportsHistory() const = 0;
    virtual bool buffersEvents() const = 0;
    virtual bool connect(const Poco::Net::SocketAddress &address) = 0;
    virtual void start(const Types::Core::DateAndTime &startTime) = 0;
    virtual std::shared_ptr<Workspace> extractData() = 0;
    virtual bool isConnected() = 0;
    virtual bool dataReset() = 0;
    virtual int runNumber() const = 0;
    virtual void setSpectra(const std::vector<specnum_t> &specList) = 0;
    virtual void setAlgorithm(const IAlgorithm &callingAlgorithm) = 0;
};
```

### 2.2 SNSLiveEventDataListener Implementation

**New private members:**

```cpp
class SNSLiveEventDataListener : public API::LiveListener {
private:
    // ========== NEW STATE VARIABLES ==========
    
    /** ADARA protocol state (from DAS RunStatus packets) */
    ILiveListener::RunStatus m_adaraRunStatus{NoRun};
    
    /** Listener internal transition state (for extractData() logic) */
    ILiveListener::RunStatus m_runStatus{NoRun};
    
    /** Listener connection and read state */
    ListenerState m_listenerState{ListenerState::Disconnected};
    
    /** Read-wait flag (replaces m_pauseNetRead) */
    bool m_readWait{false};
    
    /** Connection flag (replicates m_isConnected logic) */
    bool m_isConnected{false};
    
    // ========== EXISTING MEMBERS (UNCHANGED) ==========
    std::shared_ptr<std::runtime_error> m_backgroundException;
    bool m_workspaceInitialized{false};
    std::string m_instrumentXML;
    std::string m_instrumentName;
    Types::Core::DateAndTime m_dataStartTime;
    NameMapType m_nameMap;
    VariableMapType m_variableMap;
    std::vector<std::string> m_requiredLogs;
    std::vector<std::string> m_monitorLogs;
    std::shared_ptr<ADARA::RunStatusPkt> m_deferredRunDetailsPkt;
    int m_runNumber{0};
    bool m_runPaused{false};
    bool m_keepPausedEvents{false};
    bool m_ignorePackets{false};
    bool m_filterUntilRunStart{false};
    
    // ========== EXISTING MEMBERS (PROTECTED BY MUTEX) ==========
    DataObjects::EventWorkspace_sptr m_eventBuffer;
    std::mutex m_mutex;
    Poco::Thread m_thread;
    bool m_stopThread{false};
    
    // ========== DEPRECATED MEMBERS (REMOVED) ==========
    // REMOVED: ILiveListener::RunStatus m_status{RunStatus::NoRun};
    // REMOVED: bool m_pauseNetRead{false};
};
```

**New public methods - Implementation skeleton:**

```cpp
// ========== STATE QUERY METHODS ==========

RunStatus SNSLiveEventDataListener::runState() const {
    if (m_backgroundException) throw(*m_backgroundException);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_adaraRunStatus;
}

RunStatus SNSLiveEventDataListener::listenerRunStatus() const {
    if (m_backgroundException) throw(*m_backgroundException);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_runStatus;
}

bool SNSLiveEventDataListener::isInTransitionState() const {
    if (m_backgroundException) throw(*m_backgroundException);
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_runStatus == BeginRun || m_runStatus == EndRun);
}

ListenerState SNSLiveEventDataListener::listenerState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_listenerState;
}

bool SNSLiveEventDataListener::hasDeferredRunDetails() const {
    if (m_backgroundException) throw(*m_backgroundException);
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_deferredRunDetailsPkt != nullptr);
}

// ========== NETWORK FLOW CONTROL ==========

void SNSLiveEventDataListener::setReadWait(bool value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readWait = value;
    
    if (value && m_isConnected) {
        m_listenerState = ListenerState::ReadWait;
    } else if (!value && m_isConnected) {
        m_listenerState = ListenerState::Connected;
    } else if (!m_isConnected) {
        m_listenerState = ListenerState::Disconnected;
    }
}

void SNSLiveEventDataListener::clearReadWait() {
    setReadWait(false);
}

// ========== CACHE MANAGEMENT ==========

void SNSLiveEventDataListener::markWorkspaceNeedsReinit() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_workspaceInitialized = false;
}

void SNSLiveEventDataListener::reinitializeWorkspace() {
    std::lock_guard<std::mutex> lock(m_mutex);
    initWorkspacePart1();
}

void SNSLiveEventDataListener::clearInstrumentCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_instrumentXML.clear();
    m_instrumentName.clear();
}

void SNSLiveEventDataListener::clearDataStartTime() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dataStartTime = Types::Core::DateAndTime();
}

void SNSLiveEventDataListener::clearNameMap() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nameMap.clear();
}

void SNSLiveEventDataListener::clearRequiredLogs() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requiredLogs.clear();
}

void SNSLiveEventDataListener::clearMonitorLogs() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_monitorLogs.clear();
}

void SNSLiveEventDataListener::clearVariableCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_variableMap.clear();
}

// ========== RUN DETAILS MANAGEMENT ==========

void SNSLiveEventDataListener::setRunDetails(uint32_t runNumber, const std::string &runStart) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_runNumber = static_cast<int>(runNumber);
    
    m_eventBuffer->mutableRun().addProperty("run_number", 
        Mantid::Kernel::Strings::toString<int>(m_runNumber), true);
    m_eventBuffer->mutableRun().addProperty("run_start", runStart, true);
}

void SNSLiveEventDataListener::clearDeferredRunDetails() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_deferredRunDetailsPkt.reset();
}

// ========== STATE TRANSITION CONTROL ==========

void SNSLiveEventDataListener::transitionRunStatus(RunStatus newStatus) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_runStatus = newStatus;
}

// ========== HIGH-LEVEL FACADE METHODS ==========

void SNSLiveEventDataListener::onBeginRunTransition() {
    // Execute in correct order to replicate runStatus() BeginRun behavior
    std::lock_guard<std::mutex> lock(m_mutex);
    
    markWorkspaceNeedsReinit();
    clearInstrumentCache();
    clearNameMap();
    clearRequiredLogs();
    clearMonitorLogs();
    reinitializeWorkspace();
    
    if (hasDeferredRunDetails()) {
        // Extract values from deferred packet
        auto pkt = m_deferredRunDetailsPkt;
        setRunDetails(pkt->runNumber(), pkt->runStart());
        clearDeferredRunDetails();
    }
    
    transitionRunStatus(Running);
    clearReadWait();
}

void SNSLiveEventDataListener::onEndRunTransition() {
    // Execute in correct order to replicate runStatus() EndRun behavior
    std::lock_guard<std::mutex> lock(m_mutex);
    
    markWorkspaceNeedsReinit();
    clearInstrumentCache();
    clearDataStartTime();
    clearNameMap();
    clearRequiredLogs();
    clearMonitorLogs();
    reinitializeWorkspace();
    
    transitionRunStatus(NoRun);
    clearReadWait();
}

// ========== DEPRECATED METHOD (BACKWARD COMPATIBLE) ==========

ILiveListener::RunStatus SNSLiveEventDataListener::runStatus() {
    if (m_backgroundException) {
        throw(*m_backgroundException);
    }
    
    std::lock_guard<std::mutex> scopedLock(m_mutex);
    
    // Cache return value (existing behavior)
    ILiveListener::RunStatus rv = m_runStatus;
    
    // Execute transition behavior only once (existing behavior)
    if (m_runStatus == BeginRun || m_runStatus == EndRun) {
        // Execute all side effects
        if (m_runStatus == BeginRun) {
            onBeginRunTransition();
        } else if (m_runStatus == EndRun) {
            onEndRunTransition();
        }
        
        // State transitions handled by facade methods above
        // m_runStatus is now Running or NoRun
    } else {
        // Not in transition, just clear read-wait
        clearReadWait();
    }
    
    return rv;
}
```

---

## 3. State Machine Implementation

### 3.1 State Variables and Relationships

```
ADARA Protocol State (m_adaraRunStatus)
   │
   ├── Set by: rxPacket(RunStatusPkt) - NEW_RUN, END_RUN, STATE
   ├── Read by: runState() 
   └── No transitions in foreground code

Listener Transition State (m_runStatus)
   │
   ├── Set by: rxPacket() AND transitionRunStatus()
   ├── Read by: extractData(), listenerRunStatus()
   ├── Transitions: BeginRun→Running, EndRun→NoRun (in onBegin/EndRunTransition)
   └── Used for: extractData() logic decisions

Listener State (m_listenerState)
   │
   ├── Set by: connect(), setReadWait(), error handlers
   ├── Values: Disconnected → Connected → ReadWait → Connected
   └── Read by: listenerState()

Read-Wait Flag (m_readWait)
   │
   ├── Controls: Background thread network reading loop
   ├── Set by: rxPacket() at run transitions
   ├── Cleared by: clearReadWait(), onBegin/EndRunTransition()
   └── Independent of: Run state (can be paused in NoRun state)
```

### 3.2 Packet Handler Updates

**In `run()` main loop:**

```cpp
void SNSLiveEventDataListener::run() {
    // ... setup code ...
    
    while (!m_stopThread) {
        // Check read-wait flag (replaces old m_pauseNetRead check)
        if (m_readWait) {
            Poco::Thread::sleep(100);  // 100ms matches current behavior
            continue;
        }
        
        // ... existing socket read and packet parsing logic ...
        rxPacket(pkt);
    }
}
```

**In `rxPacket(ADARA::RunStatusPkt)` (lines 636-737):**

```cpp
bool SNSLiveEventDataListener::rxPacket(const ADARA::RunStatusPkt &pkt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (pkt.status() == ADARA::RunStatus::NEW_RUN) {
        // Track ADARA protocol state
        m_adaraRunStatus = BeginRun;
        
        // Track listener transition state
        if (m_workspaceInitialized) {
            m_runStatus = BeginRun;
            m_readWait = true;  // Pause until foreground processes
        } else {
            // Workspace not ready - skip BeginRun to prevent deadlock
            // See original comment lines 651-678 for detailed explanation
            m_runStatus = Running;
        }
        
        // Save packet for deferred run details setting
        if (pkt.runNumber() != 0 && m_workspaceInitialized) {
            m_deferredRunDetailsPkt = std::make_shared<ADARA::RunStatusPkt>(pkt);
        }
        
        m_listenerState = ListenerState::ReadWait;
        return true;  // Signal parser to pause
        
    } else if (pkt.status() == ADARA::RunStatus::END_RUN) {
        // Track ADARA protocol state
        m_adaraRunStatus = EndRun;
        
        // Track listener transition state
        m_runStatus = EndRun;
        m_readWait = true;  // Pause until foreground processes
        
        // Add run_end property
        m_eventBuffer->mutableRun().addProperty(
            "run_end", 
            timeFromPacket(pkt).toISO8601String(),
            true
        );
        
        // Set run details if not already present
        if (!haveRunNumber()) {
            setRunDetails(pkt);  // Uses existing private method
        }
        
        m_listenerState = ListenerState::ReadWait;
        return true;  // Signal parser to pause
    }
    
    return false;  // Continue parsing
}
```

### 3.3 Connection State Management

**In `connect()` method:**

```cpp
bool SNSLiveEventDataListener::connect(const Poco::Net::SocketAddress &address) {
    // ... existing connection logic ...
    
    if (connection successful) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isConnected = true;
        m_listenerState = ListenerState::Connected;
        m_readWait = false;  // Start reading immediately
        
        // If we receive a STATE packet with runNumber != 0,
        // we're already in a run
        // Handled by rxPacket(STATE) calling setRunDetails()
    }
    
    return success;
}
```

**In error handling (background thread exceptions):**

```cpp
// In catch blocks within run() thread
try {
    // ... network operations ...
} catch (const std::exception &e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listenerState = ListenerState::Error;
    m_backgroundException = std::make_shared<std::runtime_error>(e.what());
}
```

---

## 4. Algorithm Migration Strategy

### 4.1 LoadLiveData (Stand-Alone Usage)

**Problem**: Deadlock when `m_pauseNetRead` not cleared

**Solution**: Use `clearReadWait()` before calling `extractData()`

```cpp
void LoadLiveData::exec() {
    auto listener = getListener();
    
    // NEW: Ensure network reading is active
    // Prevents deadlock if listener is paused from previous run
    listener->clearReadWait();
    
    bool dataNotYetGiven = true;
    while (dataNotYetGiven) {
        try {
            chunkWS = listener->extractData();
            dataNotYetGiven = false;
        } catch (Exception::NotYet &ex) {
            // NEW: Enhanced diagnostics
            if (listener->listenerState() == ListenerState::ReadWait) {
                g_log.warning() << "Listener paused: " << ex.what() << "\n";
                g_log.warning() << "Explicitly unblocking network reads.\n";
                listener->clearReadWait();
            } else {
                g_log.warning() << "Listener not ready: " << ex.what() << "\n";
            }
            
            sleepAndCheckInterrupt(10);
        }
    }
    
    // ... existing processing unchanged ...
}
```

**Benefits**:
- ✅ No deadlock in stand-alone usage
- ✅ External control over network reading
- ✅ Better diagnostics via `listenerState()`
- ✅ Minimal code change (one line addition)

### 4.2 MonitorLiveData (Polling Usage)

**Current behavior**: Calls `runStatus()` for transition detection

**Migration Phase 1 - Backward compatible (no changes):**

```cpp
void MonitorLiveData::exec() {
    auto listener = getListener();
    
    // ... inside polling loop ...
    ILiveListener::RunStatus status = listener->runStatus();
    
    if (status == BeginRun || status == EndRun) {
        // Run transition detected, handle workspace management
        // All side effects already executed by deprecated runStatus()
    }
}
```

**Migration Phase 2 - Optimize using new API (optional):**

```cpp
void MonitorLiveData::exec() {
    auto listener = getListener();
    
    // ... inside polling loop ...
    
    // Query pure state first (cheap operation)
    ILiveListener::RunStatus state = listener->listenerRunStatus();
    
    if (listener->isInTransitionState()) {
        // Only call runStatus() when transitions detected
        // Side effects will be executed exactly once
        state = listener->runStatus();
    }
    
    if (state == ILiveListener::BeginRun || state == ILiveListener::EndRun) {
        // Handle run transition behavior
    }
}
```

**Migration Phase 3 - Full external control (advanced):**

```cpp
void MonitorLiveData::exec() {
    auto listener = getListener();
    
    // ... inside polling loop ...
    
    RunStatus transitionState = listener->listenerRunStatus();
    
    if (transitionState == ILiveListener::BeginRun) {
        listener->onBeginRunTransition();  // All side effects in correct order
    } else if (transitionState == ILiveListener::EndRun) {
        listener->onEndRunTransition();    // All side effects in correct order
    }
    
    // Now safe to call extractData()
    Workspace_sptr data = listener->extractData();
    
    // ... process data ...
}
```

**Recommendation**: Implement Phase 1 initially (backward compatible), add Phase 2 optimization in later release.

### 4.3 StartLiveData (Lifecycle Management)

**No changes required** - uses base class factory and lifecycle management only.

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
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), NoRun);
        TS_ASSERT_EQUALS(listener.listenerState(), Disconnected);
        TS_ASSERT_EQUALS(listener.runStatus(), NoRun);  // Deprecated
    }
    
    void test_adara_state_tracking() {
        // Simulate NEW_RUN packet
        listener.handleNewRunPacket();
        
        TS_ASSERT_EQUALS(listener.runState(), BeginRun);      // ADARA state
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), BeginRun); // Transition state
        TS_ASSERT_EQUALS(listener.runStatus(), BeginRun);      // Legacy
        
        // After clearReadWait(), no transitions should occur yet
        listener.clearReadWait();
        TS_ASSERT_EQUALS(listener.runState(), BeginRun);
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), BeginRun);
    }
    
    void test_transition_state_detection() {
        listener.handleNewRunPacket();
        TS_ASSERT(listener.isInTransitionState());  // BeginRun is transition
        
        listener.handleEndRunPacket();
        TS_ASSERT(listener.isInTransitionState());  // EndRun is transition
        
        listener.transitionRunStatus(NoRun);
        TS_ASSERT(!listener.isInTransitionState());  // NoRun is not transition
    }
    
    void test_run_status_side_effects() {
        listener.handleNewRunPacket();
        
        // First call returns cached state and executes side effects
        RunStatus s1 = listener.runStatus();
        TS_ASSERT_EQUALS(s1, BeginRun);
        
        // Verify all side effects occurred
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), Running);  // Transitioned
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);      // Read-wait cleared
        TS_ASSERT(!listener.hasDeferredRunDetails());
    }
    
    void test_run_state_no_side_effects() {
        listener.handleNewRunPacket();
        
        // runState() is pure - no transitions
        RunStatus s1 = listener.runState();
        RunStatus s2 = listener.runState();
        TS_ASSERT_EQUALS(s1, s2);
        
        // Verify no changes
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), BeginRun);
    }
    
    void test_read_wait_external_control() {
        listener.setReadWait(true);
        TS_ASSERT_EQUALS(listener.listenerState(), ReadWait);
        
        listener.clearReadWait();
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);
    }
    
    void test_begin_run_transition_facade() {
        listener.handleNewRunPacket();
        
        // Save state before transition
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), BeginRun);
        TS_ASSERT(listener.isInTransitionState());
        
        // Execute facade
        listener.onBeginRunTransition();
        
        // Verify results
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), Running);
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);
        TS_ASSERT(!listener.hasDeferredRunDetails());
    }
    
    void test_end_run_transition_facade() {
        listener.handleEndRunPacket();
        
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), EndRun);
        
        // Execute facade
        listener.onEndRunTransition();
        
        TS_ASSERT_EQUALS(listener.listenerRunStatus(), NoRun);
        TS_ASSERT_EQUALS(listener.listenerState(), Connected);
    }
    
    void test_granular_cache_control() {
        // Test each cache clearing method independently
        listener.setTestInstrumentCache("test.xml", "TEST_INSTR");
        listener.clearInstrumentCache();
        TS_ASSERT(listener.getInstrumentXML().empty());
        
        listener.setTestDataStartTime();
        listener.clearDataStartTime();
        TS_ASSERT(listener.getDataStartTime().is_not_a_date_time());
        
        listener.populateNameMap();  // Test helper
        listener.clearNameMap();
        TS_ASSERT(listener.getNameMap().empty());
    }
    
    void test_reinitialize_workspace() {
        // Save pointer to old workspace
        auto oldWorkspace = listener.getEventBuffer();
        
        listener.reinitializeWorkspace();
        
        // Should have new workspace
        auto newWorkspace = listener.getEventBuffer();
        TS_ASSERT_DIFFERS(oldWorkspace.get(), newWorkspace.get());
        
        // Should have required properties
        TS_ASSERT(newWorkspace->run().hasProperty("pause"));
        TS_ASSERT(newWorkspace->run().hasProperty("scan_index"));
        TS_ASSERT(newWorkspace->run().hasProperty("proton_charge"));
    }
    
    void test_exception_propagation() {
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
    // This is the critical test that verifies the deadlock fix
    LoadLiveData alg;
    alg.setRethrows(true);
    alg.setChild(true);
    
    // Configure for SNS listener
    alg.setProperty("Instrument", "SNS_Live");
    alg.setProperty("Connection", "localhost:31415");
    
    // Should not hang - would deadlock with old implementation
    TS_ASSERT_THROWS_NOTHING(alg.execute());
    TS_ASSERT(alg.isExecuted());
    
    // Verify data was actually extracted
    Workspace_sptr output = alg.getProperty("OutputWorkspace");
    TS_ASSERT(output);
}

void test_read_wait_diagnostics() {
    // Test that clearReadWait() call provides diagnostics
    LoadLiveData alg;
    
    // Mock listener in read-wait state
    auto mockListener = std::make_shared<MockSNSLiveListener>();
    mockListener->setReadWait(true);
    mockListener->setThrowsNotYet(true);
    
    alg.setListener(mockListener);
    alg.setProperty("Instrument", "SNS_Live");
    
    TS_ASSERT_THROWS_NOTHING(alg.execute());
    
    // Verify clearReadWait() was called
    TS_ASSERT(mockListener->verifyClearReadWaitCalled());
    // Verify diagnostic messages were logged
    TS_ASSERT(mockListener->verifyLogContains("Listener paused"));
}
```

**Test file:** `Framework/LiveData/test/MonitorLiveDataTest.h`

```cpp
void test_backward_compatible_behavior() {
    // Verify existing MonitorLiveData usage patterns still work
    MonitorLiveData alg;
    alg.setRethrows(true);
    alg.setChild(true);
    
    // Setup algorithm
    alg.setProperty("Instrument", "SNS_Live");
    alg.setProperty("UpdateEvery", 1);
    alg.setProperty("RunTransitionBehavior", "Rename");
    
    // Execute (mock listener should produce BeginRun/EndRun sequence)
    TS_ASSERT_THROWS_NOTHING(alg.execute());
    
    // Verify run transitions detected correctly
    auto listener = alg.getListener();
    TS_ASSERT(listener->listenerRunStatus() == NoRun || 
              listener->listenerRunStatus() == Running);
}

void test_run_transition_workspace_renaming() {
    // Specifically test Rename behavior which depends on runStatus() return value
    MonitorLiveData alg;
    alg.initialize();
    alg.setChild(true);
    
    // Mock listener
    auto mockListener = std::make_shared<MockSNSLiveListener>();
    mockListener->setSimulatedRunNumber(12345);
    alg.setListener(mockListener);
    
    alg.setProperty("Instrument", "SNS_Live");
    alg.setProperty("OutputWorkspace", "test_output");
    alg.setProperty("RunTransitionBehavior", "Rename");
    alg.setProperty("UpdateEvery", 1);
    
    TS_ASSERT_THROWS_NOTHING(alg.execute());
    
    // Verify workspace was renamed with run number
    TS_ASSERT(AnalysisDataService::Instance().doesExist("test_output_12345"));
}
```

### 5.3 Concurrency Tests

```cpp
void test_concurrent_state_access() {
    SNSLiveEventDataListener listener;
    
    // Background thread simulating packet arrival
    std::thread bg([&listener]() {
        for (int i = 0; i < 100; ++i) {
            // Simulate various packet types
            switch (i % 3) {
                case 0: listener.handleNewRunPacket(); break;
                case 1: listener.handleEndRunPacket(); break;
                case 2: listener.handleBankedEvents(); break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    
    // Foreground thread querying state (simulates algorithms)
    std::thread fg([&listener]() {
        for (int i = 0; i < 300; ++i) {
            // Query various states
            RunStatus s1 = listener.runState();
            ListenerState ls = listener.listenerState();
            RunStatus s2 = listener.listenerRunStatus();
            bool inTransition = listener.isInTransitionState();
            
            // Verify no crashes or data races
            TS_ASSERT(s1 == NoRun || s1 == BeginRun || s1 == Running || s1 == EndRun);
            TS_ASSERT(ls == Disconnected || ls == Connected || ls == ReadWait || ls == Error);
            
            // Occasionally call runStatus() for backward compat testing
            if (i % 10 == 0) {
                try {
                    listener.runStatus();
                } catch (Exception::NotYet &) {
                    // Expected in some states
                }
            }
        }
    });
    
    bg.join();
    fg.join();
    
    // If we get here, no deadlocks or crashes occurred
    TS_ASSERT(true);
}
```

### 5.4 Deadlock Reproduction Test (Prerequisite)

**Critical**: This test reproduces the deadlock BEFORE implementing the fix

```cpp
void test_deadlock_reproduction() {
    // This test demonstrates the deadlock in current implementation
    // Should be written and run BEFORE implementing the fix to verify the bug exists
    // After fix implementation, this test should pass (no deadlock)
    
    // Setup: Connect listener, receive NEW_RUN packet to trigger m_pauseNetRead = true
    SNSLiveEventDataListener listener;
    listener.connect(testAddress);
    listener.start();
    
    // Simulate receiving NEW_RUN packet before workspace initialized
    listener.simulateNewRunPacket();
    
    // Now try to use LoadLiveData pattern (calls only extractData(), never runStatus())
    bool deadlockDetected = false;
    std::thread extractionThread([&listener, &deadlockDetected]() {
        try {
            auto start = std::chrono::steady_clock::now();
            listener.extractData();
            auto end = std::chrono::steady_clock::now();
            
            // If it takes > 5 seconds, likely deadlock
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
            if (duration.count() > 5) {
                deadlockDetected = true;
            }
        } catch (...) {
            // Expected if not deadlocked
        }
    });
    
    extractionThread.detach();  // Let it run
    std::this_thread::sleep_for(std::chrono::seconds(6));
    
    TS_ASSERT(deadlockDetected);  // Before fix: should be true
    
    // After implementing fix with clearReadWait(), this test should fail (no deadlock)
    // Then update expected result to TS_ASSERT(!deadlockDetected)
}
```

---

## 6. Implementation Plan

### Phase 1: Core Infrastructure (1 week)

**Goal**: Add new interface methods and state variables without breaking existing functionality

**Actions**:
1. Update `Framework/API/inc/MantidAPI/ILiveListener.h` with new virtual methods
2. Add state variables to `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`
3. Implement getters: `runState()`, `listenerRunStatus()`, `isInTransitionState()`, `listenerState()`, `hasDeferredRunDetails()`
4. Implement network control: `setReadWait()`, `clearReadWait()`
5. Implement cache management: All granular clearing methods
6. Implement run details: `setRunDetails()`, `clearDeferredRunDetails()`
7. Mark `runStatus()` as deprecated but DO NOT reimplement yet

**Testing**: All existing tests should pass with no changes

**Risk**: Low - additive only, no behavior changes

### Phase 2: Facade Implementation (1-2 weeks)

**Goal**: Implement high-level facade methods that replicate `runStatus()` behavior

**Actions**:
1. Implement `transitionRunStatus()` for explicit state transitions
2. Implement `onBeginRunTransition()` and `onEndRunTransition()` facades
3. Update packet handlers to set BOTH state variables
4. Replace `m_status` usage with `m_adaraRunStatus`/`m_runStatus` in rxPacket()
5. Replace `m_pauseNetRead` with `m_readWait` in run() loop
6. Reimplement `runStatus()` to delegate to facade methods (backward compatibility)

**Testing**: 
- New unit tests for all granular methods
- Test facade methods execute correct operations in correct order
- Verify `runStatus()` behavior unchanged (existing tests still pass)
- Run deadlock reproduction test (should now pass)

**Risk**: Medium - Complex state relationships, must preserve exact timing

### Phase 3: Integration Testing (1 week)

**Goal**: Test algorithm integration and fix deadlock

**Actions**:
1. Update `LoadLiveData` to call `clearReadWait()` before extractData()
2. Write integration tests for stand-alone usage
3. Write concurrency tests
4. Verify backward compatibility with `MonitorLiveData`
5. End-to-end test with live SNS data (if test system available)

**Testing**:
- Stand-alone LoadLiveData no longer deadlocks
- MonitorLiveData behavior unchanged
- Concurrent access doesn't cause data races

**Risk**: Low - Based on solid unit tests from Phase 2

### Phase 4: Optimization (Future - Optional)

**Goal**: Optionally optimize MonitorLiveData using new pure getters

**Actions**:
1. Add conditional logic to only call `runStatus()` when transition detected
2. Update documentation with migration guide
3. Promote deprecation warning to stronger level
4. Consider making `runState()` and `listenerRunStatus()` `const` methods

**Testing**: Performance tests comparing old vs new implementation

**Risk**: Very low - All changes optional and isolated

---

## 7. Backward Compatibility Guarantee

**Existing behavior will be preserved exactly:**

- ✅ `runStatus()` return values unchanged
- ✅ `runStatus()` side effects unchanged (one-time state transitions, cache clearing)
- ✅ `extractData()` behavior unchanged
- ✅ Exception propagation unchanged
- ✅ `MonitorLiveData`, `LoadLiveData`, `StartLiveData` algorithms work identically
- ✅ Timing of side effects preserved (critical for MonitorLiveData)

**New API is strictly additive** - no existing methods removed or signature-modified.

**Deprecation timeline**:
1. **v6.x**: Add new API, deprecate `runStatus()` (current version)
2. **v7.x**: Remove `runStatus()` from ILiveListener (breaking change)
3. **v8.x**: Remove deprecated implementations from concrete listeners

---

## 8. Critical Implementation Notes

### 8.1 Mutex Synchronization

**All state variable access must be protected by `m_mutex`:**
```cpp
// All methods require mutex except background exception check
void SNSLiveEventDataListener::someNewMethod() {
    if (m_backgroundException) throw(*m_backgroundException);  // No lock needed
    std::lock_guard<std::mutex> lock(m_mutex);  // All other operations locked
    // ... access state variables ...
}
```

**Exception**: `m_backgroundException` is thread-safe via `shared_ptr` (atomic reference count)

**Performance consideration**: Use `std::lock_guard`, not `std::unique_lock` unless needed for condition variables

### 8.2 Initialization Order

```cpp
// Members declared in this order
ILiveListener::RunStatus m_adaraRunStatus{NoRun};  // First: ADARA protocol state
ILiveListener::RunStatus m_runStatus{NoRun};        // Second: Listener transition state
ListenerState m_listenerState{Disconnected};        // Third: Connection state
bool m_readWait{false};                            // Fourth: Network control
bool m_isConnected{false};                          // Fifth: Connection flag

// m_readWait starts as false (not paused)
// This maintains existing behavior where background thread begins reading immediately
// Pause is only set when BeginRun or EndRun packets arrive
```

### 8.3 Exception Handling

**Consistent exception handling pattern**:

```cpp
RunStatus SNSLiveEventDataListener::runState() const {
    // Check background exception FIRST (before acquiring lock)
    if (m_backgroundException) throw(*m_backgroundException);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // No other exceptions expected, return value directly
    return m_adaraRunStatus;
}

void SNSLiveEventDataListener::setReadWait(bool value) {
    // Background exception doesn't prevent state changes
    if (m_backgroundException) throw(*m_backgroundException);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Perform state change
    m_readWait = value;
    // ... update listener state ...
}
```

### 8.4 Testing Considerations

**Mocking strategy**: Create `MockSNSLiveEventDataListener` that:
- Overrides network I/O methods to avoid actual socket connections
- Provides test helpers: `injectBackgroundException()`, `simulateNewRunPacket()`
- Allows state inspection: `getEventBuffer()`, `getInstrumentXML()`

**Thread safety tests**: Use `std::thread` (not Poco::Thread) for concurrency tests to avoid test framework dependencies

**Performance tests**: Measure overhead of new mutex locks (should be negligible compared to network I/O)

### 8.5 Common Pitfalls to Avoid

1. **Don't acquire mutex in const methods**: Declare `getParserState()` as non-const if it needs mutex
2. **Don't forget exception check**: All new public methods should check `m_backgroundException`
3. **Don't change rxPacket return semantics**: Must return `true` to pause, `false` to continue
4. **Don't forget m_dataStartTime clearing**: Only clear on EndRun, NOT on BeginRun (original behavior)
5. **Don't call reinitializeWorkspace() unnecessarily**: Only at run transitions and initialization

---

## 9. Files to Modify

### Core Implementation

1. `Framework/API/inc/MantidAPI/ILiveListener.h`
   - Add new virtual methods and enums

2. `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`
   - Add new private state members
   - Declare new public methods
   - Include method documentation comments

3. `Framework/LiveData/src/SNSLiveEventDataListener.cpp`
   - Implement all new methods
   - Update packet handlers (rxPacket, run)
   - Reimplement runStatus() as delegation to facades

### Algorithm Updates (Phase 3)

4. `Framework/LiveData/src/LoadLiveData.cpp`
   - Add `listener->clearReadWait()` call before extractData()

5. `Framework/LiveData/src/MonitorLiveData.cpp` (optional Phase 4)
   - Optimize polling logic to use pure getters

### Test Files

6. `Framework/LiveData/test/SNSLiveEventDataListenerTest.h`
   - Add new unit test methods
   - Add MockSNSLiveEventDataListener test helper class

7. `Framework/LiveData/test/LoadLiveDataTest.h`
   - Add deadlock test
   - Add standalone usage test

8. `Framework/LiveData/test/MonitorLiveDataTest.h`
   - Add backward compatibility test
   - Add run transition workspace rename test

### Documentation

9. `docs/source/algorithms/LoadLiveData-v1.rst`
   - Document deadlock fix and usage of clearReadWait()

10. `dev-docs/source/Standards/ILiveListenerInterface.rst` (new file)
    - Document new API with migration guide
    - Include complete state machine diagrams
    - Provide code examples using new methods

---

## 10. Success Criteria

The refactoring is considered successful when:

1. ✅ Stand-alone `LoadLiveData` executes without deadlock
2. ✅ All existing unit tests pass without modification
3. ✅ MonitorLiveData behavior unchanged (integration tests pass)
4. ✅ New unit tests for all granular and facade methods pass
5. ✅ Concurrency tests pass without data races or deadlocks
6. ✅ Performance overhead is < 1% (mutex operations are negligible vs network I/O)
7. ✅ ClearReadWait.cpp compilation: No warnings introduced
8. ✅ clang-tidy on touched files shows no new issues
9. ✅ Documentation updated with migration examples
10. ✅ Release notes added describing deadlock fix

---

## Version History

- **v1.0** (Initial spec): Identified problem and proposed basic solution
- **v1.1** (This version): 
  - Added complete decomposition of runStatus() side effects
  - Added granular cache management methods (clearRequiredLogs, clearMonitorLogs, clearVariableCache)
  - Added critical `reinitializeWorkspace()` method
  - Added `transitionRunStatus()` for explicit state transitions
  - Added `onBeginRunTransition()` and `onEndRunTransition()` facade methods
  - Added detailed concurrency tests
  - Added deadlock reproduction test (prerequisite)
  - Fixed initialization order and read-wait default value
  - Made mutex usage and exception handling consistent
  - Added complete migration examples for all three algorithms

---

**Owner**: Mantid Development Team  
**Reviewers**: Core Framework Team, Live Data Experts  
**Estimated Effort**: 3-4 weeks (including comprehensive testing)  
**Risk Level**: Medium (complex state machine but well-tested)  
**Backward Compatibility**: 100% - Zero breaking changes
