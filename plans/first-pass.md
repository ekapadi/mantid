# SNSLiveEventDataListener rare-hang investigation — design notes

> **Status: design / discussion. Not ready to implement.**
> Working document — amend in place as the discussion continues. Split into additional files
> under `plans/` if this outgrows one document.

______________________________________________________________________

## 1. Context

`SNSLiveEventDataListener` hangs **rarely** on the target deployment (an AlmaLinux-family VM).
The recent refactoring existed largely to simplify the code enough to make this hang findable
deterministically, or to design it out.

The investigation started as "how do we get a stack trace out of a hung process without
`ptrace`". It detoured through two hypotheses that observation has since killed (§5), and
arrived at a mechanism that fits the observed symptom (§3) — for which a stack trace turns out
to be the right instrument after all.

### Observed symptom (user-reported, decisive)

- Hang is **rare**.
- **No error reported** by the foreground thread — no throw of any kind is observed.
- **No `NotYet` retry warnings** in the log.
- Process remains alive; live view frozen.
- **It never self-heals.** Not once.

Any candidate mechanism must produce **silence** and be **permanent**. Together these eliminate
most hypotheses immediately — including every "slow I/O that eventually completes" reading. What
remains is a call that never returns, or a genuine deadlock.

> **Direction:** a live occurrence will probably never be available to inspect, so the on-demand
> diagnostics below (§6, §7) are of limited practical value. The work worth doing is split across
> two documents:
>
> - **[`keepalive-watchdog.md`](keepalive-watchdog.md)** — detect it from **outside** the process,
>   via a shared-memory keep-alive and a watchdog the listener launches. Independent of the fix,
>   and worth shipping first: it works whatever the cause turns out to be.
> - **[`deferred-action-refactor.md`](deferred-action-refactor.md)** — make the bug class
>   impossible by construction, plus the separate `m_pauseNetRead` exception-safety defect.
> - **[`hazard-probes.md`](hazard-probes.md)** — optional tests that deliberately trigger these
>   lock-ups, turning the hypotheses into executable evidence. Also enumerates the framework-level
>   synchronisation defects those probes would evidence for upstream stories.
>
> **Read §5a (field evidence) and §5c (the `pystack core` procedure) first.** §5c is the single
> highest-value action available: it converts the next occurrence into a complete answer, and the
> only preparation needed is enabling core dumps on the unit.
>
> §6 and §7 below are retained as analysis and as a record of what was considered.

### Hard constraints

| Constraint                             | Consequence                                                                                                                                                                                    | Evidence                                                                                    |
| -------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `ptrace` hard-blocked on target        | No `gdb`/`pstack`/`eu-stack` attach. `prctl(PR_SET_PTRACER)` does **not** help — it defeats yama only, not seccomp/SELinux                                                                     | user-confirmed                                                                              |
| C++20, not C++23                       | No `<stacktrace>`                                                                                                                                                                              | `buildconfig/CMake/CommonSetup.cmake:149`                                                   |
| `CMAKE_CXX_VISIBILITY_PRESET = hidden` | `dladdr` reads `.dynsym` only; most internal frames resolve to no name                                                                                                                         | `buildconfig/CMake/CommonSetup.cmake:194`                                                   |
| Deployed build Release + stripped      | Offline `addr2line` yields nothing unless debug info is deliberately retained                                                                                                                  | `conda/recipes/mantid/build.sh` sets no build type; conda-build supplies Release and strips |
| **The hang is rare**                   | Compile-time-OFF instrumentation will be off when it happens; manual `kill`-triggering needs someone at a shell mid-hang. Anything built must be **always-on** and ideally **self-triggering** | user-confirmed                                                                              |

______________________________________________________________________

## 2. Corrections to earlier analysis in this document

Recorded explicitly because both errors redirected the investigation.

**"A stack dump is the wrong instrument for this bug class."** Wrong. Under §3 the background
thread is wedged inside framework code, which a stack trace names directly and a
listener-predicate dump does not. The original instinct — get a stack trace — was sound.

**"There is exactly one mutex, so lock-order inversion is impossible" — then withdrawing it.**
Both the claim and its withdrawal were muddled; the net position is:

- The original claim is *literally correct*: `m_mutex` is the only lock in the class, it is
  non-recursive and never nested (`SNSLiveEventDataListener.h:266-269`), so no AB-BA inversion is
  possible within the class.
- What it wrongly *implied* is that no lock-related hang is possible. **§3 needs no second lock
  at all** — one thread holding `m_mutex` across an unbounded blocking call is sufficient. That
  is a different failure mode entirely, and the single-mutex argument says nothing about it.
- A genuine two-lock inversion would require some thread to hold a framework lock and *then*
  need `m_mutex`. **No such path has been demonstrated.** The candidates checked do not overlap:
  `doExtractData()` acquires and releases `ConfigService` at `:1555` *before* taking `m_mutex` at
  `:1597`; the logger is taken and released inside the `m_mutex` region, same order on both
  threads. Treat AB-BA as unsupported, not as a live hypothesis.

