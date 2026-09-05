# Component 1 — keep-alive heartbeat and out-of-process watchdog

> **Status: design / discussion. Not ready to implement.**
> Part 1 of 3. Companion documents: [`first-pass.md`](first-pass.md) (investigation history and
> evidence), [`deferred-action-refactor.md`](deferred-action-refactor.md) (part 2 — the
> structural fix), [`hazard-probes.md`](hazard-probes.md) (part 3 — tests that trigger the
> lock-ups, and the framework defects they would evidence).
>
> This component is **independent of part 2** and worth shipping first: it turns an unobservable
> once-a-year event into a recorded one, whatever the underlying cause turns out to be.

______________________________________________________________________

## Story for EWM use

*Plain prose, intended to be pasted directly into an EWM work item. Everything below this section
is implementation detail and does not need to go into EWM.*

The automated live-reduction service occasionally stops processing data and never recovers. The
service runs headless as a systemd unit, driving Mantid's standard StartLiveData /
MonitorLiveData / LoadLiveData stack against the SNS live data stream. When the failure occurs
the process remains alive but produces no further output: no exception is raised, no error or
warning is written to the log, and the live view simply stops updating. The only known recovery
is to kill the service and restart it. Because the failure is rare and leaves no trace, we have
never captured any information about the state of the system at the time it occurred, and we
therefore cannot say with confidence what the cause is. Every diagnostic approach that depends on
someone being present while the service is hung has proven impractical, and approaches that would
have the affected process examine itself are unreliable by construction, because the component
doing the examining lives inside the process that has already failed. The same listener is also
used from ordinary Python scripts and from Workbench, outside any service, so whatever is built
must work in both settings.

This story delivers automated, always-on detection that records the state of the live listener at
the moment it stops making progress, without requiring anyone to be present. The listener will
publish a small, continuously updated status record into a shared-memory segment: progress
counters for its background and foreground threads, which phase of its work each thread is
currently in, how long it has been there, the values of its internal synchronisation flags, and a
short rolling history of recent state transitions. Writing this record costs a handful of memory
writes and involves no locks, no system calls and no file access, so it is safe to leave enabled
permanently in production. A small companion watchdog process, started automatically by the
listener itself so that it requires no operator action in either setting, observes that record
from outside the affected process and reports when progress stops. Reporting goes to the system
journal, which unprivileged user processes can write to directly — this has been verified — so the
same mechanism serves both the service and ordinary script use. The watchdog additionally
maintains a plain-text status file that can simply be displayed with standard shell commands,
which requires no journal access or special tooling at all.

Because the watchdog runs as a separate process and only ever reads, it continues to function
when the monitored process is completely stalled. On detecting a stall it records what it already
knows — which thread stopped, in which phase, for how long, and the sequence of transitions that
led there — and additionally reads the operating system's own per-thread state for the affected
process. That last item resolves a question we currently cannot answer: whether the stall is a
kernel-level block, from which the process cannot be signalled or killed at all, or an ordinary
lock deadlock, from which a full diagnostic snapshot can still be obtained. Knowing which of these
it is determines what any future recovery or diagnosis can achieve, and at present we do not know
because the service has only ever been terminated with an unconditional kill. The watchdog also
reports its full assessment on request and at shutdown, so an operator can inspect a running
service at any time and confirm the instrumentation is working correctly, rather than discovering
a problem with it at the moment it is needed.

The scope of this story is limited to observation. It introduces no change to data acquisition
behaviour, no change to the reduction pipeline, and no change to how the service is deployed or
started. There is no new deployable artifact to package or install: the watchdog is a Python
module that ships inside the existing Mantid conda package, and the status record uses shared
memory facilities already available through libraries the listener depends on. The work is
independent of, and can be delivered ahead of, the related structural refactoring of the listener's
locking behaviour. It is worth doing first regardless of the outcome of that work, because it
records what actually happens the next time the failure occurs instead of requiring us to
reproduce it first.

______________________________________________________________________

## 1. Deployment context — two modes, not one

`SNSLiveEventDataListener` is used in at least two quite different ways, and the design must serve
both from a single mechanism.

**Mode A — the live-reduce service.** A non-user `systemd` unit driving Mantid's standard
`StartLiveData` → `MonitorLiveData` → `LoadLiveData` stack. Headless, no Qt, no operator present
when it wedges. This is where the observed hang occurs.

**Mode B — ordinary Python scripts and Workbench.** The same listener launched from user code
outside any unit: interactive sessions, ad-hoc analysis scripts, MantidWorkbench. No unit, no
cgroup, possibly a terminal attached, possibly several concurrent sessions on one machine and
several users on one machine.

Consequences that shape everything below:

1. **Nobody can start a monitor by hand in either mode.** The listener must start it
   automatically. Treated as a hard requirement (§5.2).
