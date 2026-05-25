# Sub-spec 05 — SNS: split `m_status` field

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Mechanically split `SNSLiveEventDataListener::m_status` into the three v3
fields and rename `m_runPaused` → `m_isDasPaused`. **No behavioural change.**
At the end of this commit, the listener still does FSM work inside
`runStatus()`; it just uses three internal fields instead of one.

## Reference

- v3 §4.2 — header layout for `m_adaraRunStatus`, `m_pendingTransition`,
  `m_lastTransition`, `m_isDasPaused`
- v3 §5.2 — `rxPacket(RunStatusPkt)` field assignments (excluding the
  invariant-throw; see sub-spec 07)

## Scope

| File                                                               | Change                                                                                                                                                                                                                                                                                                                                                                                    |
| ------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` | Replace `RunStatus m_status` with `RunStatus m_adaraRunStatus`, `std::optional<RunStatus> m_pendingTransition`, `std::optional<RunStatus> m_lastTransition`. Rename `m_runPaused` → `m_isDasPaused`. Make `m_mutex` `mutable`.                                                                                                                                                            |
| `Framework/LiveData/src/SNSLiveEventDataListener.cpp`              | Replace every `m_status` write/read with the appropriate new field. Translate the current `runStatus()` logic to: read `m_pendingTransition`, perform the side effects, set `m_lastTransition`, clear the queue, write `m_adaraRunStatus`. Rename every `m_runPaused` reference. **Do not yet** introduce the named hooks (sub-spec 06) and **do not yet** move the commit (sub-spec 07). |

This commit is a pure refactor: the inputs to and outputs from
`runStatus()` are byte-identical to before. `rxPacket(RunStatusPkt)`
queues into `m_pendingTransition` instead of writing `m_status` directly;
`runStatus()` then consumes from `m_pendingTransition` exactly as the old
code consumed `m_status`.

### Explicit non-goals

- No new public API methods are implemented on the SNS subclass yet (they
  exist as base-class defaults).
- `extractData()` is the template method provided by `LiveListener`
  (sub-spec 02b); the subclass override is `doExtractData()` and is
  **untouched** in this commit.
- `AnnotationPkt` handler is updated only to write `m_isDasPaused` (rename
  only).
- No `std::runtime_error` invariant checks yet — those land in sub-spec 07.

## Tests

| Test                                              | Location                                                                                                                                     |
| ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Existing `SNSLiveEventDataListenerTest` cases     | must pass unchanged                                                                                                                          |
| `test_field_rename_does_not_break_pause_handling` | new, exercises a PAUSE/RESUME sequence and asserts events are correctly gated by `m_isDasPaused` (proves the rename did not invert the test) |

## Verification

- `ninja SNSLiveEventDataListenerTest` builds.
- `./bin/SNSLiveEventDataListenerTest` passes.
- `ctest -R "LiveData|SNS"` all pass.
- `clang-tidy` clean on the two modified files.

## Done when

- `m_status` and `m_runPaused` no longer appear in the SNS listener.
- All existing tests continue to pass.
- The diff for this commit is dominated by mechanical renames.