**Consequence: TSan / Helgrind do not help here.** They detect lock-order inversions and data
races. §3 is neither — there is no inversion and no race, just a lock held across a syscall that
does not return. An earlier revision of this document reinstated them on the strength of the
muddled reasoning above; that advice is withdrawn. (They remain independently useful for finding
*races*, which is a separate concern from this hang.)

______________________________________________________________________

## 2c. SECONDARY HYPOTHESIS — memory-checker restart deadlocks on the GIL

> **Demoted by the field evidence in §5a.** The 2026-08-27 occurrence hung **17 seconds** after
> start at **778 MB**, so memory pressure cannot have been the trigger. This mechanism remains
> real, fully evidenced in code, and a plausible cause of *some* hangs after long uptime — the
> user's own view is that there are probably several distinct hangs — but it is **not** the one
> seen shortly after startup. §5b is the leading hypothesis.

> **Complete mechanism, every element in deployed code, and it predicts a log signature that
> should already exist in `livereduce.log`.** Detail: [`hazard-probes.md`](hazard-probes.md) §3.0b.

### The trigger is automatic, not operator-initiated

`livereduce.py` starts a **memory-checker thread** (`:434-437`), enabled by default
(`system_mem_limit_perc` defaults to **70**% of system RAM; `0` disables) and polling every
**1 second** (`mem_check_interval_sec`):

```python
def memory_checker(config, livemanager):  # :396
    while True:
        mem_used = config.proc_pid.memory_info().rss
        if mem_used > config.mem_limit:
            logger.error(f"Memory usage {...} MB exceeds limit")  # :400
            livemanager.restart_and_clear()  # :401
        time.sleep(config.mem_check_interval_sec)
```

`restart_and_clear()` → `stop()` → `mantid.AlgorithmManager.shutdown()` (`:85`).

**This is why it does not look like a shutdown hang.** Nobody shuts it down. The service runs for
days, RSS grows as workspaces accumulate, crosses 70%, and the memory checker initiates a restart
by itself — which deadlocks.

### The deadlock

| Thread                                                 | Holds                                                                     | Waits for                                                               |
| ------------------------------------------------------ | ------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| **T4** memory-checker (Python thread)                  | the **GIL** — `AlgorithmManager.shutdown()`'s binding does not release it | `runningInstances()` to empty; spin-waits at `AlgorithmManager.cpp:188` |
| **T2** MonitorLiveData async (`StartLiveData.cpp:230`) | its slot in `runningInstances()`                                          | the **GIL**, to run `RunPythonScript`                                   |
| **T3** listener background                             | `SplitterChannel::_mutex`                                                 | the **GIL**, via `PythonLoggingChannel` on every `g_log`                |

`AlgorithmManager.cpp` (Python export) `:56-67` has the GIL release **commented out**, with
`/// TODO We should release the GIL here otherwise we risk deadlock (see issue #33895)` — reverted
because it exposed #33924. `AlgorithmManagerImpl::shutdown()` (`:186-192`) then spin-waits.

T4 cannot finish until T2 leaves `runningInstances()`; T2 cannot proceed without the GIL; T4 holds
it. **Circular, permanent.**

### Fits every constraint in §1

| Constraint                      | Why                                                                                                                                                                         |
| ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Runs for days, then hangs**   | Memory accumulation is the clock. Nothing else in the system has a multi-day time constant.                                                                                 |
| Not operator-initiated          | The memory checker calls the restart itself.                                                                                                                                |
| Silent                          | T4 is in a C++ `sleep_for` loop; T2/T3 cannot log without the GIL. `log_to_python` (`:29`) makes *every* Mantid log call GIL-dependent, so even "cancelled" messages stall. |
| Never self-heals                | Circular wait around a spin loop.                                                                                                                                           |
| Process alive, `SIGKILL` needed | T4 spins at 100 ms; nothing is in `D` state.                                                                                                                                |
| Predates the listener refactor  | `m_mutex` is nowhere in the cycle.                                                                                                                                          |
| Restarted by the watchdog       | Exactly what `livereduce_watchdog.service` exists for.                                                                                                                      |

### THE CHECK — one grep, on logs that already exist

`logger.error("Memory usage … exceeds limit")` at `:400` is a **plain Python logging call**, emitted
*before* `restart_and_clear()`. It does not need the GIL from a blocked thread, so **it should be
written successfully.**

> **If `livereduce.log` shows `"Memory usage … exceeds limit"` as the last message before each
> hang, this hypothesis is confirmed.**

```
grep -n "exceeds limit" /var/log/SNS_applications/livereduce.log*
```

Then correlate those timestamps with the observed hangs and watchdog restarts. This costs one
command against data already on disk — no reproduction, no instrumentation, no waiting.

### Mitigations

