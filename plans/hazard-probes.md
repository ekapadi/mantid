# Component 3 — hazard probes: tests that deliberately trigger the lock-ups

> **Status: design / discussion. Not ready to implement.**
> Part 3 of 3. Companions: [`first-pass.md`](first-pass.md) (investigation history),
> [`keepalive-watchdog.md`](keepalive-watchdog.md) (detection),
> [`deferred-action-refactor.md`](deferred-action-refactor.md) (the fix).
>
> These probes turn the hypotheses in the other documents into executable evidence, and become
> the regression tests for the refactor.

______________________________________________________________________

## Story for EWM use

*Plain prose, intended to be pasted directly into an EWM work item. Everything below this section
is implementation detail and does not need to go into EWM.*

The investigation into the live-reduction service hang identified two related concurrency hazards
in the SNS live event data listener: it calls into the Mantid framework while holding its internal
lock, and it makes those calls from a background thread that no other part of the system knows
exists. Both are plausible explanations for a failure that leaves no diagnostic trace, but neither
has been demonstrated, because the production failure is rare and has never been captured. Any fix
built on an undemonstrated hypothesis carries the risk that it addresses the wrong problem, and
any fix delivered without a failing test has no way to show that it worked.

This story delivers a small set of deliberately hostile tests that reproduce the hazardous
conditions on demand. Because a test controls the timing, the thread scheduling, the logging
configuration and the duration completely, the conditions that occur only rarely in production can
be forced to occur every time. One probe holds the listener's lock across a blocking framework
call and confirms that both listener threads stop making progress. A second exercises the
interaction between Mantid's logging lock and the Python interpreter lock, which is a genuine
hazard in the deployed logging configuration. A prototype of the second has already been run
successfully outside the build tree, producing a deadlock deterministically against the real Poco
logging framework and the real Python interpreter lock, so the approach is known to work before
any work is committed.

The probes are built as optional targets alongside the existing LiveData tests and are disabled by
default, since they deliberately hang and would be unsuitable for routine continuous integration.
Each isolates the hazardous scenario in a child process with an independent observer that
terminates it once the condition is confirmed, so a reproduced deadlock is reported as a
successful test result rather than hanging the build. Each probe is written so that the same
executable expresses both expectations: before the structural refactor it should report the
deadlock, and after the refactor the identical test should report that progress continued. That
inversion is the acceptance criterion for the refactor.

The probes convert hypotheses into evidence, so the refactoring work is justified by a
demonstrated failure rather than by code inspection alone; they provide the regression test that
shows the fix is effective, which is otherwise difficult for a defect that cannot be reproduced;
and they remain in the tree as a permanent guard against a future change reintroducing the
hazard. The most significant benefit, however, is that several of these hazards are defects in the
Mantid framework itself rather than in the listener, and refactoring the listener only stops one
caller from provoking them. A reproducible test changes what can be asked for upstream: an
observation that a shared component looks unsynchronised is easy to defer, whereas a test that
deadlocks against the real classes is a defect report. At least five distinct framework-level
synchronisation defects have been identified during this investigation — affecting logging,
configuration access, the object factories, the data service's notification dispatch, and the
live-listener base class — each of which would be a separate upstream story with its own owner.
This work supplies the evidence needed to raise them.

______________________________________________________________________

## 1. Does this actually work? Yes — demonstrated

**The GIL inversion has already been reproduced deterministically** outside the build tree, using
the real `Poco::SplitterChannel` and the real CPython GIL:

```
1. bg thread holds the SplitterChannel mutex (inside dispatch)
2. other thread holds the GIL
3. GIL-holder is now blocked on the SplitterChannel mutex
4. released bg thread -> it now needs the GIL
RESULT: DEADLOCK after 3s  (bgGotGil=0 mainGotMutex=0)
exit=0  (0 = deadlock reproduced, 1 = no deadlock)
```

Two prerequisites were measured rather than assumed:

- **`Poco::SplitterChannel` holds its `_mutex` across sub-channel dispatch.** Verified directly: a
  sub-channel blocked inside `log()` blocks *other threads'* `log()` calls. Had the splitter
  copied its channel list and released the mutex before dispatching, no inversion would be
  possible and the whole hypothesis would have needed rework.
- **`std::atomic_ref<uint64_t>::is_always_lock_free == 1`** on this platform (relevant to
  Component 1, checked in the same run).

So the answer to "should it be possible to trigger these lock-ups?" is **yes for the hazard
class** — a GIL holder blocking on a C-level lock deadlocks reliably and on demand. Whether that
shape occurs on the *production* path is a separate question, and §3.0 records where that stands
after a claim in an earlier revision had to be retracted. See also §5.

______________________________________________________________________

## 2. Probe P1 — blocking framework call under the parse lock

**Target hazard:** [`deferred-action-refactor.md`](deferred-action-refactor.md) §1–§2. The
background thread holds `m_mutex` across `LoadInstrument`; if that call does not return, the
foreground blocks on the lock forever and nothing is logged.

