# Sub-spec 08 — Stand-alone `LoadLiveData` regression test

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Add the integration regression test that proves the original motivating
bug is fixed: stand-alone `LoadLiveData` (i.e. *not* driven by
`MonitorLiveData`) completes successfully against the SNS listener after
a run-state boundary, without deadlocking.

## Reference

- v3 §2 — problem statement
- v3 §7.1 — `LoadLiveData` impact analysis
- v3 §10.3 — integration test #14

## Scope

| File                                                                                       | Change                                                                                                                                                                                                            |
| ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/test/LoadLiveDataTest.h`                                               | Add `test_standalone_LoadLiveData_no_deadlock_after_run_boundary` per v3 §10.3 #14.                                                                                                                               |
| `Framework/LiveData/test/MockSNSLiveListenerFixture.{h,cpp}` (new, if not already present) | Test fixture that exposes a subclass of `SNSLiveEventDataListener` whose `rxPacket` entry points can be driven directly from the test thread, simulating a `NEW_RUN` boundary without requiring an actual socket. |

The test:

1. Instantiate the fixture and have the test thread inject a `NEW_RUN`
   `RunStatusPkt`.
1. Invoke `LoadLiveData` (stand-alone, *not* via `MonitorLiveData`).
1. Assert the algorithm completes within 5 s.
1. Assert the produced output workspace is non-null and has the expected
   run number.

Run the test with a wall-clock timeout watchdog so that a regression
(re-introduced deadlock) fails fast rather than hanging CI.

## Tests

This sub-spec **is** a test. There is no production change in this commit.

## Verification

- `ninja LoadLiveDataTest` builds.
- `./bin/LoadLiveDataTest test_standalone_LoadLiveData_no_deadlock_after_run_boundary`
  passes (within the watchdog window).
- The test would have failed prior to sub-spec 07 — confirm by reverting
  sub-spec 07 locally and re-running; expect a timeout/deadlock. Restore
  sub-spec 07. (Optional smoke check; do not commit the revert.)

## Done when

- The new test exists and passes.
- The test is registered in `Framework/LiveData/test/CMakeLists.txt`.
- CI watchdog times out cleanly on regression rather than hanging.