1. **Set `system_mem_limit_perc = 0`** in `/etc/livereduce.conf` to disable the memory checker.
   Diagnostic first step: if the hangs stop, confirmed. Trade-off — unbounded memory growth
   returns, so pair it with monitoring.
1. **Replace `shutdown()` with GIL-releasing calls.** `cancelAll()` (`:71`) and
   `runningInstancesOf()` (`:69`) both release the GIL. Cancel, then poll from Python with
   `time.sleep()` between attempts and a bounded timeout. **livereduce-side, no framework change.**
1. **Drop `log_to_python`** so Mantid logging does not need the GIL, letting cancelled algorithms
   log their way out.
1. **Upstream:** release the GIL in `shutdown()`/`clear()` — issue #33895, blocked on #33924.

______________________________________________________________________

## 3. Secondary hypothesis: blocking framework call under the parse lock

**Demoted by §2c, but still a genuine defect worth removing.** Fits the observed
symptoms and is well supported by the code; §2c simply fits better and has its risk acknowledged
upstream. The refactor that fixes this also removes background-thread logging, which shrinks §2c's
exposure as a side effect.

`initWorkspacePart2()` (`:1398`) runs a full `LoadInstrument` algorithm at `:1416`. The code
states its own locking context at `:1419-1421`:

```
// Do NOT acquire m_mutex here — the caller always holds it
// (guaranteed by the outer lock around bufferParse() in run()) ...
```

It is reached from seven `rxPacket()` sites: `:541, :738, :761, :911, :972, :1016, :1063` — all
under the parse lock acquired once in `run()` at `:445`, held for the whole parse iteration
(`:427-445`).

`loadInst->execute()` performs XML SAX parsing, `InstrumentDataService` singleton access,
`ConfigService` lookups, geometry-cache file I/O, and logging.

### Correction: the disk I/O is the VTP cache, not the IDF directory

An earlier revision of this document blamed a network-mounted `instrumentDefinition.directory`.
**That path is not taken.** The listener supplies `InstrumentXML` (from the ADARA `GeometryPkt`)
at `:1404`, and `LoadInstrument.cpp:101-115` short-circuits on it: when `InstrumentXML` is
non-default it sets `loader_type = LoaderType::Xml`, uses `filename = instname` as a mere label,
and at `:176` constructs `InstrumentDefinitionParser(filename, instname, InstrumentXML->value())`
— parsing the XML string that arrived over the wire. **No IDF file is read.**

The disk I/O is real but lives elsewhere — the geometry cache, in
`InstrumentDefinitionParser.cpp:2570-2590`:

- `:2584` `m_cacheFile->exists() && applyCache(m_cacheFile)` — reads a `.vtp` cache
- `:2589` `writeAndApplyCache(...)` — **writes** it
- `:2579` / `:2990` — path from `ConfigService::getTempDir()` or `getVTPFileDirectory()`

So the check to run on the target is against **`getVTPFileDirectory()` / the temp dir**, not the
instrument-definition directory. A cache *write* stalling is at least as plausible as a read, and
it happens under the parse lock.

### Why it is silent

| Symptom       | Why                                                                                                                                                 |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| No exception  | The `try/catch` at `:1415` only fires if `execute()` *returns*. A blocked call never does.                                                          |
| No `NotYet`   | The foreground never reaches the gate — it is blocked on `m_mutex` at `:1597` in `doExtractData()`.                                                 |
| No log        | Neither thread advances far enough to log.                                                                                                          |
| Process alive | Both threads are blocked, not crashed. Teardown may also stall (dtor needs the bg thread to observe `m_stopThread`, which it cannot while blocked). |

### The mechanism needs only ONE lock

Worth stating explicitly, because an earlier revision of this document got it wrong. There is no
second lock and no inversion. A stalled `read()` acquires nothing — it simply does not return.
The background thread holds `m_mutex` while it does not return; the foreground blocks on
`m_mutex`. That alone freezes both threads. "Hold a lock across an unbounded operation" is the
defect, not "deadlock".

### Why it would be rare — and the discriminating check

The most plausible blocker on a beamline VM is filesystem I/O on the **VTP geometry-cache
directory** (see the correction above) if it resolves to an NFS or other network-mounted path.
Such a stall is untimed, silent and indefinite, and would never reproduce against a local disk in
testing. Other candidates: contention on `InstrumentDataService`, or `ConfigService` I/O.

**Permanence is the discriminator.** Hold-across-I/O only freezes *permanently* if the syscall
never returns. If it eventually completes, the symptom is a long stall that self-heals, not a
dead session. NFS **hard** mounts block indefinitely against a dead server; **soft** mounts time
out and return an error. So:

- Session never recovers → hard mount fits; soft mount does not.
- Session eventually recovers on its own → consistent with either, and with slow I/O generally.

### Checks that would confirm or kill it cheaply