**Deterministic, no timing luck, no synthetic framework.** The hook is already virtual and the
subclassing pattern already exists in the test tree — `TestableSNSListener`,
`ShortWriteListener`, `ThrowOnRunStatusListener`, `PartialHelloListener` in
`Framework/LiveData/test/`.

`initWorkspacePart2()` is **not** virtual, so it cannot be overridden directly. But
`rxPacket(const ADARA::GeometryPkt &)` **is** (`SNSLiveEventDataListener.h:101`), it runs under
`m_mutex` (acquired once by `run()` around `bufferParse()`), and it is exactly where
`initWorkspacePart2()` is called from. Overriding it models the hazard precisely:

```cpp
struct BlockingFrameworkListener : SNSLiveEventDataListener {
  std::atomic<bool> insideLockedRegion{false}, letGo{false};
  bool rxPacket(const ADARA::GeometryPkt &pkt) override {
    insideLockedRegion = true;                       // we hold m_mutex here
    while (!letGo) std::this_thread::sleep_for(1ms); // stand in for a wedged
    return SNSLiveEventDataListener::rxPacket(pkt);  //   LoadInstrument
  }
};
```

**Sequence:** feed a geometry packet → wait for `insideLockedRegion` → call `extractData()` from
the foreground → assert it does not return within N seconds → set `letGo` → assert both threads
complete.

**Expectation inversion:** pre-refactor the foreground blocks (probe reports the hazard);
post-refactor the framework call happens outside the lock and on the foreground thread, so
`extractData()` is unaffected and the probe reports progress. **This is the acceptance criterion
for Variant B.**

Note this probe needs no deadlock at all — `letGo` releases it — so it can run inside the normal
cxxtest suite without process isolation, unlike P2.

### 2.1 The vagueness problem — and a fix for it

As written above, P1 is close to a tautology: *we* choose to block under the lock, then observe
that blocking under the lock blocks. It demonstrates a structural property, not that any realistic
call would actually hang there. Worse, a real block would usually accompany some error condition
that ought to have thrown and been logged — and the synthetic gate suppresses exactly that. So P1
in the semaphore form is a **regression guard for the invariant**, not evidence for the hang, and
should not be presented as the latter.

**A non-vague variant: make real framework code block for a real reason.** The geometry-cache read
at `InstrumentDefinitionParser.cpp:2584` is `m_cacheFile->exists() && applyCache(m_cacheFile)`,
with the path derived from `getVTPFileDirectory()` / `getTempDir()` (`:2579`, `:2990`). Create the
expected `<mangled>.vtp` path as a **FIFO with no writer**: `exists()` succeeds, and the read
blocks indefinitely inside genuine framework code.

That gives a real blocking framework call, under the real parse lock, with no synthetic hook and
no suppressed exception — and it directly models the network-filesystem stall that motivated the
whole hypothesis. It also escapes the criticism above entirely, because a FIFO open is not an
error condition: there is nothing that *should* have been logged.

Needs verifying that `IDFObject::exists()` treats a FIFO as existing and that `applyCache()` opens
it for read rather than stat-ing it first — open question 6.

______________________________________________________________________

## 3. Probe P2 — the logging deadlock

### 3.0 Configuration confirmed — but the proposed cycle does NOT close

**Two separate results, and they must not be conflated.**

**(a) The configuration finding stands.** The live-reduce service *is* configured with a Python
log channel — from `/home/ux0/workspaces/livereduce`:

```python
scripts/livereduce.py:17   from mantid.utils.logging import log_to_python as mtd_log_to_python
scripts/livereduce.py:29   mtd_log_to_python("information")     # at module import
scripts/livereduce.py:34   fileHandler = logging.FileHandler("/var/log/SNS_applications/livereduce.log")
```

`log_to_python()` (`mantid/utils/logging.py:87`) sets
`logging.channels.consoleChannel.class = "PythonLoggingChannel"`, at `"information"` level — so
every listener `g_log.information/notice/warning/error` is forwarded into Python, with the GIL
acquired inside `PythonLoggingChannel::log()` (`:70-76`) **while `SplitterChannel` holds
`_mutex`**. That much is real.

**(b) The deadlock cycle previously claimed here was WRONG, and is retracted.** An earlier
revision asserted a confirmed cycle of GIL ↔ Python logging handler lock. It does not close.

*Measured:* **CPython releases the GIL while a thread blocks on a `threading.Lock`/`RLock`.** A
CPU-bound Python thread completed 20.7 million iterations while another thread sat in
`lock.acquire()`. So the chain unwinds harmlessly:

1. Thread A holds the splitter mutex, acquires the GIL, calls Python logging, blocks on the
   handler `RLock` — **and drops the GIL while blocked**.
1. Thread B reacquires the GIL, finishes `emit()`, releases the handler lock.
1. Thread A wakes, completes, releases the GIL and the splitter mutex.

No deadlock. **Two threads logging concurrently is not sufficient.**

