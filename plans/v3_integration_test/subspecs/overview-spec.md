# SNSLiveEventDataListener — UDS Integration Test: Overview

**Branch:** `EWM15431_live-listener-interface__agents`
**Scope:** `Framework/LiveData/test/` and `Framework/LiveData/CMakeLists.txt` only.
**Status:** Implementation-ready, broken out into a sequence of single-commit
implementation sub-specs (`subspec01-…` through `subspec06-…`).

This document is the **single primary spec** that every implementation
sub-spec in this directory may reference. Each implementation sub-spec is
otherwise self-contained: it embeds the §0 agent restrictions, all
file-level details required to produce its commit, and the exact test
contents (where applicable). An agent should be able to be pointed at
`overview-spec.md` + a single `subspecNN-….md` and "implement that".

______________________________________________________________________

## 0. Agent execution instructions

Read this section first; it constrains *how* every sub-spec is to be
implemented. **These restrictions are restated at the top of every
implementation sub-spec — they apply to every commit.**

1. **Base branch.** Base the PR on `EWM15431_live-listener-interface__agents`. Do
   **not** base on `main` / `master`.

1. **Scope fence.** Touch only the files explicitly named in §2 below, in
   the file-list of each sub-spec, and in §7 (CMake). Do **not** modify
   any file under `Framework/LiveData/src/` or `Framework/LiveData/inc/`.
   Do not "fix" comments, reformat headers, or touch unrelated tests.

1. **Static verification only — DO NOT build, DO NOT run tests.** A full
   Mantid build takes hours and you cannot launch the test binary in the
   correct environment. Verify your changes by:

   - reading the relevant production code on this branch;
   - cross-referencing against the line ranges cited in §1 and in each
     test description;
   - checking that file names, `#include`s, `namespace` usages, and
     CMake edits are internally consistent;
   - hand-tracing the threading / lifetime rules in `subspec02`.

   The maintainer will perform the build and runtime verification by
   hand as part of PR review. Anywhere any sub-spec mentions `cmake`,
   `ctest`, compiler output, or test pass/fail status, that instruction
   is for the *reviewer* — **not** for you.

1. **No build artefacts in the PR.** Do not commit `build/`,
   `CMakeFiles/`, generated headers, or compiler output.

1. **Legacy file.** The current `SNSLiveEventDataListenerTest.h` is
   renamed and **retained** as historical reference (see `subspec01`).
   It must remain in the tree but must **not** appear in `TEST_FILES`.
   Do not delete it.

1. **Ambiguity protocol.** If a sub-spec is wrong, contradictory, or
   under-specified, **stop** and surface the question in the PR
   description. Do not invent a resolution silently.

1. **Flake check.** Skip unless explicitly requested. It is a manual
   operator check, not a gating CI step.

1. **No production code changes.** If you believe production code must
   change for a test to pass, you have misread either the spec or the
   production contract — stop and ask.

1. **This is a real integration test — do NOT use `TestableSNSListener`.**
   The purpose of this suite is to drive the *real*
   `SNSLiveEventDataListener` through its actual
   `connect()` → `start()` → background-thread → `extractData()`
   lifecycle, talking to a real socket. Do not inject mocks at the
   listener level. Do not try to bypass the UDS connection. If the
   listener has a `TestableSNSListener` subclass, that class is NOT used
   here — it exists only for no-network unit tests in
   `SNSLiveEventDataListenerNoNetworkTest.h`.

1. **One commit per sub-spec.** Each `subspecNN-…` file in this
   directory corresponds to exactly one commit on the integration-test
   PR. Implement sub-specs in numerical order; do not reorder.

______________________________________________________________________

## 1. Goal

Replace the unregistered, network-dependent
`Framework/LiveData/test/SNSLiveEventDataListenerTest.h` with a hermetic
integration suite that drives a real `SNSLiveEventDataListener` instance
against an in-process `MockSMSServer` over a Unix-domain socket (UDS).
The suite verifies, in addition to the legacy behavioural contract:

- The v3 single-slot pending-transition invariant at
  `SNSLiveEventDataListener.cpp:651-656, 743-750` (BeginRun and
  EndRun sides respectively).
- The **C1 fix** — `m_lastTransition` survives an `extractData()`
  NotYet path. The guarantee is structural:
  `API::LiveListener::extractData()` (`Framework/API/src/LiveListener.cpp:16-21`)
  does not call `onAfterExtract()` when `doExtractData()` throws
  `NotYet`, so the reset at `SNSLiveEventDataListener.cpp:1566` is
  bypassed and the slot set in `onBeforeExtract()` at `:1555` survives.
- The **deferred-run-details invariant** at
  `SNSLiveEventDataListener.cpp:1601-1609` (`onBeginRun()` enforces
  that `m_deferredRunDetailsPkt` is non-null before consuming it; the
  deferral is set up in `rxPacket(RunStatusPkt)` at `:711-723`) —
  coverage delegated to the existing no-network unit suite (see
  `subspec06`).