1. **Output cannot rely on inherited stdout.** Under a unit, a child's stderr reaches journald by
   inheritance; from a script it reaches a terminal, or nowhere at all. A uniform mechanism is
   needed — see §5.6, which verifies that unprivileged processes can write to the journal
   directly.
1. **Status must be inspectable with plain shell commands**, since in Mode B there is no
   `systemctl status` to consult and possibly no journal retention configured. See §5.7.
1. **`systemd` `PrivateTmp=yes`** (Mode A only) gives the unit a private `/tmp` *and* `/dev/shm`
   mount namespace. If set, a segment created inside the unit is invisible outside it — a concrete
   argument for launching the watchdog from the listener, since an independently-started monitor
   could not see it at all. Worth confirming whether the unit sets it.
1. **Default `KillMode=control-group`** (Mode A only) means `systemctl stop|restart` signals the
   whole cgroup, watchdog included. See §5.4.
1. **Mode B is multi-user and multi-session.** Segment naming and permissions must not collide
   across users or across concurrent sessions (§3, §5.1).

______________________________________________________________________

## 2. Why out-of-process

Settled in [`first-pass.md`](first-pass.md) §1: the hang never self-heals, a live occurrence will
probably never be available to inspect, and an in-process dumper thread lives inside the process
that has already failed. Any design where the wedged process diagnoses itself bets on the
component that just broke.

The discipline that follows: **the listener may only ever *publish*; the watchdog may only ever
*observe*.** Nothing in the protocol may require the monitored process to respond.

This immediately rules out `Poco::NamedEvent` and `Poco::NamedMutex`, both of which need the
monitored process to actively signal.

______________________________________________________________________

## 3. Transport: `Poco::SharedMemory` on `/dev/shm`

Poco supplies the primitive, and the listener already depends on Poco (`Poco::Thread`,
`Poco::Net`, `Poco::Timer`):

```cpp
#include <Poco/SharedMemory.h>

// Listener: one segment per PROCESS, created once under std::call_once.
// server=true  ->  creates the object AND unlinks it on destruction.
Poco::SharedMemory shm("mantid-livedata-" + std::to_string(getpid()),
                       sizeof(KeepAliveSegment), Poco::SharedMemory::AM_WRITE,
                       nullptr, /*server=*/true);
auto *seg = reinterpret_cast<KeepAliveSegment *>(shm.begin());
```

The watchdog attaches with `AM_READ` and **`server=false`**.

### The `server` flag — measured, and the opposite of the documentation

`SharedMemory_POSIX.h:44-45` states: *"If server is set to **false**, the shared memory region
will be unlinked by calling `shm_unlink` when the SharedMemory object is destroyed."* **That is
wrong.** Measured behaviour:

| `server` | Actual behaviour                                                                             |
| -------- | -------------------------------------------------------------------------------------------- |
| `true`   | Creates the object (`O_CREAT`) **and unlinks it on destruction** — the owner                 |
| `false`  | Attaches to an existing object only; throws `Poco::SystemException` if absent; never unlinks |

So the listener uses `true` and the watchdog uses `false`. Do not follow the header comment.

Two consequences that shape the watchdog:

- **An existing mapping survives the unlink** (verified: a reader still read its payload after the
  creator was destroyed). So the watchdog must **attach once at startup and hold the mapping**,
  never re-open per poll. This is what lets it produce a final report after a clean listener
  shutdown has already removed the segment.
- **A fresh attach after unlink fails** with `Cannot create shared memory object: /<name>`. The
  watchdog therefore needs a bounded retry at startup — it is launched immediately after the
  segment is created, but process startup is not instantaneous, and a very short-lived listener
  could unlink first. Treat repeated failure as "nothing to watch" and exit quietly.

**Naming.** Poco prepends `/` for `shm_open` (visible in the exception text) but the file appears
in `/dev/shm/` under the bare name — verified as `/dev/shm/mantid-livedata-<pid>`, mode `0600`.
The header also warns the name must contain no slashes, which `mantid-livedata-<pid>` satisfies.

**One segment per process, one slot per listener instance.** Mode B can run several concurrent
live listeners in one Workbench or script process, so the segment holds a small fixed array and
each listener claims a slot on construction (§4). This keeps exactly one watchdog per process
rather than one per listener.

**Boot safety.** `/dev/shm` is tmpfs and is cleared on reboot, so a segment can never outlive the
boot whose `CLOCK_MONOTONIC` epoch its timestamps are relative to. No boot-id check is needed.

The named constructor uses `shm_open`, so on Linux the segment lands on **tmpfs** under
`/dev/shm/`. That is the property that matters:

- Steady-state writes are plain memory stores — no syscall, no lock, no allocation.
- tmpfs never blocks on disk or network, so the heartbeat cannot be caught by the failure it is
  reporting. **A regular file would be wrong** for exactly this reason: if the wedged resource is
  the filesystem, `write()` joins the wedge. `Poco::SharedMemory`'s other constructor takes a
  `Poco::File` and is file-backed — do not use it here.