1. Is the **VTP cache directory** (`ConfigService::getVTPFileDirectory()`, else the temp dir) on
   a network mount, and with what options — `hard` or `soft`? `findmnt -T <dir>` settles both in
   seconds. This is discriminating, not merely suggestive. Note this is *not*
   `instrumentDefinition.directory`, which is not read on this path.
1. During an occurrence: `/proc/<pid>/task/*/status` — a thread in state `D` (uninterruptible
   sleep) is the signature of blocked I/O and needs no `ptrace`.
1. Did an occurrence coincide with storage trouble — NFS server, mount, or network?
1. A stack trace from a live occurrence would name the blocking frame outright. This is the
   scenario that justifies the tooling in §7.

### Structural fix direction

Do not hold the parse lock across a Mantid algorithm run or any I/O. Options: hoist workspace
initialisation out of the `rxPacket()` path; run `LoadInstrument` before taking the lock and
publish the result under it; or narrow the lock so it covers only the shared-state mutation. This
matches the refactoring's stated goal — design the hazard out rather than instrument around it.

Independently worth bounding: `LoadInstrument`'s I/O has no timeout, so nothing recovers it.

______________________________________________________________________

## 4. Secondary structural finding: `extractData()` is not exception-safe

Latent regardless of whether it is the observed hang; the symptom evidence says it is not (§5).

`Framework/API/src/LiveListener.cpp:16-21` — no try/catch, no RAII guard:

```cpp
std::shared_ptr<Workspace> LiveListener::extractData() {
  onBeforeExtract();
  auto workspace = doExtractData();   // if this throws...
  onAfterExtract();                   // ...this never runs
  return workspace;
}
```

`m_pauseNetRead` is set at `:813` / `:888` and cleared at exactly two sites, asymmetrically:

| Edge     | Clear site                      | Runs                       | Throw-safe? |
| -------- | ------------------------------- | -------------------------- | ----------- |
| BeginRun | `:1739`, in `onBeforeExtract()` | *before* `doExtractData()` | yes         |
| EndRun   | `:1761`, in `onAfterExtract()`  | *after* `doExtractData()`  | **no**      |

So a throw from `doExtractData()` with an EndRun pending strands `m_pauseNetRead = true`, parking
the background thread in the unbounded wait at `:325`. Persistent variant: `m_backgroundException`
is first-writer-wins and never cleared, so once set, `:1598` throws on every subsequent call.

**Worth fixing on its own merits.** Base-class fix (RAII guard, or a distinct `onExtractFailed()`)
affects every listener; a narrower fix clears the EndRun pause on the same side as BeginRun.

**Status: eliminated as the observed hang, by the absence of any throw.** That is the whole
argument; nothing further is needed.

*Footnote, for completeness only:* `MonitorLiveData.cpp:107-111` holds the listener in a local so
it "will destruct if the algorithm throws". An earlier revision presented this as *additional*
evidence for the elimination — it is not. The antecedent (a throw) never occurs, so the fact does
no discriminating work. Recorded because it is true and useful to know, not because it supports
any conclusion here.

______________________________________________________________________

## 5. Hypotheses eliminated by observation — WITH A RETRACTION

> **RETRACTION.** The `NotYet` elimination below was **invalid**. `MonitorLiveData.cpp:167` sets
> `loadAlg->setLogging(false)` with the comment `// Too much logging`, and `LoadLiveData` has no
> file-static logger — its `g_log` is the inherited `Algorithm::g_log`, a reference to the
> per-instance `m_log` (`Algorithm.cpp:112`). **So `LoadLiveData`'s `NotYet` retry warnings at
> `:488-489` never reach the log at all.** Their absence proves nothing, and the "`NotYet` forever"
> mechanism is back in contention — see §5b.

| Hypothesis                                                                                   | Prediction                                                                                       | Observed                    | Verdict                                                                                 |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ | --------------------------- | --------------------------------------------------------------------------------------- |
| Stranded `m_pauseNetRead` after a throw (§4)                                                 | Requires a throw from `doExtractData()`; would surface as a failed algorithm with a logged error | No throw, no error reported | **eliminated** — the required antecedent never occurs                                   |
| ~~`onEndRun()` resets `m_bgThreadCaughtUp`; quiet stream leaves it false; `NotYet` forever~~ | ~~Two `g_log.warning()` lines every ~10 s~~                                                      | ~~No `NotYet` warnings~~    | **ELIMINATION RETRACTED** — those warnings are suppressed by `setLogging(false)`        |
| Classic AB-BA deadlock on `m_mutex` alone                                                    | —                                                                                                | —                           | impossible: one mutex, non-recursive, no nesting (`SNSLiveEventDataListener.h:266-269`) |

______________________________________________________________________

## 5a. FIELD EVIDENCE — bl4a-livereduce, 2026-08-27

First real artifact from a live occurrence. `systemctl status livereduce`:

