# Sub-spec 02 — Side-effect-free listener overrides

## Goal

Add the trivial `runState()` and `listenerState()` overrides to every
concrete `ILiveListener` implementation whose existing `runStatus()` is
already side-effect-free. Existing `runStatus()` overrides are removed and
fall through to the base default introduced in sub-spec 01.

## Reference

- OL §3.2 — `FileEventDataListener`
- OL §3.3 — `ISISLiveEventDataListener`
- OL §3.4 — `ISISHistoDataListener`
- OL §3.5 — `KafkaEventListener` / `KafkaHistoListener`
- OL §3.7 — `TestGroupDataListener` / `TestDataListener`
- OL §3.8 — `MockLiveListener` and algorithm-test mocks

## Scope

For each listener in the above sections:

- Add `RunStatus runState() const override` returning what the listener's
  current `runStatus()` returns.
- Add `ListenerState listenerState() const override` mapping the listener's
  existing connection-state member to the enum.
- Remove the listener's `runStatus()` override (let the base default apply).

Listeners affected (full list — see OL §2 matrix):

- `Framework/LiveData/src/FileEventDataListener.{h,cpp}`
- `Framework/LiveData/src/ISIS/ISISLiveEventDataListener.{h,cpp}`
- `Framework/LiveData/src/ISIS/ISISHistoDataListener.{h,cpp}`
- `Framework/LiveData/src/Kafka/KafkaEventListener.{h,cpp}`
- `Framework/LiveData/src/Kafka/KafkaHistoListener.{h,cpp}`
- `Framework/LiveData/test/TestGroupDataListener.{h,cpp}`
- `Framework/LiveData/test/TestDataListener.{h,cpp}`
- `Framework/API/test/LiveListenerTest.h` (`MockLiveListener`)
- Any algorithm-test mocks that derive from `ILiveListener` directly:
  `LoadLiveDataTest`, `MonitorLiveDataTest`, `StartLiveDataTest`,
  `LiveDataAlgorithmTest` (audit during implementation).

## Tests

For each modified production listener, the existing tests must pass
unchanged. Additionally add one new compile-and-call test per listener:

- `test_runState_matches_legacy_runStatus` — call `runState()` and the
  base-class `runStatus()` (via legacy shim); assert equal.
- `test_listenerState_reflects_connected_state` — drive
  `connect()`/`disconnect()` (where applicable) and assert the enum value.

For the `MockLiveListener` and algorithm mocks, no new tests — the
existing algorithm test suites prove the wiring.

## Verification

- `ninja Framework LiveData SINQ` — succeeds.
- `ctest -R "Listener|LiveData"` — all pass.
- `pre-commit run --files <changed>`.

## Done when

- Every side-effect-free concrete listener listed above provides both
  overrides.
- No `runStatus()` override remains on any of these listeners.
- All affected unit tests pass.
- No behavioural change at any algorithm level — `MonitorLiveData` workspace
  renaming, `LoadLiveData` workspace publication, and `StartLiveData` all
  behave identically.
