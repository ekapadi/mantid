# Sub-spec 04 — `SINQHMListener` anti-pattern fix

## Goal

Move the HTTP poll and `dimDirty` / `hmhost` mutations out of
`SINQHMListener::runStatus()` and into a dedicated `pollStatus()` helper
invoked from `extractData()`.

## Reference

- OL §1 — anti-pattern review
- OL §3.6 — full code template

## Scope

| File                                             | Change                                                                                                                                                                                                                                                                                        |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Framework/SINQ/inc/MantidSINQ/SINQHMListener.h` | Declare `runState()`, `listenerState()` overrides; declare private `pollStatus()` and `m_cachedRunState`.                                                                                                                                                                                     |
| `Framework/SINQ/src/SINQHMListener.cpp`          | Implement per OL §3.6. Move the HTTP poll body (current `runStatus()` lines 60–98) into `pollStatus()`, writing `m_cachedRunState` instead of returning. Call `pollStatus()` from `extractData()` (replacing the explicit `runStatus()` call at line 104). Remove the `runStatus()` override. |

`oldStatus` and `dimDirty` continue to be written by `pollStatus()` with
the same triggering logic.

## Behavioural note

The only in-tree caller of `runStatus()` is `MonitorLiveData`, and it
always calls `extractData()` first (`MonitorLiveData.cpp:177`) then
`runStatus()` (line 191). After this refactor:

- `extractData()` calls `pollStatus()` which refreshes `m_cachedRunState`,
  `hmhost`, and `dimDirty`.
- `MonitorLiveData`'s subsequent `runStatus()` call goes through the base
  default and returns `m_cachedRunState` (i.e. exactly what the legacy
  `runStatus()` would have returned after its second HTTP fetch).

The previously-performed second HTTP fetch (one per `MonitorLiveData`
iteration) is eliminated — a beneficial side effect of removing the
anti-pattern.

## Tests

| Test                                             | Location                                   |
| ------------------------------------------------ | ------------------------------------------ |
| `test_runState_is_pure_getter`                   | `Framework/SINQ/test/SINQHMListenerTest.h` |
| `test_extractData_refreshes_hmhost_and_dimDirty` | same                                       |
| `test_pollStatus_throws_on_invalid_DAQ_code`     | same — preserves the existing throw        |

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
- `pollStatus()` is invoked exclusively from `extractData()`.
- SINQ system tests (if any depend on this listener) pass unchanged.
