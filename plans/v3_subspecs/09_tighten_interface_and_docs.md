# Sub-spec 09 — Tighten interface, release notes, docs

## Goal

Final pass: tighten `ILiveListener::listenerState()` to pure virtual now
that every concrete listener overrides it (per v3 §4.1), add release
notes, and add migration documentation for downstream listener authors.

## Reference

- v3 §4.1 — final base-class shape
- v3 §6 — backward-compatibility messaging
- v3 §11 — implementation plan, step 6

## Scope

### Production

| File                                          | Change                                                                                                                                                                                 |
| --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/API/inc/MantidAPI/ILiveListener.h` | Change `virtual ListenerState listenerState() const { return ListenerState::Disconnected; }` (added in sub-spec 01) to `virtual ListenerState listenerState() const = 0;` per v3 §4.1. |

This change is safe in this commit because sub-specs 02 / 03 / 04 / 07
have already added the override to every concrete listener in the tree.

### Documentation

| File                                                                | Change                                                                                                                                                                                                 |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `docs/source/release/v6.x/Framework/LiveData/Bugfixes/<id>.rst`     | "`LoadLiveData` no longer deadlocks when used stand-alone after a run-state boundary."                                                                                                                 |
| `docs/source/release/v6.x/Framework/LiveData/New_features/<id>.rst` | "`ILiveListener` now exposes pure-getter state queries (`runState`, `isPaused`, `listenerState`, `lastTransition`); see migration guide."                                                              |
| `dev-docs/source/LiveListener.rst` (new or updated)                 | Brief migration guide for downstream listener authors: how to map an existing `runStatus()` to the new API. Reference `plans/listener_refactoring_other_listeners.md` as the worked-examples document. |

### Optional clean-up (may be deferred)

| File                                         | Change                                                                                                                                                                                                   |
| -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/LiveData/src/MonitorLiveData.cpp` | Replace the call to `listener->runStatus()` at line 191 with `listener->lastTransition().value_or(listener->runState())`. Pure code clean-up; the deprecated `runStatus()` continues to work either way. |

Mark this as **optional** in the commit message — it is the obvious
follow-up but does not affect correctness.

## Tests

No new tests are required for the API tightening (every listener already
overrides `listenerState()`; the change is enforced at compile time).

For the optional `MonitorLiveData` cleanup, the existing
`MonitorLiveDataTest` cases (and the v3 §10.3 #15 test introduced in
sub-spec 07) cover behaviour.

## Verification

- `ninja Framework LiveData SINQ` — succeeds (proves every concrete
  listener overrides `listenerState()`).
- `ctest -R "LiveData|Listener|SINQ"` — all pass.
- `make docs-html` (or pixi equivalent) — succeeds; new release notes
  render.
- `pre-commit run --all-files` — clean.

## Done when

- `listenerState()` is pure virtual on `ILiveListener`.
- Release notes are present in the next-release directory.
- Migration guide exists in `dev-docs/`.
- (Optional) `MonitorLiveData` uses the new pure-getter API.

## Post-commit follow-ups (not part of this PR)

- In a future release: remove the `[[deprecated]]` `runStatus()` entirely.
  Track via an issue created at this commit.