- The watchdog maps the same segment `AM_READ`. A wedged process's *memory* stays readable even
  when it executes nothing; the counter simply stops advancing, which is the signal.

### Implementation traps

- **Do not put `std::atomic<T>` in the shared struct.** Cross-process atomics are valid only if
  lock-free — a lock-based atomic uses a per-process lock table that is not shared — and there is
  no clean way to construct them in a mapped region the reader must not re-construct.
- **Use C++20 `std::atomic_ref` over plain POD instead.** The project is C++20
  (`first-pass.md` §1), so the struct stays POD and both sides wrap fields at the point of
  access. No lifetime or construction problem. Guard with
  `static_assert(std::atomic_ref<uint64_t>::is_always_lock_free)`.
- **The `server` flag is counter-intuitive.** `SharedMemory_POSIX.h:44-45`: *"If server is set to
  **false**, the shared memory region will be unlinked by calling `shm_unlink` when the
  SharedMemory object is destroyed."* Verify empirically before relying on it.
- Keep the struct naturally aligned and versioned. Do **not** cache-line-align the counters —
  measured in §4, the padding buys nothing at this write rate and complicates the Python reader.

______________________________________________________________________

## 4. Published state

Plain POD, accessed through `std::atomic_ref`.

```cpp
constexpr uint32_t MAX_LISTENER_SLOTS = 4;
constexpr uint32_t TRACE_DEPTH = 64;

struct TraceEntry {                   // 16 bytes
  uint64_t us;                        // Poco::Clock, MICROseconds, monotonic
  uint32_t phase;
  uint32_t detail;
};

struct Heartbeat {                    // one per listener instance — 1096 bytes
  uint32_t inUse;                     // slot claimed
  uint32_t reserved;

  uint64_t bgIterations;              // ++ per parse-loop iteration
  uint64_t bgPhaseEnteredUs;
  uint32_t bgPhase;                   // Polling|Reading|Parsing|InFramework|Paused|Exited
  uint32_t bgLastPacketType;          // ADARA packet type last dispatched

  uint64_t fgExtractCalls;
  uint64_t fgPhaseEnteredUs;
  uint64_t fgLastSuccessUs;           // last extractData() that returned data
  uint32_t fgPhase;                   // Idle|WaitingInit|WaitingLock|Extracting
  uint32_t flags;                     // connected|pauseNetRead|bgCaughtUp|stopThread|dasPaused

  uint32_t traceHead;                 // ring index
  uint32_t traceCount;
  TraceEntry trace[TRACE_DEPTH];
};

struct KeepAliveSegment {             // one per process — 4400 bytes
  uint32_t magic;                     // 'MLKA'
  uint32_t version;                   // bump on ANY layout change
  int32_t  pid;                       // owning process
  uint32_t slotCount;                 // == MAX_LISTENER_SLOTS
  Heartbeat slots[MAX_LISTENER_SLOTS];
};
```

**Declaration order matters** — an earlier revision of this document listed `KeepAliveSegment`
first, which does not compile: `Heartbeat` must be complete before it can be used as an array
member.

**Units are microseconds, not nanoseconds.** `Poco::Clock` is documented at `Clock.h:47` as a
*"Monotonic clock value in microsecond resolution"*, with a `microseconds()` accessor. An earlier
revision named these fields `...Ns`, which would have been silently wrong by a factor of 1000
against a `Poco::Clock` source. Microsecond resolution is ample against a one-second poll. Use
`clock_gettime(CLOCK_MONOTONIC)` directly only if nanoseconds are ever genuinely needed — and then
rename the fields.

**No `alignas(64)`.** An earlier revision cache-line-aligned the two counter groups to avoid false
sharing. Measured, that costs more than it buys:

|                    | `sizeof(Heartbeat)` | offsets of `bgIterations` / `fgExtractCalls` / `trace` |
| ------------------ | ------------------- | ------------------------------------------------------ |
| with `alignas(64)` | 1216                | 64 / 128 / 168                                         |
| without            | **1096**            | **8 / 32 / 72**                                        |

Fields are written roughly once per parse iteration — order 10 Hz — so false sharing is
irrelevant, while the padded layout forces the Python reader to hardcode non-obvious offsets. The
natural layout is directly describable as a format string, which is the real win (§4a).

### 4a. Mirrored layout — verified against the C++

The Python watchdog reads the segment with `mmap` + `struct.unpack_from`, so the layout is
expressed twice. Mitigating that duplication was listed as the cost of choosing Python; here is
the exact contract, checked by compiling the struct and comparing:

```python
SEG_HDR = "<4I"  # magic, version, pid, slotCount      -> 16 bytes
SLOT_HDR = "<2I2Q2I3Q2I2I"  # through traceCount                  -> 72 bytes
TRACE_ENT = "<QII"  # us, phase, detail                   -> 16 bytes
SLOT_SIZE = 72 + 64 * 16  #                                     -> 1096 bytes
SEGMENT = 16 + 4 * 1096  #                                     -> 4400 bytes
```

Verified equal to the compiled C++ (`sizeof(Heartbeat) == 1096`,
`offsetof(Heartbeat, trace) == 72`, four-slot segment `== 4400`). Both sides must assert
`magic == 'MLKA'` and refuse an unrecognised `version`; the Python module should additionally
assert `struct.calcsize(SLOT_HDR) == 72` at import, so a layout drift fails loudly at startup
rather than producing plausible nonsense.

Four things earn their place beyond a bare liveness bit:

- **`bgPhase`** — *where* the background thread was when it stopped advancing. Once part 2 confines
  every framework call to one identifiable block, `InFramework` is close to a complete diagnosis
  **with no stack trace involved at all**. This is the single highest-value field.
- **`fgPhase`** — distinguishes "foreground blocked on `m_mutex`" from "foreground never called
  again". Identical from outside; different bugs.
- **`flags`** — the listener predicates (`m_pauseNetRead`, `m_bgThreadCaughtUp`, …) that
  `first-pass.md` §4 shows can strand. Free to publish, and directly diagnostic.
- **`trace` ring** — history, not just the final state. For a once-a-year event the sequence
  leading in is worth more than the instant, at one store per transition.

**Timestamps: `Poco::Clock`, not `Poco::Timestamp`.** `Clock.h:27-39` documents it as monotonic
where the OS provides it (POSIX `clock_gettime` / `CLOCK_MONOTONIC`), with `Clock::monotonic()` to
verify. `Timestamp` is wall-clock and misreports elapsed time across an NTP step.

**Writer cost:** a handful of relaxed stores per iteration. Safe to ship always-on — which §2
requires, since a compile-time-OFF feature will be off when a rare event fires.

______________________________________________________________________

## 4b. Configuration keys

**Every option is a `Mantid.properties` key.** No environment variables, no compile-time macros
for behaviour, no hard-coded paths. Existing convention, from
`Framework/Properties/Mantid.properties.template:127-130`:

```properties
# Default values for SNS DAE (live data)
SNSLiveEventDataListener.keepPausedEvents = false
SNSLiveEventDataListener.startupTimeout = 10
SNSLiveEventDataListener.testAddress = 127.0.0.1:12345
```

So: `SNSLiveEventDataListener.` prefix, camelCase leaf, `=` spacing, booleans as `true`/`false`,
durations as bare seconds. Mantid already uses deeper dotted keys elsewhere
(`logging.channels.consoleChannel.class`), so a `keepAlive.` sub-namespace stays consistent while
keeping the feature's keys grouped.

New block, to be added immediately after `SNSLiveEventDataListener.testAddress`:

```properties
# Keep-alive heartbeat and out-of-process watchdog.  The listener publishes a
# small progress record to shared memory and launches a watchdog process that
# reports to the system journal if progress stops.  Purely diagnostic: disabling
# it changes no acquisition behaviour.
SNSLiveEventDataListener.keepAlive.enabled = true
# Seconds the watchdog waits between polls of the shared-memory record.
SNSLiveEventDataListener.keepAlive.pollInterval = 1
# Seconds without progress, while connected and unpaused, before a wedge is
# declared and reported.  Must exceed the worst-case normal parse iteration,
# which includes instrument loading on the first packets of a run.
SNSLiveEventDataListener.keepAlive.stallThreshold = 60
# Seconds between routine "still healthy" summaries.  Bounds how much history is
# lost if the watchdog is SIGKILLed.  Set to 0 to emit only on wedge detection.
SNSLiveEventDataListener.keepAlive.summaryInterval = 300
# Directory for the plain-text status mirror.  Must be on a filesystem that
# cannot block (tmpfs); do NOT point this at network storage.
SNSLiveEventDataListener.keepAlive.statusDir = /dev/shm
# Syslog identifier used for journal entries; retrieve with
#   journalctl -t mantid-livedata
SNSLiveEventDataListener.keepAlive.syslogTag = mantid-livedata
# Command used to launch the watchdog.  Empty means derive the interpreter from
# /proc/self/exe and run "-m mantid.LiveData.watchdog", which is correct for all
# Python-hosted deployments.  Set explicitly only for unusual environments.
SNSLiveEventDataListener.keepAlive.command =
# If true, the watchdog sends SIGABRT to a wedged process to force a core dump.
# This TERMINATES the session, so it is off by default and should only be enabled
# on the unattended reduction service, never for interactive use.
SNSLiveEventDataListener.keepAlive.abortOnWedge = false
```

Notes on individual keys:

