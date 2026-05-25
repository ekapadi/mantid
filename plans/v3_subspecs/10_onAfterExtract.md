# Sub-spec 10 — `onAfterExtract()` post-hook: eliminate the `m_lastExtractSucceeded` bit

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.
> "sub-spec NN" refers to a sibling sub-spec in this directory.

## Goal

Eliminate the `SNSLiveEventDataListener::m_lastExtractSucceeded` bit by
structural means rather than encoded state. Promote the
`API::LiveListener::extractData()` template method (introduced in
sub-spec 02b) so that, in addition to its existing `onBeforeExtract()`
*pre-hook*, it also invokes an `onAfterExtract()` *post-hook* — and only
on the **success path** (i.e., `doExtractData()` returned a workspace
without throwing).

The SNS listener then moves its post-extract bookkeeping — clearing
`m_lastTransition` after a successful workspace hand-off — out of the
top of `onBeforeExtract()` and into an `onAfterExtract()` override.
The cross-call success bit (`m_lastExtractSucceeded`) is deleted.

## Why this commit exists

After sub-spec 07 landed, `SNSLiveEventDataListener::onBeforeExtract()`
opens with this block:

```cpp
if (m_lastExtractSucceeded) {
  std::lock_guard<std::mutex> scopedLock(m_mutex);
  m_lastTransition.reset();
  m_lastExtractSucceeded = false;
}
```

and `doExtractData()` ends with:

```cpp
m_lastExtractSucceeded = true;
return temp;
```

This is a hand-rolled "did the previous call succeed?" flag whose sole
purpose is to defer the `m_lastTransition.reset()` until *after* a
successful workspace hand-off, while keeping `m_lastTransition` intact
across `Exception::NotYet` throws from `doExtractData()` (the C1 fix
from v3 §5.3 / sub-spec 07 critical-detail C1).

The flag is a workaround for the absence of a post-success hook in the
template method. The pre-hook (`onBeforeExtract`) runs unconditionally;
there is no place to run code *only* when `doExtractData()` returned
normally. So SNS encodes "did we return normally?" as a member
variable and reads it back on the next entry. This:

- duplicates the control flow already present in the template-method
  contract (`onBeforeExtract` → `doExtractData` → ?);
- splits a single logical responsibility — "manage the
  `m_lastTransition` edge lifecycle" — across two functions that must
  be kept in sync;
- couples Phase 1 (pre-extract) bookkeeping to Phase 3 (post-extract)
  success, which the template method already knows about;
- requires every new listener that wants similar
  "only-on-success" behaviour to re-derive the same pattern.

Adding `onAfterExtract()` to the template method closes the contract
gap. The C1 fix then expresses itself directly: "clear the edge after
a successful extract" — no flag, no cross-call state, no mutex acquisition
just to read a `bool`.

## Reference

- v3 §5.3 — `extractData()` body, Phase 1 / Phase 2 / Phase 3 split.
  Phase 3 in v3 §5.3 is exactly what this sub-spec promotes into a
  named hook.
- v3 §5.3 critical-detail C1 — `m_lastTransition` must survive
  `Exception::NotYet` from `doExtractData()`. The post-hook contract
  below codifies this directly: the hook only runs on the success path,
  so a `NotYet` throw automatically preserves `m_lastTransition`.
- Sub-spec 02b — `extractData()` template method, `onBeforeExtract()`
  + `doExtractData()` introduction. This sub-spec extends 02b's
  contract symmetrically.
- Sub-spec 06 — `onBeginRun`/`onEndRun`/`onRunPause` hook style;
  `onAfterExtract()` follows the same naming and exception conventions.
- Sub-spec 07 — `onBeforeExtract()` body that this commit shrinks.

## Scope