**(c) What the §1 reproduction actually proves.** The demonstrated deadlock is real, but its
essential property is narrower than stated: the GIL holder blocked on a **C-level** lock (Poco
`FastMutex`), which — unlike a Python-level lock — does **not** release the GIL. So the
requirement is:

> **Some thread must hold the GIL and then block on a C-level lock**, while another thread holds
> that C-level lock and is waiting for the GIL.

**(d) The production partner is not identified.** Mantid's Python `Logger` bindings release the
GIL on every call (`Logger.cpp:36,41,46,51,56,61`), closing the obvious route. A GIL-holding
thread would have to enter `SplitterChannel::log()` through some *other* binding that does not
release the GIL — for example a boost::python export called from inside a Python algorithm's
`PyExec` (where `AlgorithmAdapter` holds the GIL) that logs internally. **No such path has been
found yet.** Until one is, F1 is a demonstrated *hazard class* with a confirmed *exposure* in the
deployment, not a demonstrated cause.

**(e) A separate, untested reentrancy hazard in the same area.** `Poco::FastMutex` is
**non-recursive**, and `PythonLoggingChannel::log()` calls arbitrary Python code while holding it.
Any logging handler, filter or formatter that calls back into Mantid's C++ logger would re-enter
`SplitterChannel::log()` on the same thread and **self-deadlock immediately**. livereduce's own
handlers are plain `FileHandler`/`StreamHandler`, but the instrument-specific script — not yet
available — could add one. Worth checking when that script arrives.

### 3.0b THE MISSING HALF, FOUND — `AlgorithmManager.shutdown()` holds the GIL

§3.0(d) asked what production path lets a thread hold the GIL and then block. **Answer: Mantid's
own Python binding for `AlgorithmManager.shutdown()`, and the deadlock risk is acknowledged in a
source comment with an upstream issue number.**

`Framework/PythonInterface/mantid/api/src/Exports/AlgorithmManager.cpp:56-67`:

```cpp
void clear(AlgorithmManagerImpl *self) {
  /// TODO We should release the GIL here otherwise we risk deadlock (see issue #33895). However,
  /// doing so causes test failures because it exposes an unrelated bug to do with the way we
  /// handle shared_ptrs to Python objects (see #33924). Fixing that is not trivial, so I am
  /// reverting this change until it can be resolved properly.
  // ReleaseGlobalInterpreterLock releaseGIL;
  return self->clear();
}
void shutdown(AlgorithmManagerImpl *self) {
  // See comment above for clear()
  // ReleaseGlobalInterpreterLock releaseGIL;
  return self->shutdown();
}
```

`Framework/API/src/AlgorithmManager.cpp:186-192` — it **spin-waits**:

```cpp
void AlgorithmManagerImpl::shutdown() {
  cancelAll();
  while (runningInstances().size() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  clear();
}
```

**The cycle, with every element in deployed code:**

| Thread                                                                         | Holds                                     | Waits for                                               |
| ------------------------------------------------------------------------------ | ----------------------------------------- | ------------------------------------------------------- |
| **T1** main Python — `livereduce.py:85` `AlgorithmManager.shutdown()`          | the **GIL** (binding does not release it) | `runningInstances()` to empty — spin-wait at `:188`     |
| **T2** MonitorLiveData async thread (`StartLiveData.cpp:230` `executeAsync()`) | its place in `runningInstances()`         | the **GIL** — to run the reduction script, *and* to log |
| **T3** listener background thread                                              | `SplitterChannel::_mutex`                 | the **GIL** — `PythonLoggingChannel` on every `g_log`   |

T1 cannot finish until T2 exits; T2 cannot exit without the GIL; T1 holds it. **Permanent.**

**`log_to_python` is what makes this near-inescapable.** With `PythonLoggingChannel` installed
(`livereduce.py:29`), *every* Mantid log call anywhere in the process requires the GIL. So it is
not merely the Python reduction script that stalls — a cancelled algorithm that logs "interrupted"
on its way out also stalls, and therefore never leaves `runningInstances()`.

**Fits every constraint:**

| Constraint                   | Why it fits                                                                                                                               |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| Silent                       | T1 is in a C++ `sleep_for` loop; T2/T3 cannot log without the GIL. Nothing can be written.                                                |
| Never self-heals             | Circular wait around a spin loop.                                                                                                         |
| Predates the refactor        | `m_mutex` is nowhere in the cycle.                                                                                                        |
| Rare                         | `stop()` is only reached on teardown paths — `restart_and_clear()` when the processing script changes (`livereduce.py:392`), or a signal. |
| Process alive, needs SIGKILL | T1 spins at 100 ms; nothing is in `D` state. Consistent with `SIGKILL` always having been used.                                           |

**Corroborating detail:** `livereduce.py:86-87` already guards this area —
`if runningInstancesOf("MonitorLiveData"): raise RuntimeError("MonitorLiveData algorithm could not be stopped")`. Someone has previously seen shutdown fail to complete. If `shutdown()` deadlocks,
that check is never reached.