```
Active: active (running) since Thu 2026-08-27 06:36:06 EDT; 56min ago
Tasks: 22 (limit: 151862)
Memory: 778.7M (peak: 780.7M)
CPU: 19.598s
  1191290 /usr/bin/bash /usr/bin/livereduce.sh
  1191301 pixi run --frozen --manifest-path /usr/local/pixi/mr_reduction_qa python /usr/bin/livereduce.py
  1191315 .../bin/python /usr/bin/livereduce.py
```

Last journal lines, all at 06:36:23:

```
SNSLiveEventDataListener-[Information] Run paused
SNSLiveEventDataListener-[Information] Annotation: Run 47210 Paused.
Python-[Warning] Run 47210 [Off_Off]: Could not find direct beam with matching slit, trying with wl only
SNSLiveEventDataListener-[Information] Run paused
SNSLiveEventDataListener-[Information] Annotation: [NEW RUN FILE CONTINUATION] Run 47210 Paused.
SNSLiveEventDataListener-[Information] Run resumed
SNSLiveEventDataListener-[Information] Annotation: Run 47210 Resumed.
SNSLiveEventDataListener-[Information] Scan Start: 249
SNSLiveEventDataListener-[Information] Annotation: [Run 47210] Scan #249 Started.
Python-[Notice] Run 47210 [Off_Off]: Direct beam run: 47191          <-- LAST
```

### What this settles

| Finding                                                                                             | Consequence                                                                                                                                                                         |
| --------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Hung 17 s after start** (06:36:06 → 06:36:23) at **778 MB**                                       | **§2c is ruled out.** Memory pressure is impossible at this uptime and footprint.                                                                                                   |
| **CPU 19.598 s over 56 min**, essentially all in the first 17 s                                     | Nothing is spinning afterwards — every thread is *blocked*, not looping.                                                                                                            |
| **T2 and T3 logging concurrently in the same second**                                               | Exactly the §5b scenario: concurrent logging from the reduction script and the listener background thread.                                                                          |
| Listener's last messages are `Annotation:` / `Scan Start:` — `rxPacket(AnnotationPkt)` `:1239-1271` | **T3 was inside `bufferParse()`, holding `m_mutex`,** when it stopped. So `m_bgThreadCaughtUp == false`, and the foreground's `extractData()` would throw `NotYet` — silently (§5). |
| **Last message of all is from the reduction script** (T2)                                           | T2 was alive and logging immediately before the stall.                                                                                                                              |
| `pixi ... mr_reduction_qa`                                                                          | The instrument script is **`mr_reduction`** (BL4A, Magnetism Reflectometer) — the missing artifact now has a name.                                                                  |
| 22 tasks                                                                                            | Thread count for a future `/proc/<pid>/task/*/status` capture.                                                                                                                      |

### Both log streams go through `PythonLoggingChannel`

`Python-[...]` is Mantid's logger named "Python", i.e. the script logs via `mantid.kernel.logger`.
So *every* message from *every* thread traverses **splitter mutex → GIL → Python handler lock**,
in that order. Consistent ordering across all threads means **logging alone cannot invert** — which
is consistent with §3.0's retraction and means the missing half is still missing.

### The strongest remaining candidate for the missing half

A thread holding the GIL that then blocks on a **C-level** lock. `Logger.cpp:36-61` releases the
GIL, so the script's own logging is not it. But **`ConfigService.__setitem__` does *not* release
the GIL** (`Exports/ConfigService.cpp:133,169` bind `setString` directly), and
`ConfigServiceImpl::setString` calls `configureLogging()` for logging keys (`:1012`) and always
posts a notification. So:

- **T2**: Python script → `config[...] = ...` → **holds GIL** → Poco logging reconfiguration →
  wants a C-level logging lock
- **T3**: holds splitter mutex (mid-`Annotation` log, under `m_mutex`) → wants the **GIL**

→ deadlock, with `m_bgThreadCaughtUp` stuck false. **Does `mr_reduction` set any ConfigService
value during processing?** That single question may close the case.

### Asks, in order of value

1. **The `mr_reduction` script** (`/usr/local/pixi/mr_reduction_qa`). Specifically: any
   `ConfigService[...] = ...` / `config.setString(...)`, any custom Python logging handler, and
   anything that can block indefinitely.
1. **Next occurrence, while still hung** — the process stays alive, so this costs nothing and needs
   no `ptrace`:
   ```
   for t in /proc/<pid>/task/*; do echo "$t $(awk '/^State:/{print $2}' $t/status) $(cat $t/comm)"; done
   ```
   Thread states discriminate blocked-on-futex (`S`) from uninterruptible I/O (`D`) from spinning
   (`R`), and `comm` names which thread is which.
1. **Is `/var/log/SNS_applications/` on a network mount?** A stalled log write holds the Python
   handler lock while the splitter mutex is held by another thread — which wedges *all* logging
   process-wide, and hence the listener, since it logs under `m_mutex`.

______________________________________________________________________

## 5b. LEADING HYPOTHESIS — mid-chunk stall with concurrent logging

