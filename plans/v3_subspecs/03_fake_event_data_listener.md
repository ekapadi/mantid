# Sub-spec 03 — `FakeEventDataListener` anti-pattern fix

## Goal

Eliminate the side effects from `FakeEventDataListener::runStatus()` and
move the periodic-`EndRun` decision into `extractData()`.

## Reference

- OL §1 — anti-pattern review
- OL §3.1 — full code template

## Scope

| File                                                            | Change                                                                                                                                                                                                                                            |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/inc/MantidLiveData/FakeEventDataListener.h` | Declare `runState()`, `listenerState()`, `lastTransition()` overrides; declare private `tickRunState()` and new members `m_runState`, `m_lastTransition`.                                                                                         |
| `Framework/LiveData/src/FakeEventDataListener.cpp`              | Implement per OL §3.1. Move the `m_nextEndRunTime` advance and `m_runNumber++` from `runStatus()` into `tickRunState()`; call `tickRunState()` from `extractData()` before producing the output workspace. Remove the old `runStatus()` override. |

The externally-observable cadence (one `EndRun` every `m_endRunEvery`,
incrementing `m_runNumber`) is preserved; only the *trigger* moves from
"someone polled `runStatus()`" to "`extractData()` was invoked".

## Tests

| Test                                                             | Location                                                                                                   |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `test_runState_is_pure_getter` (regression for the anti-pattern) | `Framework/LiveData/test/FakeEventDataListenerTest.h`                                                      |
| `test_extractData_advances_runNumber_at_EndRun`                  | same                                                                                                       |
| `test_lastTransition_reports_EndRun_once`                        | same                                                                                                       |
| `test_periodic_EndRun_cadence_matches_legacy`                    | same — same expected count of `EndRun` events over a fixed wall-clock window as the pre-refactor behaviour |

The legacy-cadence test compares the *count* of EndRuns produced over a
window, not the precise wall-clock timing.

## Verification

- `ninja FakeEventDataListenerTest` builds.
- `./bin/FakeEventDataListenerTest` passes.
- `ctest -R FakeEventDataListener` passes.
- `pre-commit run --files <changed>`.

## Done when

- `FakeEventDataListener::runStatus()` no longer exists.
- `runState()` makes no mutations (asserted by the regression test).
- Periodic-EndRun behaviour for `LoadLiveData` + `FakeEventDataListener`
  integration test is unchanged.
