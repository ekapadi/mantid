# Sub-spec 01 — Base interface additions

> **Cross-reference key**
> "v3 §X.Y" refers to a section of `plans/listener_refactoring_v3.md`.
> "OL §X.Y" refers to a section of `plans/listener_refactoring_other_listeners.md`.

## Goal

Add the new pure-getter API to `ILiveListener` without changing the
behaviour of any concrete listener. After this commit, the tree compiles
unchanged, every existing test passes, and no caller yet uses the new
methods.

## Reference

- v3 §4.1 — base class `ILiveListener.h`
- v3 §6 — backward-compatibility `runStatus()` default
- OL §0 — base-class additions summary

## Scope

| File                                          | Change                                                                                                                                                                                                             |
| --------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Framework/API/inc/MantidAPI/ILiveListener.h` | Add `ListenerState` enum; add `runState()`, `isPaused()`, `listenerState()`, `lastTransition()` as **non-pure** virtuals with the safe defaults shown in v3 §4.1; add `[[deprecated]]` attribute to `runStatus()`. |
| `Framework/API/src/ILiveListener.cpp` (new)   | Out-of-line default `runStatus()` per v3 §6.                                                                                                                                                                       |
| `Framework/API/CMakeLists.txt`                | Add the new `.cpp` to `SRC_FILES`.                                                                                                                                                                                 |

### Deviation from v3 §4.1

v3 §4.1 declares `listenerState()` as `= 0` (pure virtual). To keep this
commit independent of all subclass changes, **this commit makes it
non-pure** with a default of `ListenerState::Disconnected`. Sub-spec 09
promotes it to `= 0` once every concrete listener has overridden it.

## Tests

| Test                                                              | Location                                      |
| ----------------------------------------------------------------- | --------------------------------------------- |
| `test_base_runStatus_default_returns_runState_when_no_edge`       | `Framework/API/test/LiveListenerTest.h`       |
| `test_base_runStatus_default_returns_lastTransition_when_present` | same                                          |
| `test_base_defaults_are_const_correct`                            | same — compile-time check via `const` fixture |

Use the existing `MockLiveListener` (or a minimal local subclass) that
overrides `runState()` and `lastTransition()` for the test only.

## Verification

- `ninja Framework` — succeeds (builds `Framework/API` and all downstream
  modules; confirms every existing listener still compiles against the new
  header).
- `ctest -R LiveListenerTest` — passes, including the three new test cases:
  - `test_base_runStatus_default_returns_runState_when_no_edge`
  - `test_base_runStatus_default_returns_lastTransition_when_present`
  - `test_base_defaults_are_const_correct`
- `pre-commit run --files Framework/API/inc/MantidAPI/ILiveListener.h Framework/API/src/ILiveListener.cpp Framework/API/test/LiveListenerTest.h`
- `clang-tidy` on `Framework/API/inc/MantidAPI/ILiveListener.h` and
  `Framework/API/src/ILiveListener.cpp`.

## Done when

- Tree builds.
- All existing listener tests pass unchanged (no override required because
  every new method has a default).
- New `LiveListenerTest` cases pass.
- No behavioural change at any algorithm or listener.