**Promoted to leading by §5a.** §2c requires memory to cross a threshold, which the field
evidence excludes. This mechanism needs no memory pressure, fits a 17-second time-to-hang, and is
what the field log's concurrent T2/T3 logging shows directly.

`m_bgThreadCaughtUp` is set **false** at `bufferParse()` entry (`:424`) and **true** only at exit
(`:451`). The refactor moved logging *inside* that window and *under* `m_mutex`. So:

```
T2: processChunk -> RunPythonScript -> holds the GIL -> script blocks, or simply runs long
T3: bufferParse -> g_log -> PythonLoggingChannel -> needs the GIL -> BLOCKED,
                            holding m_mutex, with m_bgThreadCaughtUp == false
```

T2 never returns from the script, so it never re-enters `extractData()`; T3 cannot complete its
parse. If the reduction script blocks indefinitely, the stall is **permanent**.

**Why it is totally silent** — three independent gags, which is why this was so hard to see:

1. `LoadLiveData` logging is disabled (`MonitorLiveData.cpp:167`), so `NotYet` retries are mute.
1. T3 cannot log at all — logging *is* the thing needing the GIL.
1. `MonitorLiveData` is blocked inside `executeAsChildAlg()`, before its next message.

**A refactor-introduced worsening, without the refactor being the cause.** Pre-refactor the
background thread logged *outside* `m_mutex` (log-then-lock, e.g. `:405`/`:408` in the old file),
so a GIL stall there did not hold the parse lock and did not necessarily coincide with
`m_bgThreadCaughtUp == false`. Post-refactor a GIL stall on T3 translates directly into foreground
`NotYet`. The hang predates the refactor, so this is escalation, not causation — but it predicts
the symptom should have become more frequent or more total afterwards.

### THE SECOND CHECK — also one grep on existing logs

`MonitorLiveData.cpp:153` logs `"Loading live data chunk N at HH:MM:SS"` at notice level, and
**MonitorLiveData's** logging is *not* disabled.

```
grep -n "Loading live data chunk" /var/log/SNS_applications/livereduce.log*
```

Together with §2c's check, this discriminates the two mechanisms from data already on disk:

| Last line before the hang                      | Mechanism                                                                   |
| ---------------------------------------------- | --------------------------------------------------------------------------- |
| `Memory usage … exceeds limit`                 | §2c — memory-checker restart deadlocks on the GIL                           |
| `Loading live data chunk N`, no memory message | §5b — mid-chunk stall: script holding the GIL, or T3 wedged under `m_mutex` |
| Neither                                        | Something else; re-open §3                                                  |

### What is still missing

A **permanent** GIL holder. If the script simply finishes, T2 releases the GIL and T3 proceeds.
So the permanent form requires the reduction script itself to block indefinitely — plausible, and
it would be **instrument-specific**, which fits the not-yet-available instrument script and any
site-to-site variation. Two things to obtain:

1. **The instrument-specific processing script.** Anything in it that blocks — a network read, a
   file on a stalled mount, a wait on an external service — completes this mechanism.
1. **Whether the Python logging handlers in that script call back into Mantid.** With
   `PythonLoggingChannel` holding a *non-recursive* `Poco::FastMutex` while calling Python, a
   handler that logs through Mantid self-deadlocks that thread instantly.

______________________________________________________________________

## 5c. THE DIAGNOSTIC THAT RESOLVES THIS — `kill -ABRT` + `pystack core`

**This supersedes essentially all the instrumentation designed in §7 and most of §6, and it needs
no `ptrace`.**

`pystack` (already checked out at `~/workspaces/pystack`, and installed in the dev env) has two
modes: `remote <pid>`, which needs `ptrace` and is therefore blocked on the target, and
**`core <corefile>`, which needs only a core file**. Core dumps are written by the *kernel* on a
fatal signal — `ptrace` is not involved.

Crucially, `pystack core` offers:

```
--native-all   Include native (C) frames from threads not registered
               with the interpreter (implies --native)
```

**"Threads not registered with the interpreter" is exactly the listener's `Poco::Thread`** — the
thread no other component knows about, whose stack is the single most valuable unknown in this
investigation. `--locals` additionally dumps local variables per frame.

### The procedure, on the next occurrence

```bash
# 1. while the process is still hung -- kernel writes the core, no ptrace
kill -ABRT <pid>

# 2. retrieve it (systemd-coredump) or find it via core_pattern
coredumpctl dump <pid> > /tmp/livereduce.core

# 3. full Python + native stacks for ALL threads, including the listener bg thread
pystack core /tmp/livereduce.core \
    /usr/local/pixi/mr_reduction_qa/.pixi/envs/default/bin/python \
    --native-all --locals \
    --lib-search-root /usr/local/pixi/mr_reduction_qa/.pixi/envs/default
```

The `--lib-search-root` matters because the process runs under `pixi`, so its shared libraries
live in the environment prefix rather than a system path.