- **`enabled`** defaults `true` because the rarity argument ([`first-pass.md`](first-pass.md) §1)
  means anything defaulting off will be off when the event finally fires. It remains the switch
  for interactive or test contexts where the extra subprocess is unwelcome — see §7 question 3,
  which is about this default specifically.
- **`stallThreshold`** is the one value that needs measurement rather than a guess: too low and a
  legitimately slow `LoadInstrument` produces false wedges, too high and a real stall sits
  unreported. §7 question 6.
- **`statusDir`** carries a warning in the comment for a reason: pointing it at network storage
  would put the watchdog's own writes in the failure domain it is meant to observe.
- **`command`** empty-means-derive keeps the common case configuration-free while leaving an
  escape hatch. It is also the natural way to disable the feature where there is no interpreter,
  such as the C++ unit tests.
- **`abortOnWedge`** is the only key that can destroy data, hence off by default and explicitly
  documented as service-only.

Read them with the existing idiom (`ConfigService::Instance().getValue<T>(...)` returning
`std::optional`, as at `SNSLiveEventDataListener.cpp:100`, `:178`, `:1555`), and pass the
watchdog-side values to the child on its command line so it needs no `ConfigService` of its own.

**Scope of the prefix.** These keys sit under `SNSLiveEventDataListener.` per the existing
grouping. If the mechanism is later generalised to other listeners, `LiveData.keepAlive.*` would
be the better home, with the SNS keys kept as aliases.

______________________________________________________________________

## 5. The watchdog process

### 5.1 Distribution — there is no binary

The concern was shipping a separate compiled component. **Not needed.**
`Framework/PythonInterface/setup.py:17` uses `find_packages(exclude=["*.test"])`, so any
directory carrying an `__init__.py` is packaged and installed by `pip` into the `mantid` conda
package automatically — no CMake rule, no build target, no packaging entry.

So the watchdog is a **Python module** at `Framework/PythonInterface/mantid/LiveData/watchdog/`.
It reads the segment with `mmap` + `struct.unpack_from`, parses `/proc`, and logs. Deployable,
and — importantly for a beamline — modifiable without a C++ rebuild.

**Prerequisite: the `__init__.py` chain under `mantid/LiveData/` is currently broken.**

| Directory                                     | `__init__.py` |
| --------------------------------------------- | ------------- |
| `mantid`                                      | present       |
| `mantid/LiveData`                             | **missing**   |
| `mantid/LiveData/ADARA`                       | **missing**   |
| `mantid/LiveData/ADARA/utils`                 | **missing**   |
| `mantid/LiveData/ADARA/utils/packet_playback` | present       |

Verified with a `find_packages()` probe: nothing under `LiveData` is currently discovered, so the
existing ADARA `packet_playback` tooling is **not shipped** — it is dev/test-only, reached by
tests through an absolute source path
(`test/python/mantid/LiveData/ADARA/utils/packet_playback/CMakeLists.txt:22`).

Adding the three missing `__init__.py` files makes the whole tree ship. Note this is a real
side effect to weigh: it would also start shipping `packet_playback`, which may or may not be
wanted. If not, place the watchdog outside that subtree and add only
`mantid/LiveData/__init__.py`.

**Cost of Python:** the `Heartbeat` layout is mirrored in two places. Mitigate with `magic` and
`version` checked on every read, refusing to interpret an unrecognised version, plus a single
source-of-truth comment block in the C++ header naming the exact `struct` format string.

### 5.2 Launch — by the listener, in both modes

One watchdog **per process**, not per listener instance. `installKeepAlive()` is `std::call_once`
guarded; it creates the segment and launches the watchdog. Each listener instance then claims a
slot within that segment, so a Workbench session with several concurrent live listeners still has
exactly one watchdog.

**Finding the interpreter without linking PythonInterface.** `LiveData` does not link the Python
interface and cannot call `Py_GetProgramFullPath()`. Verified alternative: `readlink("/proc/self/exe")`
returns the hosting interpreter in both modes — measured as
`…/envs/mantid-developer/bin/python3.12` from a Python process. Since every deployment that
matters is Python-hosted, this resolves the interpreter with no dependency and no configuration.
Fall back to `SNSLiveEventDataListener.keepAlive.command` (§4b) when `/proc/self/exe` is
not a Python binary — which is also the natural way to *disable* the feature in C++ unit tests,
where there is no interpreter and no watchdog is wanted.

```cpp
Poco::Process::launch(interpreter,
                      {"-m", "mantid.LiveData.watchdog", segmentName});
```

`Poco::Process` also supplies `isRunning(PID)`, `requestTermination(PID)` and `kill(PID)` for
teardown.

I previously argued against listener-launched monitors on lifetime-coupling grounds. **That
objection does not survive either deployment**: under `PrivateTmp` an external monitor cannot see
the segment, and in both modes there is nobody to start one by hand.

Requirements on the child:

- Must not inherit file descriptors beyond what it needs — `O_CLOEXEC` discipline on the listener
  side, including the socket to the SMS.
