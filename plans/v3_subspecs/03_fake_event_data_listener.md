# Sub-spec 03 — `FakeEventDataListener` anti-pattern fix

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Eliminate the side effects from `FakeEventDataListener::runStatus()` and
move the periodic-`EndRun` decision into the `onBeforeExtract()` hook
introduced in sub-spec 02b.

## Reference

- OL §1 — anti-pattern review
- OL §3.1 — full code template (read as "override `onBeforeExtract()`
  instead of wrapping `extractData()`"; see "Adjustment" below)

## Adjustment versus OL §3.1

OL §3.1 was written before sub-spec 02b existed and describes the fix as
adding a private `tickRunState()` helper called from a wrapped
`extractData()`. Under sub-spec 02b that pattern is named, and the
implementation is:

- The body that OL §3.1 puts in `tickRunState()` becomes the body of
  `onBeforeExtract()`.
- The body that OL §3.1 leaves in `extractData()` is already
  `doExtractData()` (renamed mechanically in sub-spec 02b).
- The private `tickRunState()` helper is **not** added — its content
  lives directly in `onBeforeExtract()`.

All other content in OL §3.1 (new members, `runState()`/`listenerState()`/
`lastTransition()` overrides, removal of `runStatus()`) applies unchanged.

## Scope

| File                                                            | Change                                                                                                                                                                                                                                                                                                                    |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/inc/MantidLiveData/FakeEventDataListener.h` | Declare `runState()`, `listenerState()`, `lastTransition()` overrides. Add new members `m_runState`, `m_lastTransition`. Declare `protected: void onBeforeExtract() override;`. Drop the `runStatus()` declaration.                                                                                                       |
| `Framework/LiveData/src/FakeEventDataListener.cpp`              | Implement `onBeforeExtract()` with the body OL §3.1 assigns to `tickRunState()` (advance `m_nextEndRunTime`, increment `m_runNumber`, set `m_runState` / `m_lastTransition`). `doExtractData()` (renamed in 02b) is otherwise untouched. Implement the three pure getters per OL §3.1. Remove the `runStatus()` override. |

The externally-observable cadence (one `EndRun` every `m_endRunEvery`,
incrementing `m_runNumber`) is preserved; only the *trigger* moves from
"someone polled `runStatus()`" to "`extractData()` was invoked".

## Tests

| Test                                                             | Location                                                                                                   |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `test_runState_is_pure_getter` (regression for the anti-pattern) | `Framework/LiveData/test/FakeEventDataListenerTest.h`                                                      |
| `test_onBeforeExtract_advances_runNumber_at_EndRun`              | same — replaces the `extractData()`-direct test now that the side effect lives in the hook                 |
| `test_lastTransition_reports_EndRun_once`                        | same                                                                                                       |
| `test_periodic_EndRun_cadence_matches_legacy`                    | same — same expected count of `EndRun` events over a fixed wall-clock window as the pre-refactor behaviour |

The legacy-cadence test compares the *count* of EndRuns produced over a
window, not the precise wall-clock timing.

## Verification

> **Build note:** Cxxtest test executables are `EXCLUDE_FROM_ALL`. Run
> `ninja AllTests` (or at least `ninja LiveDataTest`) before
> `ctest` so stale test objects are rebuilt against any modified listener
> headers. See sub-spec 01 for rationale.

- `ninja Framework LiveData` — succeeds.
- `ninja AllTests` — **required** before `ctest`.
- `ctest -R FakeEventDataListener` passes, including the four new tests:
  - `test_runState_is_pure_getter`
  - `test_onBeforeExtract_advances_runNumber_at_EndRun`
  - `test_lastTransition_reports_EndRun_once`
  - `test_periodic_EndRun_cadence_matches_legacy`
- `pre-commit run --files \`
  `Framework/LiveData/inc/MantidLiveData/FakeEventDataListener.h \`
  `Framework/LiveData/src/FakeEventDataListener.cpp \`
  `Framework/LiveData/test/FakeEventDataListenerTest.h`

## Done when

- `FakeEventDataListener::runStatus()` no longer exists.
- `runState()` makes no mutations (asserted by the regression test).
- The periodic-`EndRun` side effect is driven from `onBeforeExtract()`,
  not from `runStatus()` and not from a private wrapper around
  `extractData()`.
- Periodic-EndRun behaviour for `LoadLiveData` + `FakeEventDataListener`
  integration test is unchanged.
