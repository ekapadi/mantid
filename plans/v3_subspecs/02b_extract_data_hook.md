# Sub-spec 02b — `extractData()` template method (`onBeforeExtract` + `doExtractData`)

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Promote the common "do something just before producing a workspace" pattern
into a Template Method on `API::LiveListener`. Introduces two protected
virtual hooks:

- `onBeforeExtract()` — runs on the foreground thread immediately before
  each extraction. Default no-op. Subclasses use it to: commit a queued
  FSM transition (SNS), advance a synthetic clock (Fake), poll a remote
  endpoint (SINQ), etc. May throw to abort the extraction.

- `doExtractData()` — pure virtual; the actual workspace-construction
  step. Replaces what subclasses currently override as `extractData()`.

`LiveListener::extractData()` becomes the single non-virtual entry point
that chains them. This codifies the pattern that v3 §5.3 (SNS Phase 1 vs
Phase 2/3), OL §3.1 (Fake), and OL §3.6 (SINQ) each derive ad hoc.

## Why this commit exists

Without the hook, sub-specs 03, 04, and 07 each instruct the implementer
to "override `extractData()` to call a private helper before the existing
body" — the same architectural pattern written three times with no shared
contract. Introducing the hook centralises the contract and the
documentation, and makes SNS Phase 1 stop feeling like a special case.

## Scope

| File                                         | Change                                                                                                                                                                                                                                          |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/API/inc/MantidAPI/LiveListener.h` | Make `extractData()` `final` here, implementing the template method body shown below. Declare `protected: virtual void onBeforeExtract(); virtual std::shared_ptr<Workspace> doExtractData() = 0;`. The default `onBeforeExtract()` is a no-op. |
| `Framework/API/src/LiveListener.cpp` (new)   | Out-of-line `extractData()` body and default `onBeforeExtract()`.                                                                                                                                                                               |
| `Framework/API/CMakeLists.txt`               | Register the new `.cpp` if not already present (sub-spec 01 may already have created it).                                                                                                                                                       |

### Template body

```cpp
std::shared_ptr<Workspace> LiveListener::extractData() {
    onBeforeExtract();
    return doExtractData();
}
```

The exception-safety contract:

- If `onBeforeExtract()` throws, `doExtractData()` is not called. The
  exception propagates to the caller (`LoadLiveData`).
- `doExtractData()` may itself throw `Exception::NotYet` (workspace not
  yet initialised), in which case any side effects performed by
  `onBeforeExtract()` are kept. This is exactly what v3 §5.3 wants for
  the C1 fix: a queued transition committed by `onBeforeExtract()` must
  survive `Exception::NotYet` thrown from `doExtractData()` so that the
  `LoadLiveData` retry loop (`LoadLiveData.cpp:476–490`) does not lose
  the edge.

### Subclass-rename pass

Every concrete listener that derives from `API::LiveListener` and
currently overrides `extractData()` must be renamed in this commit to
override `doExtractData()` instead:

- `SNSLiveEventDataListener` — `extractData()` → `doExtractData()`. The
  Phase-1/Phase-2/Phase-3 split introduced later in sub-spec 07 will
  move Phase 1 into `onBeforeExtract()`; this commit only renames the
  existing single body.
- `FakeEventDataListener` — `extractData()` → `doExtractData()`. Sub-spec
  03 will then move side-effect work into `onBeforeExtract()`.
- `SINQHMListener` — same; sub-spec 04 follows up.
- `FileEventDataListener`, `ISISLiveEventDataListener`,
  `ISISHistoDataListener`, `KafkaEventListener`, `KafkaHistoListener`,
  `TestGroupDataListener`, `TestDataListener` — rename the override.
  No `onBeforeExtract()` override is needed for these.
- `MockLiveListener` (`Framework/API/test/LiveListenerTest.h`) and any
  algorithm-test mocks deriving from `ILiveListener`/`LiveListener` —
  rename if they currently override `extractData()`.

This commit is purely mechanical for every listener: the body of each
override is unchanged; only its name and the surrounding declaration
change.

### Out-of-tree compatibility

Sealing `extractData()` is a breaking change for downstream listeners
that override it directly. Justification:

- `ILiveListener::extractData()` remains pure-virtual and unsealed for
  listeners that derive directly from the interface (rare).
- `API::LiveListener` (the practical base class) is where the seal
  applies; downstream subclasses that currently inherit from it almost
  always do so to pick up `dataReset()`, `setSpectra()`, and
  `setAlgorithm()`. Renaming `extractData() override` to
  `doExtractData() override` is a one-line fix.
- The change is announced in the release note that ships with sub-spec 09.

If at code-review time the breakage is considered unacceptable, the
fallback is:

- Leave `extractData()` non-final.
- Provide a default `extractData()` that calls the hooks but allows
  override.
- Document `onBeforeExtract`/`doExtractData` as the preferred extension
  point.

The cost of the fallback is the loss of compile-time enforcement; SNS,
Fake, and SINQ would still use the hooks idiomatically by convention.
Decision is captured in this commit's PR description.

## Tests

| Test                                                                 | Location                                                                                |
| -------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| `test_extractData_calls_onBeforeExtract_then_doExtractData`          | `Framework/API/test/LiveListenerTest.h`                                                 |
| `test_throw_in_onBeforeExtract_skips_doExtractData`                  | same                                                                                    |
| `test_throw_in_doExtractData_preserves_onBeforeExtract_side_effects` | same (the C1-style invariant; uses an integer counter incremented by `onBeforeExtract`) |
| Every existing concrete-listener test                                | must pass unchanged (proves the rename is mechanical)                                   |

## Verification

> **Build note:** Cxxtest test executables are `EXCLUDE_FROM_ALL`. Run
> `ninja AllTests` before `ctest` to ensure test binaries are rebuilt
> against any modified listener headers. See sub-spec 01 for rationale.

- `ninja Framework LiveData SINQ workbench` — succeeds, proving every
  in-tree listener has been renamed.
- `ninja AllTests` — **required** before `ctest`.
- `ctest -R "Listener|LiveData"` — all pass.
- `pre-commit run --files <changed>`.
- `clang-tidy` clean on `LiveListener.{h,cpp}`.

## Done when

- `API::LiveListener::extractData()` is the only entry point and is
  `final` (modulo the documented fallback).
- Every in-tree listener overrides `doExtractData()`, not `extractData()`.
- The template-method tests pass.
- No behavioural change at any algorithm level; the rename is mechanical.

## Effect on subsequent sub-specs

Sub-specs 03, 04, and 07 are reworded to override `onBeforeExtract()`
instead of re-wrapping `extractData()`. The semantic content is
identical; the contract is now shared and named.