- The white-lie path on mid-run join at
  `SNSLiveEventDataListener.cpp:658-704`.
- Pause/resume orthogonality (`onRunPause`) at
  `SNSLiveEventDataListener.cpp:1629-1635` and the
  `SNSLiveEventDataListener.keepPausedEvents` config gate at
  `SNSLiveEventDataListener.cpp:96-99, 405`.

**No production code is modified.** The listener already accepts a UDS
`Poco::Net::SocketAddress`; see `connect()` at
`SNSLiveEventDataListener.cpp:141-156`.

______________________________________________________________________

## 2. File rearrangement (cumulative across all sub-specs)

| Action    | Path                                                                                                                      | Introduced in                         |
| --------- | ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------- |
| Rename    | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` → `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` | `subspec01`                           |
| Create    | `Framework/LiveData/test/MockSMSServer.h`                                                                                 | `subspec02`                           |
| Create    | `Framework/LiveData/test/MockSMSServer.cpp`                                                                               | `subspec02`                           |
| Create    | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (new, UDS-driven)                                                | `subspec03`                           |
| Edit      | `Framework/LiveData/test/CMakeLists.txt` (TESTHELPER_SRCS)                                                                | `subspec02`                           |
| Edit      | `Framework/LiveData/CMakeLists.txt` (TEST_FILES)                                                                          | `subspec03`                           |
| Edit      | `Framework/LiveData/test/CMakeLists.txt` (`TIMEOUT 120`)                                                                  | `subspec03`                           |
| Add tests | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (new)                                                            | `subspec04`, `subspec05`, `subspec06` |

Existing `SNSLiveEventDataListenerNoNetworkTest.h` is **unchanged**.

______________________________________________________________________

## 3. Sub-spec sequence

| #   | File                                     | Purpose                                                                                                                                            | Files touched                                                                        |
| --- | ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| 1   | `subspec01-rename-legacy-test.md`        | Rename legacy test + add header comment                                                                                                            | rename only                                                                          |
| 2   | `subspec02-mock-sms-server.md`           | Create `MockSMSServer.{h,cpp}` (UDS transport, threading, watchdog, packet builders); register in `TESTHELPER_SRCS`                                | + `MockSMSServer.h`, `MockSMSServer.cpp`, `test/CMakeLists.txt`                      |
| 3   | `subspec03-integration-test-scaffold.md` | Create new `SNSLiveEventDataListenerTest.h` (fixture + helpers + Windows stub + one placeholder test); register in `TEST_FILES`; set `TIMEOUT 120` | + `SNSLiveEventDataListenerTest.h`, `LiveData/CMakeLists.txt`, `test/CMakeLists.txt` |
| 4   | `subspec04-tests-lifecycle.md`           | Add 8 tests: §6.1 remainder (3), §6.2 (2), §6.3 (3)                                                                                                | edit `SNSLiveEventDataListenerTest.h`                                                |
| 5   | `subspec05-tests-invariants.md`          | Add 6 tests: §6.4 (1), §6.5 (2), §6.7 (3)                                                                                                          | edit `SNSLiveEventDataListenerTest.h`                                                |
| 6   | `subspec06-tests-error-and-xfail.md`     | Add 7 tests: §6.8 XFAIL (2), §6.9 (3), §6.10 (2)                                                                                                   | edit `SNSLiveEventDataListenerTest.h`                                                |

Implementation order is fixed. After `subspec03` the suite compiles and
registers one test; each subsequent sub-spec only adds new `void test_…`
methods to the same fixture.

______________________________________________________________________

## 4. CMake registration (cumulative — for reference)

`subspec02` and `subspec03` together produce the following final state.
Each sub-spec edits only its own portion; do not jump ahead.

### 4.1 `Framework/LiveData/CMakeLists.txt`

In `TEST_FILES` (alphabetical), replace the existing comment line:

```cmake
# Needs fixing to not rely on network. SNSLiveEventDataListenerTest.h
```

with:

```cmake
SNSLiveEventDataListenerTest.h
# (SNSLiveEventDataListenerLegacyTest.h is intentionally NOT registered —
#  retained as historical reference.)
```

(This edit happens in `subspec03`.)

### 4.2 `Framework/LiveData/test/CMakeLists.txt`

Add `MockSMSServer.cpp` to `TESTHELPER_SRCS` (currently around line 10):

```cmake
set(TESTHELPER_SRCS
    KafkaTesting.h KafkaTestThreadHelper.h
    TestDataListener.cpp TestGroupDataListener.cpp
    MockSMSServer.cpp     # <-- added
)
```

(This edit happens in `subspec02`.)

And the ctest timeout, near the existing `MonitorLiveDataTest` serial-run
property:

```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```

(This edit happens in `subspec03`.)

> **Notes for reviewer:** *\[ agent should not run `ctest`! \]*
> The `set_tests_properties(... TIMEOUT 120)` CMake edit *is* part of the
> agent's deliverable. The runtime behaviour of `ctest` enforcing this
> ceiling is observable only at review time.

______________________________________________________________________

## 5. Out of scope

- **No production code change.** The v3 sub-specs 05/06/07 are already
  implemented on this branch; this work only verifies them.
- **No replacement of the unit tests** in
  `SNSLiveEventDataListenerNoNetworkTest.h`. The integration suite
  complements, does not duplicate.
- **No Windows port.** SNS is Linux; the Windows precedent for skipped
  suites is followed (the suite compiles to an empty `CxxTest::TestSuite`
  on Windows).
- **No `m_deferredRunDetailsPkt` invariant test** in the integration
  suite — covered by the no-network unit suite.

______________________________________________________________________

## 6. Definition of done

Items 1–4 and 7 are the **agent's deliverables** — verifiable by static
inspection of the PR.

1. `SNSLiveEventDataListenerLegacyTest.h` exists, is not registered, and
   contains the required header comment (`subspec01`).
1. `SNSLiveEventDataListenerTest.h`, `MockSMSServer.h`,
   `MockSMSServer.cpp` exist with the contents described in
   `subspec02`–`subspec06`.
1. `Framework/LiveData/CMakeLists.txt` registers
   `SNSLiveEventDataListenerTest.h` in `TEST_FILES` and **not**
   `SNSLiveEventDataListenerLegacyTest.h` (`subspec03`).
1. `Framework/LiveData/test/CMakeLists.txt` lists `MockSMSServer.cpp` in
   `TESTHELPER_SRCS` (`subspec02`) and sets the `TIMEOUT 120` property
   (`subspec03`).
1. No file under `Framework/LiveData/src/` or `Framework/LiveData/inc/`
   is modified by any commit on the PR.

> **Notes for reviewer:** *\[ agent should not run `ctest`! \]*
> Items 5, 6, and 8 below are runtime-verifiable only, by the maintainer,
> after a local build. The coding agent has been explicitly instructed
> (§0 item 3) **not** to build or run tests.

5. *(Reviewer-verified.)* On Linux/macOS, all 22 tests pass under
   `ctest -R LiveDataTest` (the two XFAIL tests pass by asserting the
   *broken* behaviour with an inline `TSM_ASSERT` message — see
   `subspec06`).
1. *(Reviewer-verified.)* On Windows, the suite compiles cleanly to an
   empty `CxxTest::TestSuite` and `ctest -R LiveDataTest` passes (no SNS
   tests registered).
1. *(Reviewer-only, optional, not CI-gated.)* A deliberate-deadlock
   injection check — temporarily insert a
   `std::this_thread::sleep_for(std::chrono::hours{1})` in
   `onBeforeExtract` — must cause the affected test to fail-fast via
   the `extractWithTimeout` guard within ~10 s, not hang. Revert before
   commit. Operator-run only; **never** to be performed by the coding
   agent. Note: the same probe inserted into `onAfterExtract` tests the
   same guard for the EndRun dispatch path (EndRun is dispatched from
   `onAfterExtract`, not `onBeforeExtract`).

______________________________________________________________________

## 7. References

Production code (this branch, `EWM15431_live-listener-interface__agents`):

- [`Framework/LiveData/src/SNSLiveEventDataListener.cpp`](../../../Framework/LiveData/src/SNSLiveEventDataListener.cpp)
  — see line ranges cited in §1 and in each sub-spec's test descriptions.

Existing test files:

- [`Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`](../../../Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h)
  — companion unit suite.
- [`Framework/LiveData/test/ADARAPackets.h`](../../../Framework/LiveData/test/ADARAPackets.h)
  — binary exemplars (SHA `42ebd4dcac52e69546e31037e9d8ee9be33c7673` on
  this branch) used by `subspec02`.
- [`Framework/LiveData/test/KafkaTesting.h`](../../../Framework/LiveData/test/KafkaTesting.h)
  - `TestDataListener.cpp` — the `TESTHELPER_SRCS` precedent.

Config / tempfile precedent:

- [`Framework/Kernel/test/ConfigObserverTest.h`](../../../Framework/Kernel/test/ConfigObserverTest.h)
  - `ConfigPropertyObserverTest.h` — save / restore config in
    `setUp` / `tearDown`.
- `Poco::TemporaryFile` usage across `Framework/*` — see `SaveGSSTest.h`,
  `SaveGDATest.h`, `SaveOpenGenieAsciiTest.h`, `DownloadFileTest.h`,
  `InternetHelperTest.h`.

Latent-defect documentation:

- [`plans/ignore-packets-defect.md`](../../ignore-packets-defect.md) —
  analysis of the `m_ignorePackets` bug that makes two `subspec06` tests
  XFAIL.

Companion design docs:

- `plans/listener_refactoring_v3.md` — the v3 refactor this suite
  verifies.
- `plans/v3_subspecs/` — sub-specs 05/06/07 referenced in §1.
