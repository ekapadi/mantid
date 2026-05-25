# Sub-spec 06 — SNS: extract transition hooks

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Extract the side-effect blocks currently embedded in `runStatus()` and
`rxPacket(AnnotationPkt)` into the named protected-virtual hooks
`onBeginRun()`, `onEndRun()`, `onRunPause()`. The hooks are still called
from the *same locations* as before; this commit only renames and
reorganises code.

## Reference

- v3 §4.2 — hook declarations
- v3 §5.4 — hook bodies (read carefully for cache-clear set and
  `m_dataStartTime` asymmetry)

## Scope

| File                                                               | Change                                                                                                                                                                                                                                                                       |
| ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` | Declare `protected: virtual void onBeginRun(); virtual void onEndRun(); virtual void onRunPause(bool paused);` per v3 §4.2.                                                                                                                                                  |
| `Framework/LiveData/src/SNSLiveEventDataListener.cpp`              | Implement the three hooks per v3 §5.4. Replace the BeginRun side-effect block in `runStatus()` with a call to `onBeginRun()`; same for EndRun → `onEndRun()`. In `rxPacket(AnnotationPkt)`, replace direct writes to `m_isDasPaused` with calls to `onRunPause(true/false)`. |

`runStatus()` still consumes `m_pendingTransition` and still drives the
side effects, but now does so via the named hooks.

### Important

- The exact set of cache clears in `onBeginRun()`/`onEndRun()` must match
  the current code line-for-line (compare with the pre-refactor file as
  of sub-spec 05). v3 §5.4 lists the asymmetries — in particular,
  `m_dataStartTime` is cleared on `EndRun` only, **not** on `BeginRun`.
- `onBeginRun()` per v3 §5.4 raises `std::runtime_error` when
  `!m_deferredRunDetailsPkt`. Implement that throw in this commit.
- `onRunPause()` per v3 §5.4 is documented as background-thread-only.

## Tests

| Test                                                       | Location                                                                          |
| ---------------------------------------------------------- | --------------------------------------------------------------------------------- |
| `test_onBeginRun_invoked_when_runStatus_returns_BeginRun`  | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` (subclass hook recorder) |
| `test_onEndRun_invoked_when_runStatus_returns_EndRun`      | same                                                                              |
| `test_onBeginRun_throws_when_deferred_run_details_missing` | same — invariant from v3 §5.4                                                     |
| `test_onRunPause_invoked_for_PAUSE_and_RESUME_markers`     | same — observe via subclass override                                              |
| `test_onRunPause_does_not_modify_adaraRunStatus`           | same — orthogonality assertion                                                    |
| Existing `SNSLiveEventDataListenerTest` cases              | must pass unchanged                                                               |

## Verification

- `ninja SNSLiveEventDataListenerTest` builds.
- `./bin/SNSLiveEventDataListenerTest` passes.
- `clang-tidy` clean.

## Done when

- The three hooks exist and are exercised by tests.
- No side-effect code remains directly inline in `runStatus()` or
  `rxPacket(AnnotationPkt)` — all such code is in the hooks.
- External behaviour of the SNS listener is unchanged.