**The trigger is the memory checker, not an operator.** `livereduce.py:434-437` starts a
memory-checker thread, enabled by default (`system_mem_limit_perc` = **70**% of RAM,
`mem_check_interval_sec` = **1** s):

```python
def memory_checker(config, livemanager):  # :396
    while True:
        mem_used = config.proc_pid.memory_info().rss
        if mem_used > config.mem_limit:
            logger.error(f"Memory usage {...} MB exceeds limit")  # :400
            livemanager.restart_and_clear()  # :401
        time.sleep(config.mem_check_interval_sec)
```

`restart_and_clear()` → `stop()` → `AlgorithmManager.shutdown()` (`:85`). So the GIL-holding
shutdown is reached **automatically in steady state**, once RSS crosses the threshold after days
of accumulation. That resolves the objection that this is a "shutdown-only" hang: the service is
never deliberately shut down — it shuts *itself* down, and deadlocks doing so. Memory growth is the
only mechanism in the system with a multi-day time constant, which is exactly the observed
"runs for a few days, then hangs".

**THE CHECK — one grep, against logs that already exist.** `logger.error("Memory usage … exceeds limit")` at `:400` is a plain Python logging call emitted *before* `restart_and_clear()`, so it is
written before anything deadlocks:

```
grep -n "exceeds limit" /var/log/SNS_applications/livereduce.log*
```

> **If that message is the last entry before each hang, the hypothesis is confirmed** — no
> reproduction, no instrumentation, no waiting. Correlate the timestamps with the watchdog
> restarts.

**Mitigations, cheapest first:**

1. **Avoid `AlgorithmManager.shutdown()` from Python while algorithms run.** `cancelAll()` *does*
   release the GIL (`AlgorithmManager.cpp` export `:71`), and so does `runningInstancesOf(...)`
   (`:69`). So: call `cancelAll()`, then poll `runningInstancesOf("MonitorLiveData")` from Python
   with a `time.sleep()` between attempts and a bounded timeout — `time.sleep` releases the GIL,
   so the algorithms can actually finish. Never call the GIL-holding `shutdown()` while anything
   is running. This is a **livereduce-side change requiring no framework fix**, and it is the
   single cheapest thing available.
   *Caveat:* this fixes the shutdown-initiated deadlock. It does **not** help if a thread is
   already wedged, nor does it address the listener destructor's join (see the second window
   above) — for that, mitigation 2 is also needed.
1. **Drop `log_to_python`** (the service-only-channel idea). Removes the GIL from the logging path,
   so a cancelled algorithm can at least log its way out.
1. **Upstream: release the GIL in `shutdown()`/`clear()`** — issue #33895, blocked on #33924.

### 3.1 Probe construction

**Target hazard:** [`deferred-action-refactor.md`](deferred-action-refactor.md) §2b.1.

Structure of the already-working prototype, to be reproduced in-tree:

1. `Py_Initialize()`, then `PyEval_SaveThread()` so the GIL is available to other threads.
1. A `SplitterChannel` with two sub-channels: a **gate channel** (synchronisation hook) followed by
   the channel that acquires the GIL.
1. Thread B logs → enters the splitter mutex → blocks in the gate → **holds the splitter mutex**.
1. Thread C acquires the GIL, then logs → **blocks on the splitter mutex while holding the GIL**.
1. Release the gate → thread B proceeds to `PyGILState_Ensure()` → **blocks on the GIL**.
1. Deadlock, every time.

**Use the real `PythonStdoutChannel`, not a stand-in.** The prototype substituted a small
`GilChannel` doing `PyGILState_Ensure()`/`Release()` because Mantid was not built in the scratch
environment. That is exactly what `PythonStdoutChannel.cpp:28` does inside its sink, but the
in-tree probe should link `MantidPythonInterfaceCore` and use the production channel so that
nothing in the lock cycle is synthetic. The gate channel then remains the *only* test-specific
element, and it contributes no locking of its own — it merely parks inside a dispatch that the
splitter was already going to serialise.

**Why the gate is legitimate.** It does not create the inversion; the lock order
(splitter mutex → GIL on one side, GIL → splitter mutex on the other) is a property of the real
code. The gate removes the need to *win a race* to observe it. This is the standard technique for
testing an interleaving, and the alternative — hammering both threads and hoping — is exactly the
non-determinism that has made this bug untraceable for so long.

**Variant P2b — model the *deployed* logging path, and expect it NOT to deadlock.** Per §3.0(b)
this configuration does not close a cycle, so P2b's job is to **falsify**, and to stay in the tree
as the guard that would notice if that ever changed:

1. `mtd_log_to_python("information")`, exactly as `livereduce.py:29` does.
1. Install a Python logging handler whose `emit()` gates — signalling on entry and blocking while
   holding the handler `RLock`.
1. From a `Poco::Thread` Python has never seen, call `g_log.information(...)`: it takes the
   splitter mutex, acquires the GIL, and blocks on the handler lock — **releasing the GIL as it
   blocks**.
1. Assert the Python thread still makes progress, i.e. **`--expect-progress` is the correct
   expectation today.**