| File                                                              | Change                                                                                                                                                                                                                                                                            |
| ----------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/API/inc/MantidAPI/LiveListener.h`                      | Declare `protected: virtual void onAfterExtract();` with the documented contract below. Update the `extractData()` template-method docblock to describe the three-phase contract (pre → do → after) and the on-success-only condition for `onAfterExtract()`.                     |
| `Framework/API/src/LiveListener.cpp`                              | Rewrite the `extractData()` body to call `onAfterExtract()` after `doExtractData()` returns normally. Provide a default no-op `onAfterExtract()` definition. Do **not** use `try`/`catch` to gate the call — let an exception from `doExtractData()` propagate without firing the post-hook (see "Exception safety" below). |
| `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h` | Declare `protected: void onAfterExtract() override;`. Delete the `m_lastExtractSucceeded` field. Update the docblock on `m_lastTransition` to reference `onAfterExtract()` instead of `m_lastExtractSucceeded`.                                                                |
| `Framework/LiveData/src/SNSLiveEventDataListener.cpp`             | Remove the `if (m_lastExtractSucceeded) { … m_lastTransition.reset(); … }` block from the top of `onBeforeExtract()`. Remove the `m_lastExtractSucceeded = true;` line from `doExtractData()`. Add a new `onAfterExtract()` override whose body is `lock m_mutex; m_lastTransition.reset();`. |

### Template body (after this commit)

```cpp
std::shared_ptr<Workspace> LiveListener::extractData() {
    onBeforeExtract();
    auto ws = doExtractData();  // may throw — onAfterExtract not called
    onAfterExtract();
    return ws;
}
```

### Exception-safety contract (extended from sub-spec 02b)

- If `onBeforeExtract()` throws, neither `doExtractData()` nor
  `onAfterExtract()` runs. The exception propagates.
- If `doExtractData()` throws (e.g., `Exception::NotYet`),
  `onAfterExtract()` is **not** called. Any side effects performed by
  `onBeforeExtract()` are preserved (unchanged from 02b). This is the
  structural expression of the C1 invariant.
- If `onAfterExtract()` throws, the workspace produced by
  `doExtractData()` is lost (the named-return-value is destroyed during
  stack unwind). Subclasses must therefore keep `onAfterExtract()`
  side-effects narrow and non-throwing in the steady state. The default
  implementation is a no-op and trivially satisfies this.
- `onAfterExtract()` runs on the foreground thread, in the same
  thread-context as `onBeforeExtract()` and `doExtractData()`.

The choice **not** to wrap `doExtractData()` in `try`/`catch` is
deliberate: it preserves the existing exception propagation path
(important for `Exception::NotYet`, which `LoadLiveData` catches and
retries — see `Framework/LiveData/src/LoadLiveData.cpp:476–490`) and
keeps the template body trivially readable. The "on-success-only"
semantics fall out naturally from straight-line control flow.

### SNS subclass changes

`onBeforeExtract()` shrinks to its essential responsibility — dequeue
the pending transition and dispatch:

```cpp
void SNSLiveEventDataListener::onBeforeExtract() {
  // (No m_lastExtractSucceeded gating — moved to onAfterExtract.)

  std::optional<RunStatus> pending;
  {
    std::lock_guard<std::mutex> scopedLock(m_mutex);
    pending = m_pendingTransition;
    m_pendingTransition.reset();
  }

  if (!pending)
    return;

  if (*pending == BeginRun)
    onBeginRun();
  else if (*pending == EndRun)
    onEndRun();

  {
    std::lock_guard<std::mutex> scopedLock(m_mutex);
    m_lastTransition = pending;
  }

  m_pauseNetRead = false;
}
```

`doExtractData()` loses its trailing `m_lastExtractSucceeded = true;`
assignment.

`onAfterExtract()` becomes:

```cpp
void SNSLiveEventDataListener::onAfterExtract() {
  // The template method only calls us when doExtractData() returned a
  // workspace without throwing.  At this point the caller has the
  // committed transition in hand (either via the workspace contents or
  // via a subsequent lastTransition() query), so the edge has been
  // delivered and may be cleared.  If doExtractData() had thrown
  // Exception::NotYet, we would not be here, and m_lastTransition would
  // remain intact for the LoadLiveData retry loop (C1 fix).
  std::lock_guard<std::mutex> scopedLock(m_mutex);
  m_lastTransition.reset();
}
```

### Field deletion

Delete from `SNSLiveEventDataListener.h`:

```cpp
/// Cleared in onBeforeExtract() only when the previous doExtractData() …
bool m_lastExtractSucceeded{false};
```

A grep for `m_lastExtractSucceeded` after this commit must return zero
hits across the tree.

### Effect on the C1 fix

The C1 invariant is now an immediate consequence of the template-method
contract rather than an inductive argument over the flag's value across
calls. The C1-regression test introduced in sub-spec 07 (test #14,
`test_lastTransition_survives_NotYet_retry`) continues to pass without
modification, because the observable behaviour — `lastTransition()`
returning the previous edge after a `NotYet` throw — is unchanged.

### Other listeners

No other in-tree listener currently needs `onAfterExtract()`. Concrete
listeners (`FakeEventDataListener`, `SINQHMListener`,
`FileEventDataListener`, `ISISLiveEventDataListener`,
`ISISHistoDataListener`, `KafkaEventListener`, `KafkaHistoListener`,
`TestGroupDataListener`, `TestDataListener`, `MockLiveListener`) inherit
the default no-op and require **no source changes**. This sub-spec is
purely additive at the base-class level and surgical at the SNS level.

### Out-of-tree compatibility

Adding a virtual hook with a default no-op implementation is
source- and ABI-compatible for any downstream listener that does not
already define a member named `onAfterExtract`. Downstream subclasses
that happen to have an unrelated `onAfterExtract` method will get a
benign signature mismatch (no `override`, no warning unless they also
add `override`). The release note from sub-spec 09 is updated to
mention the new hook.

## Tests

| Test                                                                                | Location                                                                                                  |
| ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `test_extractData_calls_hooks_in_order_pre_do_after`                                | `Framework/API/test/LiveListenerTest.h` — extend the existing template-method tests from sub-spec 02b     |
| `test_onAfterExtract_not_called_when_doExtractData_throws_NotYet`                   | same                                                                                                      |
| `test_onAfterExtract_not_called_when_onBeforeExtract_throws`                        | same                                                                                                      |
| `test_throw_from_onAfterExtract_propagates_and_workspace_is_dropped`                | same — documents the contract on `onAfterExtract` exceptions                                              |
| `test_lastTransition_cleared_after_successful_extract`                              | `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h` — replaces the implicit assertion previously made via `m_lastExtractSucceeded` |
| `test_lastTransition_survives_NotYet_retry`                                         | already present from sub-spec 07 (test #14); must continue to pass **unchanged**                          |
| `test_no_lastExtractSucceeded_symbol`                                               | optional grep-style check (or simply: code-review check that the field is gone)                           |
| All existing `LiveListenerTest`, `SNSLiveEventDataListenerNoNetworkTest`, `SNSLiveEventDataListenerTest`, `MonitorLiveDataTest`, `LoadLiveDataTest` cases | must pass unchanged                                                                                       |

## Verification

> **Build note:** Cxxtest test executables are `EXCLUDE_FROM_ALL`. Run
> `ninja AllTests` before `ctest` so stale test objects are rebuilt
> against the modified `LiveListener.h`. See sub-spec 01 for rationale.

> **SNS network test:** The SNS-specific cleared-after-success assertion
> lives in `SNSLiveEventDataListenerNoNetworkTest`, which runs under
> ctest. The network-bound `SNSLiveEventDataListenerTest` is excluded
> from ctest and only needs to compile.

```bash
# From the build directory (with pixi environment active):
ninja Framework LiveData SINQ workbench
ninja AllTests
ctest -R "Listener|LiveData"
```

- `ninja Framework LiveData SINQ workbench` — succeeds; the base
  `LiveListener.h` change recompiles every in-tree listener.
- `ninja AllTests` — **required** before `ctest`.
- `ctest -R "Listener|LiveData"` — all pass, including the four new
  template-method tests above and the SNS-level
  `test_lastTransition_cleared_after_successful_extract`.
- `grep -rn m_lastExtractSucceeded Framework/` returns nothing.
- `pre-commit run --files \`
  `Framework/API/inc/MantidAPI/LiveListener.h \`
  `Framework/API/src/LiveListener.cpp \`
  `Framework/API/test/LiveListenerTest.h \`
  `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h \`
  `Framework/LiveData/src/SNSLiveEventDataListener.cpp \`
  `Framework/LiveData/test/SNSLiveEventDataListenerNoNetworkTest.h`
- `clang-tidy` clean on the four modified production files.

## Done when

- `API::LiveListener::extractData()` invokes `onAfterExtract()` after
  `doExtractData()` returns normally, and not otherwise.
- `API::LiveListener::onAfterExtract()` exists as a protected virtual
  hook with a default no-op implementation.
- `SNSLiveEventDataListener::m_lastExtractSucceeded` is deleted; no
  reference to it remains in the tree.
- `SNSLiveEventDataListener::onBeforeExtract()` no longer reads or
  writes any cross-call success flag; its top-of-function "clear
  m_lastTransition" block is gone.
- `SNSLiveEventDataListener::doExtractData()` no longer writes any
  cross-call success flag.
- `SNSLiveEventDataListener::onAfterExtract()` clears
  `m_lastTransition` under `m_mutex`.
- The C1 regression test (`test_lastTransition_survives_NotYet_retry`,
  introduced in sub-spec 07) continues to pass unchanged.
- The new template-method ordering and on-success-only tests pass.
- `MonitorLiveData` and stand-alone `LoadLiveData` behaviour is
  byte-identical to the post-sub-spec-08 baseline (verified by existing
  tests, including sub-spec 08's stand-alone regression test).

## Effect on subsequent sub-specs

- Sub-spec 09 (`09_tighten_interface_and_docs.md`) should be updated in
  passing to (a) mention `onAfterExtract()` in the release-note section
  alongside `onBeforeExtract()`/`doExtractData()`, and (b) reference
  the three-phase template-method contract in the public docstring for
  `extractData()`.
- No other sub-spec is affected. Sub-specs 03 (Fake), 04 (SINQ), and
  any future listener that wants per-extract bookkeeping now have a
  named, symmetric extension point in addition to `onBeforeExtract()`.
