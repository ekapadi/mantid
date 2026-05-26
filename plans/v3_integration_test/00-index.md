# SNSLiveEventDataListener — UDS Integration Test: Master Index

**Branch:** `EWM15431_live-listener-interface`  
**Scope:** `Framework/LiveData/test/` and `Framework/LiveData/CMakeLists.txt` only.  
**Status:** Implementation-ready (split into sub-specs).

This document is the master index.  The spec is split into five sub-spec files in the
same directory.  Read them in order; each one has its own scope statement.

| Sub-spec | File | Content |
|---|---|---|
| §0 + §1 + §2 | this file | Agent instructions, goal, file rearrangement |
| §3 | [`01-uds-transport.md`](01-uds-transport.md) | Unix-domain socket setup |
| §4 (API + threading) | [`02-mock-sms-server.md`](02-mock-sms-server.md) | `MockSMSServer` header and implementation |
| §4.3 (packet builders) | [`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md) | ADARA fixture reuse rules |
| §5 + §6 | [`04-test-scenarios.md`](04-test-scenarios.md) | Fixture skeleton + all 22 test scenarios |

CMake registration (§7), Out of Scope (§8), and Definition of Done (§9) are also in this file.

---

## 0. Agent execution instructions

Read this section first; it constrains *how* the spec is to be implemented.

1. **Base branch.** Base the PR on `EWM15431_live-listener-interface`. Do **not**
   base on `main` / `master`.

2. **Scope fence.** Touch only the files explicitly named in §2, §4, and §7.
   Do **not** modify any file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/`. Do not "fix" comments, reformat headers, or
   touch unrelated tests.

3. **Static verification only — DO NOT build, DO NOT run tests.** A full
   Mantid build takes hours and you cannot launch the test binary in the
   correct environment. Verify your changes by:
   - reading the relevant production code on this branch;
   - cross-referencing against the line ranges cited in §1 and §6 of
     [`04-test-scenarios.md`](04-test-scenarios.md);
   - checking that file names, `#include`s, `namespace` usages, and CMake
     edits are internally consistent;
   - hand-tracing the threading / lifetime rules in
     [`02-mock-sms-server.md §4.2`](02-mock-sms-server.md).

   The maintainer will perform the build and runtime verification by hand
   as part of PR review. Anywhere this spec mentions `cmake`, `ctest`,
   compiler output, or test pass/fail status, that instruction is for the
   *reviewer* — **not** for you.

4. **No build artefacts in the PR.** Do not commit `build/`, `CMakeFiles/`,
   generated headers, or compiler output.

5. **Legacy file.** The current `SNSLiveEventDataListenerTest.h` is renamed
   and **retained** as historical reference. It must remain in the tree but
   must **not** appear in `TEST_FILES`. Do not delete it.

6. **Ambiguity protocol.** If the spec is wrong, contradictory, or
   under-specified, **stop** and surface the question in the PR description.
   Do not invent a resolution silently.

7. **Flake check (§9 item 8).** Skip unless explicitly requested. It is a
   manual operator check, not a gating CI step.

8. **No production code changes.** If you believe production code must
   change for a test to pass, you have misread either the spec or the
   production contract — stop and ask.

9. **This is a real integration test — do NOT use `TestableSNSListener`.**
   The purpose of this suite is to drive the *real* `SNSLiveEventDataListener`
   through its actual `connect()` → `start()` → background-thread → `extractData()`
   lifecycle, talking to a real socket.  Do not inject mocks at the listener
   level.  Do not try to bypass the UDS connection.  If the listener has a
   `TestableSNSListener` subclass, that class is NOT used here — it exists only
   for no-network unit tests in `SNSLiveEventDataListenerNoNetworkTest.h`.

---

## 1. Goal

Replace the unregistered, network-dependent
`Framework/LiveData/test/SNSLiveEventDataListenerTest.h` with a hermetic
integration suite that drives a real `SNSLiveEventDataListener` instance
against an in-process `MockSMSServer` over a Unix-domain socket (UDS). The
suite verifies, in addition to the legacy behavioural contract:

- The v3 single-slot pending-transition invariant (sub-spec 05) at
  `SNSLiveEventDataListener.cpp:646-652, 738-743`.
- The C1 fix — `m_lastTransition` survives an `extractData()` NotYet path
  (sub-spec 06) at `SNSLiveEventDataListener.cpp:1515-1516`.
- The deferred-run-details invariant (sub-spec 07) at
  `SNSLiveEventDataListener.cpp:1562-1568` — coverage delegated to the
  existing no-network unit suite; see §6.6 of
  [`04-test-scenarios.md`](04-test-scenarios.md).
- The white-lie path on mid-run join at `SNSLiveEventDataListener.cpp:654-697`.
- Pause/resume orthogonality (`onRunPause`) at
  `SNSLiveEventDataListener.cpp:1590-1596` and the
  `SNSLiveEventDataListener.keepPausedEvents` config gate at
  `SNSLiveEventDataListener.cpp:96-99, 400-402`.

No production code is modified. The listener already accepts a UDS
`Poco::Net::SocketAddress`; see `connect()` at
`SNSLiveEventDataListener.cpp:141-156`.

---

## 2. File rearrangement

| Action | Path | Notes |
|---|---|---|
| Rename | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` → `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` | Retained as historical reference. **Not** registered in `TEST_FILES`. Add a one-line header comment: *"Legacy network-dependent test, retained for reference only; superseded by `SNSLiveEventDataListenerTest.h` (integration) and `SNSLiveEventDataListenerNoNetworkTest.h` (unit)."* |
| Create | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` | New UDS-driven integration suite. File-level Doxygen header **must** state: *"INTEGRATION TEST. Drives a real `SNSLiveEventDataListener` against an in-process `MockSMSServer` over a Unix-domain socket. Does NOT require SMS or any external network resource. Linux/macOS only — compiles to an empty suite on Windows."* |
| Create | `Framework/LiveData/test/MockSMSServer.h` | Declaration of `MockSMSServer` and the `ScriptEntry` variant. See [`02-mock-sms-server.md`](02-mock-sms-server.md). |
| Create | `Framework/LiveData/test/MockSMSServer.cpp` | Implementation (Poco socket plumbing, packet builders, script driver). See [`02-mock-sms-server.md`](02-mock-sms-server.md) and [`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md). |

Keep the existing `SNSLiveEventDataListenerNoNetworkTest.h` unchanged.

---

## 7. CMake registration

Edit `Framework/LiveData/CMakeLists.txt`:

```cmake
# In TEST_FILES (alphabetical), replace the existing comment line:
#   # Needs fixing to not rely on network. SNSLiveEventDataListenerTest.h
# with:
SNSLiveEventDataListenerTest.h
# (SNSLiveEventDataListenerLegacyTest.h is intentionally NOT registered —
#  retained as historical reference.)
```

Edit `Framework/LiveData/test/CMakeLists.txt`:

```cmake
# Add MockSMSServer.cpp to TESTHELPER_SRCS (currently line 10):
set(TESTHELPER_SRCS
    KafkaTesting.h KafkaTestThreadHelper.h
    TestDataListener.cpp TestGroupDataListener.cpp
    MockSMSServer.cpp     # <-- added
)
```

And the ctest timeout, near the existing `MonitorLiveDataTest` serial-run property:

```cmake
set_tests_properties(LiveDataTest_SNSLiveEventDataListenerTest
                     PROPERTIES TIMEOUT 120)
```

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*
> The `set_tests_properties(... TIMEOUT 120)` CMake edit *is* part of the
> agent's deliverable.  The runtime behaviour of `ctest` enforcing this
> ceiling is observable only at review time.

---

## 8. Out of scope

- **No production code change.** Sub-specs 05/06/07 are already
  implemented on this branch; this work only verifies them.
- **No replacement of the unit tests** in
  `SNSLiveEventDataListenerNoNetworkTest.h`. The integration suite
  complements, does not duplicate.
- **No Windows port.** SNS is Linux; the Windows precedent for skipped
  suites is followed.
- **No `m_deferredRunDetailsPkt` invariant test** in the integration
  suite — covered by the no-network unit suite (see §6.6 in
  [`04-test-scenarios.md`](04-test-scenarios.md)).

---

## 9. Definition of done

Items 1–4 and 7 are the **agent's deliverables** — verifiable by static
inspection of the PR.

1. `SNSLiveEventDataListenerLegacyTest.h` exists, is not registered, and
   contains the required header comment.
2. `SNSLiveEventDataListenerTest.h`, `MockSMSServer.h`,
   `MockSMSServer.cpp` exist with the contents described in sub-specs 01–04.
3. `Framework/LiveData/CMakeLists.txt` registers
   `SNSLiveEventDataListenerTest.h` in `TEST_FILES` and **not**
   `SNSLiveEventDataListenerLegacyTest.h`.
4. `Framework/LiveData/test/CMakeLists.txt` lists `MockSMSServer.cpp` in
   `TESTHELPER_SRCS` and sets the `TIMEOUT 120` property.
7. No file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/` is modified by the PR.

---

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*
> Items 5, 6, and 8 below are runtime-verifiable only, by the maintainer,
> after a local build. The coding agent has been explicitly instructed
> (§0 item 3) **not** to build or run tests.

5. *(Reviewer-verified.)* On Linux/macOS, all 22 tests pass under
   `ctest -R LiveDataTest` (the two XFAIL tests pass by asserting the
   *broken* behaviour with an inline TSM_ASSERT message — see
   [`04-test-scenarios.md §known-defects`](04-test-scenarios.md)).
6. *(Reviewer-verified.)* On Windows, the suite compiles cleanly to an
   empty `CxxTest::TestSuite` and `ctest -R LiveDataTest` passes (no SNS
   tests registered).
8. *(Reviewer-only, optional, not CI-gated.)* A deliberate-deadlock
   injection check — temporarily insert a
   `std::this_thread::sleep_for(std::chrono::hours{1})` in
   `onBeforeExtract` — must cause the affected test to fail-fast via
   the `extractWithTimeout` guard within ~10 s, not hang. Revert before
   commit. Operator-run only; **never** to be performed by the coding
   agent.

---

## 10. References

Production code (this branch, `EWM15431_live-listener-interface`):

- [`Framework/LiveData/src/SNSLiveEventDataListener.cpp`](../../Framework/LiveData/src/SNSLiveEventDataListener.cpp)
  — see line ranges cited in §1 and in [`04-test-scenarios.md`](04-test-scenarios.md).

Existing test files:

- [`Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`](../../Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h)
  — companion unit suite.
- [`Framework/LiveData/test/ADARAPackets.h`](../../Framework/LiveData/test/ADARAPackets.h)
  — binary exemplars for the §4.3 builders; see [`03-adara-packet-fixtures.md`](03-adara-packet-fixtures.md).
- [`Framework/LiveData/test/KafkaTesting.h`](../../Framework/LiveData/test/KafkaTesting.h)
  + `TestDataListener.cpp` — the `TESTHELPER_SRCS` precedent.

Config / tempfile precedent:

- [`Framework/Kernel/test/ConfigObserverTest.h`](../../Framework/Kernel/test/ConfigObserverTest.h)
  + `ConfigPropertyObserverTest.h` — save / restore config in
  `setUp` / `tearDown`.
- `Poco::TemporaryFile` usage across `Framework/*` — see `SaveGSSTest.h`,
  `SaveGDATest.h`, `SaveOpenGenieAsciiTest.h`, `DownloadFileTest.h`,
  `InternetHelperTest.h`.

Latent-defect documentation:

- [`plans/ignore-packets-defect.md`](../ignore-packets-defect.md) — analysis of the
  `m_ignorePackets` bug that makes two §6.8 tests XFAIL.

Companion design docs:

- `plans/listener_refactoring_v3.md` — the v3 refactor this suite verifies.
- `plans/v3_subspecs/` — sub-specs 05/06/07 referenced in §1.