- The listener must never *wait* on it, and must tolerate launch failure **silently**. A watchdog
  that fails to start must never take down data acquisition; this is diagnostic scaffolding, not a
  dependency.
- The child must reset any signal dispositions it inherits before installing its own (§5.8).

### 5.3 Lifetime in Mode B — no cgroup to clean up

Outside systemd nothing reaps the watchdog, so it must terminate itself. It already polls
`/proc/<pid>` for stale-segment detection; extend that to its own subject: **when the listener's
process disappears from `/proc`, emit a final report and exit.** That covers normal script exit,
Workbench shutdown, and `kill -9` of the host, and needs no signal cooperation from a process that
may be wedged.

Prefer this over `prctl(PR_SET_PDEATHSIG)`, which fires on the death of the *creating thread*
rather than the process — a subtlety that would misfire if the listener is constructed on a
short-lived thread.

The listener's destructor should additionally call `Poco::Process::requestTermination()` for a
prompt, tidy exit in the common case; the `/proc` poll is the backstop for when it never runs.

### 5.4 Lifetime under systemd (Mode A)

Default `KillMode=control-group` means `systemctl stop|restart` sends `SIGTERM` to the entire
cgroup, watchdog included. Two consequences:

- The watchdog **cannot** outlive the unit to report on the corpse. Accept this: its job is to
  detect and record the wedge *while it is happening* (§5.8).
- It receives that `SIGTERM` itself, which is what makes the shutdown snapshot in §5.8 possible —
  and means `kill <watchdog-pid>` and `systemctl stop` produce the same journal record.

**Stale segments:** `SIGKILL` runs no destructor, leaving `/dev/shm/mantid-livedata-<pid>` behind.
The watchdog reaps segments whose `pid` no longer exists in `/proc`, checking the in-payload `pid`
as well as the name to survive pid recycling. *If* `PrivateTmp=yes` is set, the namespace is torn
down with the unit and stale segments vanish on their own — another reason to establish whether it
is enabled.

### 5.5 Detection and escalation

Poll every ~1 s. Declare a wedge when a counter has not advanced for N seconds while `flags`
indicate connected and not paused. Then, in order:

1. **Report what it already knows** — phase, time in phase, predicate flags, trace ring. For an
   `InFramework` stall this is close to the whole answer.

1. **Read `/proc/<pid>/task/*/status` and `.../stat`.** Free, no `ptrace`. The state letter plus
   `utime`/`stime` deltas distinguish **all three** failure shapes at zero cost, automatically, at
   the next occurrence with nobody present:

   | State | CPU  | Meaning                                                                                                                                                                   |
   | ----- | ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `S`   | 0%   | lock or GIL block — e.g. the `SplitterChannel`↔GIL inversion in [`deferred-action-refactor.md`](deferred-action-refactor.md) §2b.1                                        |
   | `R`   | 100% | **data-structure corruption** — a spin in a corrupted container, from the unsynchronized singletons in [`deferred-action-refactor.md`](deferred-action-refactor.md) §2b.2 |
   | `D`   | 0%   | kernel-level block; process unkillable, no core obtainable                                                                                                                |

   Sampling CPU time across two polls is what separates `R`-spinning from `S`-blocking, and that
   distinction points at completely different root causes. Worth capturing from the start.

1. **If `S` (futex deadlock): `kill -ABRT`** for a complete all-thread core written *by the
   kernel*, retrievable with `coredumpctl`. Reliable precisely because it asks the wedged process
   to do nothing. **Gate behind `keepAlive.abortOnWedge`, default off** — it terminates a
   production acquisition.
   **Then analyse it with `pystack core ... --native-all`**, which yields Python *and* native
   frames for every thread including ones the interpreter never registered — i.e. the listener's
   `Poco::Thread`. See [`first-pass.md`](first-pass.md) §5c for the full procedure; this is the
   single most valuable artifact the watchdog can produce, and it is why the escalation is worth
   having at all.

1. **If `D` (kernel block):** record and stop. Nothing further will work; the process cannot be
   signalled or killed, and knowing *that* is the diagnosis.

**Output:** see §5.6 (journal, both modes) and §5.7 (shell-inspectable status).

### 5.6 Reaching the journal in both modes — verified

**An unprivileged process can write to the journal.** Confirmed by round trip on a target-like
host: a plain user process wrote via Python's stdlib `syslog` module and `journalctl -t <tag>`
read it back.

```python
import syslog

syslog.openlog("mantid-livedata", syslog.LOG_PID, syslog.LOG_USER)
syslog.syslog(syslog.LOG_ERR, "MANTID-LIVEDATA-WEDGE ...")
```

Verified present on the host: `/dev/log`, `/run/systemd/journal/socket`,
`/run/systemd/journal/dev-log`, plus `logger`, `systemd-cat` and `journalctl`. **No unit, no root,
no new dependency** — `syslog` is in the standard library, which matters given `systemd.journal`
is not installed and would be a new conda dependency.