### What it would answer immediately

- **Which thread holds the GIL**, and what it is doing.
- **Where T3 is blocked** — `PyGILState_Ensure`, the splitter mutex, or something else entirely.
- **Where T2 is** — which line of `mr_reduction`, with locals.
- Whether anything is inside `ConfigServiceImpl::setString` / `configureLogging()` (§5b's
  candidate for the missing half).
- Whether any thread is in `AlgorithmManagerImpl::shutdown()`'s spin-wait (§2c).

One capture distinguishes every competing hypothesis in this document.

### Prerequisites to check now, before the next occurrence

1. `ulimit -c` for the `snsdata` user, and `/proc/sys/kernel/core_pattern` — a core must actually
   be written. Note `livereduce.service` sets no `LimitCORE=`, so it inherits the system default,
   which on many systems is `0`. **Worth setting `LimitCORE=infinity` in the unit now** — that is
   a one-line change that makes the next occurrence diagnosable.
1. Disk space for a ~780 MB-plus core (the process was 778 MB RSS).
1. `coredumpctl` availability, and whether `systemd-coredump` is installed on the beamline host.

**This is the highest-value action in this document**, because it converts the next occurrence from
"another silent hang" into a complete answer, and the only preparation needed is enabling core
dumps.

______________________________________________________________________

## 6. Cheap diagnostics, in order

### A. Check the VTP cache-directory mount — minutes, no code

Directly tests §3's most likely blocker. `findmnt -T <dir>` on the target against
`ConfigService::getVTPFileDirectory()` (falling back to the temp dir). Check both that it is a
network mount and whether it is `hard` or `soft`. **Not** `instrumentDefinition.directory` — see
the correction in §3.

### B. ThreadSanitizer / Helgrind — **not useful for §3**

`CMakePresets.json:210` provides `linux-64-ci-thread-sanitiser` (`USE_SANITIZER=Thread`,
`Sanitizers.cmake:78`). It detects data races and lock-order inversions. §3 is neither: no
inversion, no race, just `m_mutex` held across a syscall that does not return. Nothing for these
tools to report. Listed here only to record that the option was considered and rejected, and
because the preset remains worth running for *races* as separate hygiene.

### C. `kill -ABRT` core dump — no `ptrace` required

Core dumps do not go through `ptrace`. With `systemd-coredump` active and `ulimit -c` permitting,
`coredumpctl gdb` gives full all-thread backtraces offline, with line numbers if debug info
exists. Terminal, so it cannot watch a hang evolve — but for §3 a single core would name the
blocking frame and settle the question. **Cheapest thing that would actually confirm §3.**
Needs a target-side check of `/proc/sys/kernel/core_pattern` and the coredump size limit.

### D. `/proc` thread states — zero code

`/proc/<pid>/task/*/status` gives per-thread `S`/`D`/`R`. A thread blocked on NFS I/O shows `D`
(uninterruptible), which alone would strongly corroborate §3. Note `wchan`, `syscall` and `stack`
are gated behind `ptrace_may_access` on modern kernels and will likely read `0`/`EPERM` — but the
state letter is free, and `D` is the tell.

______________________________________________________________________

## 7. In-process stack dump — superseded

> **Superseded twice over, not recommended.** §5c is the decisive reason: `kill -ABRT` plus
> `pystack core --native-all` produces Python *and* native stacks for every thread — including
> threads the interpreter never registered, which is exactly the listener's background thread —
> from a core the *kernel* writes, with no `ptrace` and no cooperation from the wedged process.
> Everything below tries to obtain a strictly worse version of that from inside the failed
> process.
>
> **Originally superseded because:** An in-process dumper thread lives inside the process that has
> already failed: it can be blocked on the allocator, the loader lock, or the same resource as
> everything else, and for a `D`-state wedge no thread in the process runs at all. Any design
> where the wedged process diagnoses itself bets on the component that just broke.
> Out-of-process detection ([`keepalive-watchdog.md`](keepalive-watchdog.md)) does not have this
> property, and an externally-triggered `SIGABRT` core is written by the *kernel*.
> Retained below as a record of the option and the constraints discovered while evaluating it —
> several of which (hidden visibility, stripped Release builds, `SIGRTMIN` not being a constant)
> apply to any future in-process work.

§3 is the scenario this was designed for: a thread wedged deep in framework code, where the
identity of the blocking frame *is* the answer. The design below assumed two amendments forced by
§1's rarity constraint:

- **Not default-OFF.** A compile-time flag defaulting off will be off when a once-in-months event
  fires. It must ship enabled.
- **Manual trigger is insufficient alone.** Add a watchdog: if the background thread has not
  completed a parse iteration within N seconds while the listener is connected and unpaused, dump
  automatically. Manual `kill -RTMIN+3` remains as an operator-initiated path.

### Design