If this probe ever reports a deadlock — because a future CPython changes lock behaviour, or a
handler introduces a C-level lock — that is a genuine regression worth catching. Writing a probe
whose expected result is "no deadlock" is unusual but correct here: it encodes the measurement in
§3.0(b) as an executable claim rather than a paragraph that could quietly go stale.

**The probe that would matter more, if the path exists (§3.0(d)):** a GIL-holding thread that
blocks on the *splitter mutex*. That is the §1 reproduction, and it is only production-relevant
once a real binding is found that logs without releasing the GIL. Until then it demonstrates the
hazard class, not the deployment.

______________________________________________________________________

## 4. Probe P3 — framework thread-safety, and the correction it forced

The working assumption was that `ConfigService`, `WorkspaceFactory`, `AlgorithmFactory` and
`DynamicFactory` "carry no synchronisation" and that this is what wedges the listener. Analysing
it properly — as required before putting it in front of reviewers — **that claim is substantially
narrower than stated.** Recording the corrected picture is more useful than the original
assertion.

### 4.1 What is actually synchronised

| Component                          | Synchronised? | Evidence                                                                 | On the listener's bg path?  |
| ---------------------------------- | ------------- | ------------------------------------------------------------------------ | --------------------------- |
| Poco config store (`m_pConf`)      | **yes**       | `Poco::Util::AbstractConfiguration.h:535` — `mutable Poco::Mutex _mutex` | yes → **safe**              |
| `ConfigServiceImpl` cached members | **no**        | zero sync in header or `.cpp`, narrow and broad patterns                 | **no** — see 4.2            |
| `Poco::NotificationCenter`         | **yes**       | `NotificationCenter.h:239` — `mutable RWLock _mutex`                     | yes → safe                  |
| `DataService<T>` (ADS, IDS)        | **yes**       | `DataService.h` — `std::recursive_mutex`                                 | yes → safe, modulo dispatch |
| `AlgorithmManager`                 | **yes**       | mutex present                                                            | yes → safe                  |
| **`DynamicFactory` registry**      | **no**        | `DynamicFactory.h` — `FactoryMap _map` with no locking at all            | **yes**                     |

So `ConfigService::getValue()` / `getString()` — what the listener actually calls — are safe,
because the property store beneath them is Poco-synchronised. The unsynchronised part is only
Mantid's own caches: `m_instrumentDirs`, `m_dataSearchDirs`, `m_proxyInfo` / `m_isProxySet`,
`m_changed_keys`.

### 4.2 The provable ConfigService defect — and why it is not this hang

There *is* a genuine, ASan-detectable defect:

- `getInstrumentDirectories()` (`ConfigService.cpp:1595`) returns **`const std::vector<std::string>&`**
  — a live reference to the member.
- `cacheInstrumentPaths()` (`:1628`) begins `m_instrumentDirs.clear()` and rebuilds by
  `push_back`, destroying every string and reallocating.
- It is reachable from public API: `setString("instrumentDefinition.directory", …)` (`:994`→`:1008`)
  and `updateConfig(…)` (`:642`→`:652`).
- `getInstrumentDirectory()` (`:1601`) returns `m_instrumentDirs.back()` — **UB on the empty
  vector** that `clear()` transiently leaves.

Callers holding that reference across a concurrent rebuild get a **use-after-free**:
`InstrumentFileFinder.cpp:113`, `LoadIDFFromNexus.cpp:139`, `SetSample.cpp:599`,
`ConfigService.cpp:1690` and `:2020`.

**But the listener never calls it.** Its background thread reaches ConfigService only via
`getVTPFileDirectory()` (`:1606`) and `getTempDir()` (`:1318`), both of which consult only the
synchronised store. And it cannot reach the instrument-directory path at all, because supplying
`InstrumentXML` short-circuits file resolution (`LoadInstrument.cpp:101-115`).

**Conclusion, stated plainly: this is a real framework defect worth an upstream story, and it is
not evidence for the live-reduce hang.** Conflating the two would be exactly the overreach that
gets a defect report dismissed.

### 4.3 `DynamicFactory` — the one unsynchronised service on the path

`DynamicFactory`'s `std::map` registry has no locking, and both `AlgorithmFactory::create` (via
`AlgorithmManager::createUnmanaged`, `:1401`) and `WorkspaceFactory::create` (`:1372`, `:1437`,
`:1492`, `:1493`) run on the listener's background thread.

**The honest qualification:** the race requires concurrent **mutation** — `subscribe` inserting or
`unsubscribe` erasing (`DynamicFactory.h:167`) against a lookup. Registration happens at plugin
load and Python-algorithm import, i.e. at startup. After that the map is stable, and concurrent
reads of a stable `std::map` are safe. So this is a latent defect that needs an unusual trigger
(runtime registration during a run), not a steady-state hazard.

### 4.4 Probe design: ASan, by hand

Given the above, P3 is a **by-hand diagnosis producing an artifact reviewers cannot wave away**,
not a regression test — which matches the expectation set for it.

