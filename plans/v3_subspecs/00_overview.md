# Listener Refactoring v3 — Sequential Sub-Specs

This directory decomposes `plans/listener_refactoring_v3.md` and its
companion `plans/listener_refactoring_other_listeners.md` into a sequence
of self-contained, individually-reviewable commits. Each sub-spec is
intended to land as a single PR (or a single commit within a stacked PR
series). Each one leaves the tree green: the build succeeds, all existing
tests pass, and new tests added in that commit pass.

The sub-specs **reference** the relevant sections of the two primary
specifications rather than duplicating their content. Read those documents
alongside each sub-spec.

## Sequence

| #   | Sub-spec                                     | Scope                                                                                    |
| --- | -------------------------------------------- | ---------------------------------------------------------------------------------------- |
| 1   | `01_base_interface.md`                       | Add new pure getters to `ILiveListener` with safe defaults. No subclass changes.         |
| 2   | `02_side_effect_free_listeners.md`           | Trivial `runState()`/`listenerState()` overrides for 7 simple listeners + mocks.         |
| 3   | `03_fake_event_data_listener.md`             | Fix `FakeEventDataListener` anti-pattern: move run-number mutation into `extractData()`. |
| 4   | `04_sinq_hm_listener.md`                     | Fix `SINQHMListener` anti-pattern: move HTTP poll out of `runStatus()`.                  |
| 5   | `05_sns_split_status_field.md`               | Split `SNSLiveEventDataListener::m_status` into three fields; rename `m_runPaused`.      |
| 6   | `06_sns_extract_transition_hooks.md`         | Extract `onBeginRun`/`onEndRun`/`onRunPause`; still called from old locations.           |
| 7   | `07_sns_commit_in_extract_data.md`           | Move the FSM commit into `extractData()`; remove the `runStatus()` override.             |
| 8   | `08_load_live_data_standalone_regression.md` | Add the regression test that proves the stand-alone `LoadLiveData` deadlock is fixed.    |
| 9   | `09_tighten_interface_and_docs.md`           | Promote `listenerState()` to pure virtual; release notes; deprecation messaging.         |

## Dependency invariants

- Each sub-spec depends only on the immediately previous one (linear DAG).
- Sub-specs 3 and 4 may be reordered relative to each other but must come
  after sub-spec 1 and before sub-spec 9.
- Sub-spec 8 must follow sub-spec 7 (the fix it tests for).
- Sub-spec 9 must be last: it tightens the API in ways that depend on every
  prior listener override being in place.

## Cross-references

Whenever a sub-spec says "see v3 §X.Y" it means a section of
`plans/listener_refactoring_v3.md`. "See OL §X.Y" means a section of
`plans/listener_refactoring_other_listeners.md`.