**Placement.** New LiveData-private TU — `ListenerStackDump.{h,cpp}` under `Framework/LiveData/`
— plus `tools/symbolize_stackdump.py`. Header exposes `installStackDump()`,
`registerCallingThread(const char*)`, `requestStackDump()`, with inline no-op stubs when disabled
so call sites need no `#ifdef`.

**Thread registry.** Fixed-size, allocation-free; everything handlers touch is preallocated at
namespace scope.

```
constexpr std::size_t MAX_TRACKED_THREADS = 8;
constexpr std::size_t MAX_FRAMES = 64;
struct Slot {
  std::atomic<bool> occupied{false};
  pthread_t pth{};
  char role[24]{};
  void *frames[MAX_FRAMES]{};
  std::atomic<int> nframes{0};
  sem_t done{};
};
static Slot g_slots[MAX_TRACKED_THREADS];
```

`registerCallingThread()` wraps a function-local `thread_local` RAII object: construct-on-first-use
gives register-once-per-thread, and the destructor firing at thread exit keeps the registry valid
when the background thread is joined — a stale `pthread_t` passed to `pthread_kill` is undefined
behaviour.

**Signals.** `std::call_once` install, never uninstalled. Trigger `SIGRTMIN + offset` (env
`MANTID_STACKDUMP_SIGNAL_OFFSET`, default 3), capture signal trigger+1, both validated against
`SIGRTMAX`; RT signals avoid the CPython/Qt/Poco collisions `SIGUSR1/2` risk.
*Trap:* glibc's `SIGRTMIN` is `__libc_current_sigrtmin()` — a function call, not a constant; it
cannot be a `case` label, array bound, or `constexpr` initialiser.
Trigger handler does only `sem_post`. Capture handler fills its own slot and posts. `backtrace()`
must be called once at install to force the `libgcc_s.so.1` `dlopen` off the handler path.
`SIGABRT` handler dumps synchronously then `signal(SIGABRT, SIG_DFL); raise(SIGABRT)` so the core
is preserved — deliberately cooperative with §6C. `sigaltstack` for stack-overflow aborts.

**Dumper thread.** Detached, blocks on `sem_wait`; on wake `pthread_kill`s each registered thread
and `sem_timedwait`s ~500 ms each so one wedged thread cannot stall the dump. Symbolises via
`dladdr` (module basename, `pc - dli_fbase`, `dli_sname` when non-null) and `write(2)`s to a
pre-opened fd and stderr.

**Unwinder trade-off.** glibc `backtrace()`: no new deps, poor names under hidden visibility.
libunwind: needs the dep on target, but `unw_get_proc_name` reads the on-disk symtab and recovers
static-function names `dladdr` cannot. `boost::stacktrace`: addr2line backend forks binutils —
unsafe in-handler and unlikely to be present on a locked-down VM.

**Integration points.** ctor `:89` install; `run()` `:262` register `livedata-bg`; `connect()`
`:165`, `start()` `:225`, `doExtractData()` `:1550`, `onBeforeExtract()` `:1694` register
`livedata-fg`.

**Reuse note.** `Framework/API/src/FrameworkManager.cpp:66-102` is the in-tree precedent for the
unwind call and `#ifdef __linux__` convention, but `backtraceToStream()` itself is not reusable
near a handler — `backtrace_symbols()` mallocs and it writes through `std::ostream`. Its
`set_terminate` hook is complementary; leave it alone.

**Residual risk.** A thread wedged inside `malloc` or holding the loader lock blocks the dumper
thread's own `dladdr`. `sem_timedwait` bounds the damage but cannot eliminate it. Under §3 the
wedged thread is blocked in I/O, not holding the allocator, so the dump should succeed.

______________________________________________________________________

## 8. Open questions

1. Is the VTP cache directory on a network mount on the target, and `hard` or `soft`? (§6A —
   settles §3's most likely blocker.)
1. **Does the hang ever self-heal, or is it always permanent?** This is the single most
   discriminating observation available without new code. Permanent → a call that never returns
   (hard mount against a dead server). Self-healing after minutes → slow I/O, and a timeout would
   fix it.
1. What does the deployed logging configuration retain? If warning-level messages are dropped,
   the §5 eliminations are not safe.
1. Is `systemd-coredump` active, and what is `ulimit -c`? (§6C.)
1. Does an operator have shell access while the process is hung? Decides whether manual triggering
   is worth anything, or whether the watchdog is the only viable trigger.
1. Base-class vs listener-local fix for §4 — `LiveListener::extractData()` is shared by all
   listeners (ISIS, Kafka, …), so the blast radius is wider than this class.

### Resolved

- ~~Does `MonitorLiveData` reuse the listener after a failed extract?~~ **No** — it destructs on
  throw (`MonitorLiveData.cpp:107-111`). §4 is latent, not a live hang mechanism.
- ~~Is the IDF directory the blocking I/O?~~ **No** — `InstrumentXML` short-circuits the file
  path (`LoadInstrument.cpp:101-115`). The I/O is the VTP geometry cache.