**ASan rather than TSan is the right choice here**, and for a reason worth stating: TSan reports a
*race*, which a reviewer can characterise as benign; ASan reports a **use-after-free with a stack
trace naming `cacheInstrumentPaths` and the reading caller**, which is a memory-safety bug and
cannot be argued away. Run both if convenient — TSan proves the race exists even when it does not
manifest, ASan proves the consequence.

Scenario, ~30 lines and no listener involved:

- Thread A: `ConfigService::Instance().setString("instrumentDefinition.directory", alternating)` in
  a loop.
- Thread B: hold `const auto &dirs = ConfigService::Instance().getInstrumentDirectories();` and
  read through it in a loop (mirroring `InstrumentFileFinder.cpp:113`).
- Expect an ASan use-after-free on the vector's or a string's buffer.

**Build cost fits the stated budget.** This needs only `MantidKernel` compiled with ASan, not the
whole framework — `--preset linux-64-ci-address-sanitiser` (`CMakePresets.json:186`,
`USE_SANITIZER=Address`, `Sanitizers.cmake:74-75`) with a targeted `--target Kernel` plus the
probe. That is a fraction of a full ~2 hour build.

Note `Sanitizers.cmake:53` warns that ASan without debug info is much less useful, so build
`RelWithDebInfo` — which `sanitiser-default` already sets (`CMakePresets.json:182`).

______________________________________________________________________

## 5. What these probes prove, and what they do not

Worth stating plainly so the evidence is not overclaimed.

**They prove:** the lock cycles exist and are reachable; the listener's structure allows a
blocking framework call to wedge both threads; and after the refactor those properties no longer
hold. That is genuinely valuable — it is the difference between "we inspected the code and think
this is the problem" and "here is a test that fails before and passes after".

**They do not prove** that this mechanism caused the specific production incidents. That would
require the production configuration to route logging through a Python channel — still an open
question ([`deferred-action-refactor.md`](deferred-action-refactor.md) §8 question 1) — and
evidence from an actual occurrence, which Component 1 is designed to capture. A passing probe plus
an unconfirmed deployment configuration is strong circumstantial evidence, not proof.

**They do not establish** that the framework is thread-safe after the refactor. Variant B removes
the *listener's* contribution to concurrent framework access; it makes no claim about the
framework in general.

______________________________________________________________________

## 6. Escaping a triggered lock-up

**A deadlock cannot be escaped in-process.** Both participating threads are unrecoverable: you
cannot release another thread's mutex, and a signal handler cannot help because the handler runs
on some thread that still cannot break the cycle. The only exit is process termination.

Two consequences learned from building the prototype:

**The observer must not participate in the lock cycle.** A first attempt had the main thread hold
the GIL and block on the splitter mutex, which left nobody able to observe or terminate. The
working version puts the GIL holder in its own thread so the main thread stays free to watch and
call `_exit()`. Any probe of this kind needs a dedicated non-participating observer.

**Termination must be `_exit()`, not `exit()` or `abort()`.** `exit()` runs static destructors,
which may themselves try to take the locks that are deadlocked, hanging the teardown. `_exit()`
leaves immediately.

**Process isolation.** cxxtest runs the whole suite in one process, so a probe that genuinely
deadlocks would hang the entire test binary. P2 must therefore be a **standalone executable**
rather than a cxxtest case, with:

- the hazardous scenario in the process (or in a `fork()`ed child, so the parent can enforce the
  timeout without relying on the deadlocked child at all);
- a watchdog thread or the parent enforcing the deadline;
- `_exit(0)` for "deadlock reproduced", `_exit(1)` for "progress continued".

There is precedent for fork-based test isolation in the tree — the ADARA Python tests are skipped
on Windows for exactly that reason
(`test/python/mantid/LiveData/ADARA/utils/packet_playback/CMakeLists.txt:3`).

P1 needs none of this: it releases its own gate and never truly deadlocks, so it belongs in the
normal cxxtest suite.

______________________________________________________________________

## 7. Expectation inversion — one binary, two verdicts

Each probe takes a mode flag so the same executable serves before and after the refactor:

```
livedata-hazard-p1 --expect-block      # pre-refactor:  passes if the foreground blocks
livedata-hazard-p1 --expect-progress   # post-refactor: passes if it does not
```

This is what makes the probes regression tests rather than one-off demonstrations, and it dovetails
with the verification approach in [`deferred-action-refactor.md`](deferred-action-refactor.md)
§7b: build both branches, run the same probe against each, and the verdict flips.

CI runs only the `--expect-progress` mode after the refactor lands. The `--expect-block` mode is
retained for running against the pre-implementation branch and for documenting the original
hazard.

______________________________________________________________________

## 8. Build and test wiring

Optional, **default OFF**, as requested — these deliberately hang and are unsuitable for routine
CI.

```cmake
option(ENABLE_LIVEDATA_HAZARD_PROBES
       "Build the LiveData concurrency hazard probes (deliberately deadlock; not for routine CI)"
       OFF)
```

