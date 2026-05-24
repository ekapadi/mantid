# Sub-spec 07 — SNS: move the FSM commit into `extractData()`

## Goal

The behavioural change at the heart of v3: move the run-state transition
commit out of `runStatus()` and into Phase 1 of `extractData()`. Remove
the `SNSLiveEventDataListener::runStatus()` override; the base default
takes over. This is the commit that fixes the stand-alone `LoadLiveData`
deadlock.

## Reference

- v3 §3.2 — commit points
- v3 §5.2 — `rxPacket(RunStatusPkt)` single-slot invariant (the
  `std::runtime_error` check)
- v3 §5.3 — full `extractData()` body
- v3 §5.5 — background reader loop (unchanged but called out)
- v3 §6 — base-class deprecated `runStatus()` wrapper now handles SNS
- v3 §8 — behaviour-preservation matrix

## Scope

| File                                                               | Change                                                                                                                                                                                                                                                                                           |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` | Add the `runState()`, `isPaused()`, `listenerState()`, `lastTransition()` public overrides per v3 §4.2. Remove the `runStatus()` override declaration.                                                                                                                                           |
| `Framework/LiveData/src/SNSLiveEventDataListener.cpp`              | Implement the four pure getters per v3 §5.1. Rewrite `extractData()` per v3 §5.3 (Phase 1 dequeue → hook dispatch → set `m_lastTransition`; Phase 2/3 unchanged). Delete the `runStatus()` override. Add the single-slot invariant `std::runtime_error` to `rxPacket(RunStatusPkt)` per v3 §5.2. |

### Critical detail (C1)

Per v3 §5.3, `m_lastTransition.reset()` lives **inside** the
`if (pending)` block, not at the top of Phase 1. Calls to `extractData()`
that find nothing to commit must leave the previous `m_lastTransition`
intact so that `LoadLiveData`'s `NotYet` retry loop (`LoadLiveData.cpp:476–490`)
does not lose the edge.

### Critical detail (S4)

Per v3 §5.2, the queue-enqueue site in `rxPacket(RunStatusPkt)` raises
`std::runtime_error` (not a debug assertion) if `m_pendingTransition`
already holds a value when a second transition arrives. This is the
safety net behind the `m_pauseNetRead` back-pressure invariant.

## Tests

Adopt the full §10 v3 test suite:

| #   | Test                                                       | Source                                                                                                                                                                                          |
| --- | ---------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `test_runState_pure_getter_does_not_mutate`                | v3 §10.1 #1                                                                                                                                                                                     |
| 2   | `test_listenerState_reflects_connection_and_pause`         | v3 §10.1 #2                                                                                                                                                                                     |
| 3   | `test_lastTransition_reports_BeginRun_once`                | v3 §10.1 #3                                                                                                                                                                                     |
| 4   | `test_lastTransition_reports_EndRun_once`                  | v3 §10.1 #4                                                                                                                                                                                     |
| 5   | `test_extractData_commits_BeginRun_side_effects`           | v3 §10.1 #5                                                                                                                                                                                     |
| 6   | `test_extractData_commits_EndRun_side_effects`             | v3 §10.1 #6                                                                                                                                                                                     |
| 7   | `test_no_transition_no_hook`                               | v3 §10.1 #7                                                                                                                                                                                     |
| 8   | `test_onRunPause_invoked_for_pause_resume_markers`         | v3 §10.1 #8 (extended from sub-spec 06)                                                                                                                                                         |
| 9   | `test_isPaused_orthogonal_to_runState`                     | v3 §10.1 #9                                                                                                                                                                                     |
| 10  | `test_legacy_runStatus_returns_edge_then_state`            | v3 §10.1 #10                                                                                                                                                                                    |
| 11  | `test_background_exception_propagates_from_all_getters`    | v3 §10.1 #11                                                                                                                                                                                    |
| 12  | `test_concurrent_getters_no_data_race`                     | v3 §10.2 #12 — TSan                                                                                                                                                                             |
| 13  | `test_pending_transition_queue_invariant_violation_throws` | v3 §10.2 #13                                                                                                                                                                                    |
| 14  | `test_lastTransition_survives_NotYet_retry`                | new — explicitly proves the C1 fix: inject `NEW_RUN`, call `extractData()` before workspace is initialised (catch `NotYet`), call `extractData()` again, assert `lastTransition() == BeginRun`. |
| 15  | `test_MonitorLiveData_workspace_renaming_unchanged`        | v3 §10.3 #15                                                                                                                                                                                    |

Test 14 is the regression for the C1 fix and must be present in this
commit.

## Verification

- `ninja SNSLiveEventDataListenerTest MonitorLiveDataTest LoadLiveDataTest`
  builds.
- `./bin/SNSLiveEventDataListenerTest`, `MonitorLiveDataTest`,
  `LoadLiveDataTest` all pass.
- `pre-commit run --files <changed>`.
- `clang-tidy` clean.
- Run `ctest -R "LiveData|Listener"` — all pass.

## Done when

- `SNSLiveEventDataListener::runStatus()` no longer exists.
- All v3 §10.1 / §10.2 tests pass, plus test 14 above.
- `MonitorLiveData` behaviour is byte-identical to pre-refactor (verified by
  test 15 and existing system tests).
- Sub-spec 08's stand-alone `LoadLiveData` regression test will *also*
  pass once this commit lands — sub-spec 08 only adds the test, this
  commit fixes the bug.