This replaces the earlier stdout-inheritance design, which only worked in Mode A. Using syslog
uniformly means one code path for both modes and a stable retrieval command:

```
journalctl -t mantid-livedata -n 50
journalctl -t mantid-livedata -p err --since "2 hours ago"
```

In Mode A, journald additionally attaches `_SYSTEMD_UNIT` from the sender's cgroup, so
`journalctl -u <unit>` still finds the same records — worth confirming, but it means Mode A loses
nothing by the change.

**Avoiding duplicate output.** Always write to syslog. Write to stderr *additionally* only when
`$JOURNAL_STREAM` is unset — systemd sets that variable for services whose stderr already goes to
the journal (confirmed unset in an ordinary shell), so this precisely suppresses the double entry
in Mode A while still showing output to an interactive user in Mode B.

**Journal delivery is best-effort.** If journald is absent (a non-systemd container, a stripped
image), the syslog write goes nowhere. The status file in §5.7 is therefore the channel that
always works, and the design must not depend on the journal alone.

### 5.7 Shell-inspectable status — no tooling required

Since Mode B has no `systemctl status` and may have no journal retention, the watchdog also
maintains a **plain-text mirror** of its current assessment, rewritten on each poll:

```
/dev/shm/mantid-livedata-<pid>.status
```

Written to a temporary name and `rename()`d into place, so a reader never sees a partial file —
atomic on the same filesystem, and cheap on tmpfs. This gives a complete answer with no decoder,
no Python, and no journal:

```
cat /dev/shm/mantid-livedata-*.status      # current state of every live listener
ls -l /dev/shm/mantid-livedata-*           # discover sessions on this machine
kill -USR1 <watchdog-pid>                  # force a fresh dump to the journal
journalctl -t mantid-livedata -p err       # every wedge ever recorded
```

The binary segment stays the machine-readable channel written by the listener; the `.status` file
is the human-readable snapshot written by the watchdog. Only the watchdog writes it, so a wedged
listener cannot leave it stale without the staleness itself being visible in the timestamp it
carries.

**Permissions.** `shm_open` creates with mode `0600` (verified), i.e. owner-only. Correct default
for a shared analysis machine: another user cannot read a session's segment, and the watchdog runs
as the same user by construction, being a child. Segments are named by pid, which is unique
system-wide, so Mode B's concurrent sessions and multiple users cannot collide.

### 5.8 Signal-triggered state reporting

The watchdog can report its last observed state on demand and on death, straight into the journal.

**Signals it can act on:**

| Signal              | Source                                        | Action                                                                                                                  |
| ------------------- | --------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `SIGUSR1`           | `kill -USR1 <pid>`, operator                  | **Full state dump, keep running.** The most useful of the set — inspect a live session at any time, disturbing nothing. |
| `SIGTERM`           | `systemctl stop\|restart`, plain `kill <pid>` | Final dump, then exit promptly.                                                                                         |
| `SIGINT` / `SIGHUP` | interactive / reload                          | Same as `SIGTERM`.                                                                                                      |
| `SIGKILL`           | `kill -9`, systemd after `TimeoutStopSec`     | **Nothing runs.** No handler is possible — see "do not rely on it" below.                                               |

Because plain `kill <pid>` defaults to `SIGTERM`, `kill <watchdog-pid>` produces exactly the
behaviour you described: last state appears in the systemd journal.

**Python makes this easy in a way a C++ watchdog would not.** Python signal handlers are ordinary
Python code executed between bytecodes in the main thread — *not* C signal-handler context. They
may allocate, format strings, read `/proc`, and write output freely. None of the
async-signal-safety constraints that dominated the in-process stack-dump design
([`first-pass.md`](first-pass.md) §7) apply here. This is a substantive argument for the Python
watchdog beyond distribution convenience.

**Buffering — the detail that would silently break this.** Verified in the target interpreter:
`sys.stdout.line_buffering` is `False` when piped (block-buffered), `sys.stderr.line_buffering`
is `True`. A dump written to stdout can therefore sit unflushed in a 4–8 KB buffer and be **lost**
when the process dies. Write dumps to **`sys.stderr` with an explicit `flush=True`**.

**Priority levels without a dependency.** `systemd.journal` is not present in the environment and
would be a new conda dependency. Not needed: journald's stdout/stderr parser honours syslog-style
priority prefixes, so

```python
print(f"<3>MANTID-LIVEDATA-WEDGE {details}", file=sys.stderr, flush=True)
```

lands as an **error**-priority journal entry. `<4>` warning, `<6>` info, `<7>` debug. Use a fixed
greppable token (`MANTID-LIVEDATA-WEDGE`) so `journalctl -u <unit> -g MANTID-LIVEDATA-WEDGE`
retrieves every occurrence.