In `Framework/LiveData/test/CMakeLists.txt` (or a `probes/` subdirectory), guarded by that option:
standalone targets for P2/P3, plus P1 added to the existing cxxtest suite. Register with
`add_test` and an explicit `TIMEOUT` comfortably above each probe's internal deadline, so CTest
kills anything that escapes its own watchdog:

```cmake
add_test(NAME LiveDataHazardP2 COMMAND livedata-hazard-p2 --expect-progress)
set_tests_properties(LiveDataHazardP2 PROPERTIES TIMEOUT 60 LABELS "livedata;hazard")
```

**Note on the configuration rule.** Component 1 and Component 2 put every *runtime product* option
in `Mantid.properties`. This is deliberately different: `ENABLE_LIVEDATA_HAZARD_PROBES` governs
whether test targets are *built*, which is a build-system concern with no runtime meaning and no
place in a properties file. The probes' own behaviour is set by command-line flags (§7), not by
configuration, so a probe run is fully described by its command line.

P2 needs `MantidPythonInterfaceCore` and an initialised interpreter, so it must be skipped where
`ENABLE_WORKBENCH`/Python interface is off — the conda framework-only build
(`conda/recipes/mantid/build.sh` passes `-DMANTID_QT_LIB=OFF`) still builds the Python interface,
but the guard should be explicit rather than assumed.

______________________________________________________________________

## 9. Framework-level defects the probes would justify

The refactor in [`deferred-action-refactor.md`](deferred-action-refactor.md) makes the *listener*
stop provoking these hazards. It does not fix them. Every other component that touches the
framework from a thread the framework did not create remains exposed — other live listeners,
algorithms using their own threads, and anything running under a Python-hosted process.

A reproducible probe changes what can be asked for upstream. "We think this is racy" is a code
review comment; "here is a test that deadlocks against the real classes" is a defect report. The
candidates, in the order the evidence supports them:

### F1 — Python log channels call into Python while holding the channel mutex

**Exposure confirmed in the deployed configuration; a closing cycle is not.** Be precise here —
this is the claim most likely to be scrutinised. `SplitterChannel` holds `_mutex` across dispatch
(measured, §1), and `PythonLoggingChannel.cpp:70-76` acquires the GIL *and calls arbitrary Python
code* while that mutex is held. That is a genuine lock-layering defect. It does **not** by itself
deadlock against Python's logging handler lock, because CPython releases the GIL while blocking on
a Python-level lock (measured, §3.0(b)) — an earlier revision claimed otherwise and was wrong.

This affects **every multithreaded Mantid code path in a Python-hosted process with a Python log
channel configured** — the listener is merely one caller, and `mantid.utils.logging.log_to_python`
is a public, documented API that any script may call. `PythonStdoutChannel.h:37` already carries a
TODO acknowledging weak GIL management.

The defect is not "acquiring the GIL" per se — it is **calling arbitrary Python code (which takes
its own locks) from inside a Poco channel lock**. Any user-supplied logging handler can introduce
a new lock into the cycle.

Candidate fixes, in increasing invasiveness:

- **Wrap the Python channel in `Poco::AsyncChannel`.** `AsyncChannel::log()` enqueues and returns,
  so the GIL is acquired later on the async channel's own thread — after the splitter mutex has
  been released. The cycle is broken without touching the GIL logic at all, and it may be
  achievable as a *configuration* change in `Mantid.properties.template`. **This is the most
  promising option and P2 is exactly the harness to evaluate it**: run the probe with and without
  the `AsyncChannel` wrapper and see whether the deadlock survives.
- **Provide a service-only, Python-free console channel** and configure the reduction service to
  use it. The service's stdout already reaches journald, so it gains nothing from routing log
  output through Python — plain `StdoutChannel` removes the GIL from the logging path entirely in
  the deployment that matters. This is the smallest available mitigation, needs no framework
  change at all, and can be applied to the live-reduce unit immediately as a **test of the
  hypothesis in production**: if the hang stops recurring after the channel switch, that is strong
  evidence.
- Restructure the channel so the GIL is acquired before entering the channel chain.
- Replace the channel per the existing TODO, with GIL handling that cannot nest under a channel
  lock.

### F2 — `ConfigService` returns a reference to a cache it rebuilds

**Corrected from "ConfigService has no synchronization".** The Poco property store beneath it *is*
synchronised (`AbstractConfiguration.h:535`), so ordinary `getValue()`/`getString()` reads are
safe. The defect is narrower and sharper: `getInstrumentDirectories()` (`:1595`) hands out a live
`const std::vector<std::string>&`, while `cacheInstrumentPaths()` (`:1628`) `clear()`s and rebuilds
that vector, reachable from `setString("instrumentDefinition.directory", …)` and `updateConfig()`.
Concurrent use is a **use-after-free**; `getInstrumentDirectory()` (`:1601`) additionally does
`.back()` on the transiently empty vector.

Fix: return by value, or guard the caches with a `shared_mutex`. Evidence: probe P3 under ASan
(§4.4). **Not on the listener's path** — see §4.2. This is an upstream defect in its own right,
not evidence for our hang.

