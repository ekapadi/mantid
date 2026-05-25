# Sub-spec 04 — `SINQHMListener` anti-pattern fix

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Move the HTTP poll and `dimDirty` / `hmhost` mutations out of
`SINQHMListener::runStatus()` and into the `onBeforeExtract()` hook
introduced in sub-spec 02b.

## Reference

- OL §1 — anti-pattern review
- OL §3.6 — full code template (read as "override `onBeforeExtract()`
  instead of wrapping `extractData()`"; see "Adjustment" below)

## Adjustment versus OL §3.6

OL §3.6 was written before sub-spec 02b existed and describes the fix as
adding a private `pollStatus()` helper called from a wrapped
`extractData()`. Under sub-spec 02b that pattern is named, and the
implementation is:

- The body that OL §3.6 puts in `pollStatus()` becomes the body of
  `onBeforeExtract()`.
- The body that OL §3.6 leaves in `extractData()` (the dimension loading
  and workspace construction) is already `doExtractData()` (renamed
  mechanically in sub-spec 02b).
- The private `pollStatus()` helper is **not** added — its content lives
  directly in `onBeforeExtract()`.

All other content in OL §3.6 (new member `m_cachedRunState`,
`runState()`/`listenerState()` overrides, removal of `runStatus()`,
behavioural notes about `dimDirty` / `hmhost` / `oldStatus`) applies
unchanged.

## Scope

| File                                             | Change                                                                                                                                                                                                                                                                                                                      |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/SINQ/inc/MantidSINQ/SINQHMListener.h` | Declare `runState()`, `listenerState()` overrides. Add `m_cachedRunState` member. Declare `protected: void onBeforeExtract() override;`. Drop the `runStatus()` declaration.                                                                                                                                                |
| `Framework/SINQ/src/SINQHMListener.cpp`          | Implement `onBeforeExtract()` with the body OL §3.6 assigns to `pollStatus()` (HTTP request, parse, write `m_cachedRunState`/`hmhost`/`dimDirty`/`oldStatus`). `doExtractData()` (renamed in 02b) loses its explicit `runStatus()` call at line 104 — that work is now done by the hook. Remove the `runStatus()` override. |

`oldStatus` and `dimDirty` continue to be written by `onBeforeExtract()`
with the same triggering logic.

## Behavioural note

The only in-tree caller of `runStatus()` is `MonitorLiveData`, and it
always calls `extractData()` first (`MonitorLiveData.cpp:177`) then
`runStatus()` (line 191). After this refactor:

- `extractData()` calls `onBeforeExtract()` which refreshes
  `m_cachedRunState`, `hmhost`, and `dimDirty`; then `doExtractData()`
  loads dimensions and builds the workspace.
- `MonitorLiveData`'s subsequent `runStatus()` call goes through the base
  default and returns `m_cachedRunState` (i.e. exactly what the legacy
  `runStatus()` would have returned after its second HTTP fetch).

The previously-performed second HTTP fetch (one per `MonitorLiveData`
iteration) is eliminated — a beneficial side effect of removing the
anti-pattern.

## Tests

| Test                                                 | Location                                   |
| ---------------------------------------------------- | ------------------------------------------ |
| `test_runState_is_pure_getter`                       | `Framework/SINQ/test/SINQHMListenerTest.h` |
| `test_onBeforeExtract_refreshes_hmhost_and_dimDirty` | same                                       |
| `test_onBeforeExtract_throws_on_invalid_DAQ_code`    | same — preserves the existing throw        |

If `SINQHMListenerTest.h` does not currently exercise the HTTP path
(historically it required a live HM endpoint), add a fixture that stubs
`httpRequest()` to return canned response strings. The stub may be a
protected virtual override.

## Verification

- `ninja SINQHMListenerTest` builds.
- `./bin/SINQTest SINQHMListenerTest` (or the equivalent SINQ test
  executable) passes.
- `pre-commit run --files <changed>`.

## Done when

- `SINQHMListener::runStatus()` no longer exists.
- The HTTP poll body lives in `onBeforeExtract()` and is invoked
  exclusively via the `LiveListener::extractData()` template method
  introduced in sub-spec 02b. No private `pollStatus()` helper remains.
- SINQ system tests (if any depend on this listener) pass unchanged.