**Capture in order of perishability.** On `systemctl stop`, systemd signals the whole cgroup at
once, so the listener may exit *while* the watchdog is dumping. Capture what disappears first:

1. `/proc/<pid>/task/*/status` — gone the instant the process exits;
1. the heartbeat segment — survives until unlinked;
1. formatting and emission last.

**What a dump contains:** watchdog uptime and poll count, the exit reason (signal name), the full
heartbeat snapshot (both phases, counters, flags, time-in-phase), the trace ring, the per-thread
kernel states, and a one-line verdict — `HEALTHY`, or `WEDGED: bg stalled 412 s in InFramework`.

**Do not rely on the death-dump as the primary capture.** `SIGKILL` runs no handler, and systemd
escalates to it after `TimeoutStopSec`. Ordering by reliability:

1. **Emit at detection time** — primary. The wedge is journalled the moment it is detected,
   typically long before anyone kills anything.
1. **Emit on `SIGTERM`/`SIGUSR1`** — the on-demand and shutdown snapshots.
1. **Periodic INFO summary** every few minutes — bounds how much history a `SIGKILL` can destroy.

**Do not delay shutdown.** The dump must finish well inside `TimeoutStopSec` (default 90 s);
target milliseconds. Handle `SIGTERM` inline and exit, since there may be no next loop iteration;
for `SIGUSR1` prefer setting a flag the poll loop picks up, with a simple re-entrancy guard so a
signal arriving mid-format cannot interleave output.

______________________________________________________________________

## 6. What this answers on its own

Even if part 2 is never built, this component converts the current situation — *"it froze, there
is nothing in the log, and we killed it with `SIGKILL`"* — into a journald record containing:

- which thread stopped advancing, and when;
- what phase it was in, and how long it had been there;
- the listener predicate flags at the time;
- the sequence of transitions leading in;
- **the kernel thread state**, settling whether this is an uninterruptible kernel block or a
  futex deadlock — the question that is currently unanswerable from memory because `SIGKILL` has
  always been the cleanup method, and which decides whether any core dump is obtainable at all.

That is the whole argument for doing this first: instrument once, capture whenever it happens, no
waiting and no luck required.

It also makes the state inspectable *on demand* rather than only after a failure: `kill -USR1` on
a healthy session prints the same report, which is the cheapest possible way to confirm the
instrumentation is correct and the phases are being maintained properly — long before you need it
to diagnose anything.

______________________________________________________________________

## 7. Open questions

1. Does the systemd unit set `PrivateTmp=yes`? Decides whether stale-segment reaping is needed in
   Mode A and confirms the launch model. (Mode B always needs reaping — no namespace teardown.)
1. Is `MAX_LISTENER_SLOTS = 4` enough for realistic Workbench use, and what should happen if a
   fifth listener is constructed — run unmonitored, or reuse the oldest released slot?
1. Should `SNSLiveEventDataListener.keepAlive.enabled` default on in Mode B as well as Mode A? It
   is cheap and silent unless something stalls, but it does spawn a subprocess per Mantid session,
   which may be unwelcome in interactive or test contexts. The key covers it either way; the
   question is the default.
1. Does journald attach `_SYSTEMD_UNIT` to syslog-transport messages from inside a unit's cgroup,
   so `journalctl -u <unit>` still finds them after the move away from stdout inheritance? (§5.6 —
   expected yes, worth confirming on the target.)
1. Is shipping `packet_playback` as a side effect of fixing the `__init__.py` chain acceptable, or
   should the watchdog live outside that subtree? (§5.1)
1. What value for `SNSLiveEventDataListener.keepAlive.stallThreshold`? Long enough to avoid false
   positives during a legitimately slow `LoadInstrument`, short enough to catch a stall promptly.
   Needs a measurement of worst-case normal parse-iteration time, which the trace ring itself can
   supply once deployed.
1. Should `keepAlive.abortOnWedge` be available in production at all, or diagnostic builds only?
   In Mode B it would kill a user's interactive session, so it must at minimum be Mode-A-gated.
1. Distinguishing "never progressed" from "stopped progressing": a listener that wedges during
   construction leaves `bgIterations == 0`, which is also the state of a listener that has only
   just started. Needs a start timestamp in the slot, or a grace period keyed off slot claim.

### Resolved by measurement

- ~~Verify `Poco::SharedMemory`'s `server` flag semantics.~~ **Done** (§3): `true` creates and
  unlinks on destruction, `false` attaches to an existing object and throws if absent — the
  opposite of the header comment. Listener uses `true`, watchdog `false`.
- ~~Can an unprivileged process write to the journal?~~ **Yes** (§5.6), confirmed by round trip
  through the stdlib `syslog` module.
- ~~Is the mirrored Python layout safe?~~ Contract fixed and checked against the compiled struct
  (§4a): 16 / 72 / 16 / 1096 / 4400 bytes.