### F3 — `DynamicFactory`'s registry has no synchronization

Measured: `FactoryMap _map` with no locking whatsoever, and it underlies both `WorkspaceFactory`
and `AlgorithmFactory` — both of which the listener's background thread calls. **The honest
qualification:** the race needs concurrent mutation (`subscribe`/`unsubscribe` at
`DynamicFactory.h:167`) against a lookup, and registration is a startup activity. Stable-map
concurrent reads are safe, so this needs runtime registration to trigger. Fix: a `shared_mutex`
over the registry. Evidence: probe P3 under TSan, with a deliberately concurrent `subscribe`.

### F4 — `DataService` posts notifications inconsistently with respect to its own lock

`DataService.h` posts some notifications **while holding** its `recursive_mutex` (`:196`→`:209`,
`:225`→`:233`/`:239`) and others after releasing it (`:269`, `:272`, `:331`, `:342`). Observers
therefore run on whichever thread posted, sometimes under the service lock and sometimes not. An
observer that takes any other lock has an inversion available to it, and the inconsistency is
itself a smell. Fix: post uniformly outside the lock, and state the thread-safety contract
observers must meet.

### F5 — `LiveListener::extractData()` is not exception-safe

`Framework/API/src/LiveListener.cpp:16-21`. Detailed in
[`deferred-action-refactor.md`](deferred-action-refactor.md) §6. A base-class contract defect
affecting every listener, not just the SNS one, and the narrow listener-local fix leaves it in
place for ISIS, Kafka and any future implementation.

### Sequencing, revised after §4's analysis

**F1 first, and it is stronger than "convenient".** It is the only mechanism identified so far
that requires *nothing unusual to happen* — no runtime registration, no configuration mutation,
just two threads logging concurrently. That is why it reproduced on the first attempt, and it is
why the others, despite being real defects, are worse candidates for *this* hang: F2 needs a
config write during a run, F3 needs a runtime algorithm registration, F4 needs an observer that
takes another lock. Each of those is an additional coincidence; F1 needs none.

It is also a genuine framework defect rather than purely a deployment choice: shipping a channel
that acquires the GIL beneath a Poco channel mutex is a lock-order bug whoever configures it.
`PythonStdoutChannel.h:37` already concedes the GIL handling is inadequate.

F2 and F3 are worth raising on their own merits with ASan/TSan evidence, but **should be filed as
latent defects, not as explanations of the live-reduce hang** — §4.2 and §4.3 show neither is
exercised unsafely by the listener's steady-state access pattern. Filing them as the cause would
invite a well-founded rejection and damage the credibility of the rest. F4 and F5 are small and
independent.

Each is a separate upstream story with its own owner and blast radius, so they should not be
bundled. This document's contribution is the evidence, and the discipline of not claiming more
than the evidence supports.

______________________________________________________________________

## 10. Open questions

1. Should P1 live in the existing `SNSLiveEventDataListenerTest.h` or a new
   `SNSLiveEventDataListenerHazardTest.h`? It needs no isolation, so the existing suite is viable,
   but a separate fixture keeps intent obvious.
1. Does the in-tree P2, using the **real** `PythonStdoutChannel`, still deadlock — or does that
   channel's `ConsoleChannel` base introduce buffering that changes when the GIL is taken? The
   prototype used a minimal stand-in; this is the one substitution that needs re-verifying against
   the production class.
1. Is a probe warranted for the `m_pauseNetRead` exception-safety defect
   ([`deferred-action-refactor.md`](deferred-action-refactor.md) §6)? It is deterministic and easy
   to write, but it is a stranding bug rather than a lock-up, so it may belong as an ordinary unit
   test instead.
1. Should the probes run in a nightly job with `--expect-progress` once the refactor lands, or only
   on demand? Nightly gives standing regression cover for the invariant; on-demand keeps
   deliberately-hanging tests out of automation entirely.
1. Does `PyGILState_Ensure()` from a `Poco::Thread` that Python has never seen behave identically
   in the probe and in the listener? It creates a thread state in both cases, but the probe should
   assert that rather than assume it, since it is the crux of the "unknown thread" framing.
1. Does the FIFO trick in §2.1 actually block? Needs `IDFObject::exists()` to treat a FIFO as
   existing and `applyCache()` to open it for read. If it stats or checks the file type first, a
   different real-blocking mechanism is needed for the non-vague P1.
1. **Is the live-reduce unit configured with a Python log channel?** Still the single
   highest-value question across all four documents — it decides whether F1 is the live mechanism
   or merely a latent hazard, and the answer costs one look at the deployed properties.
1. Would switching the service to a Python-free console channel be acceptable as a production
   experiment? If the hang stops recurring, that is the strongest evidence obtainable without
   reproducing it — and it is a configuration change, not a code change.
1. Does any part of live reduction register algorithms *at runtime* rather than at import? That is
   the trigger F3 needs, and if the answer is no, F3 drops out as a candidate for this hang
   entirely.
