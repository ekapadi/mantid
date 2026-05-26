# Defect: `SNSLiveEventDataListener::m_ignorePackets` is never set to `true`

**Status:** Reportable defect. Not introduced by current refactor.
**Discovered during:** Implementation review of `plans/v3_integration_test/SNSListener-integration-test.md`
(branch `EWM15431_live-listener-interface`), while validating the proposed
integration-test cases in §6.8 ("Historical replay & variable cache").
**Component:** `Framework/LiveData` — `SNSLiveEventDataListener`.
**Severity:** Functional — a documented runtime feature (historical replay
filtering + deferred variable-value replay) is silently inert. No crash,
no error, no log message.

---

## 1. Summary

`SNSLiveEventDataListener::m_ignorePackets` is declared with an in-class
initializer of `false` and is **never assigned `true` anywhere in the
codebase**. As a consequence:

- The "filter packets until run start" path (`m_filterUntilRunStart`) is
  unreachable.
- The "filter packets until absolute start time" path is unreachable.
- The variable-value packet cache (`m_variableMap`) is populated by the
  `rxPacket(VariableU32Pkt&)` / `VariableDoublePkt` / `VariableStringPkt`
  overloads but is **never replayed**, because `replayVariableCache()` is
  only called from inside the `if (!m_ignorePackets) { ... }` block of
  `ignorePacket()`, which is itself reached only when `m_ignorePackets`
  is `true`.
- The `extractData()` guard

  ```cpp
  if (m_ignorePackets) // This variable is (un)set in ignorePacket()
      throw Exception::NotYet("Waiting for a run to start.");
  ```

  can never throw.

In short: a chunk of `SNSLiveEventDataListener` that exists specifically
to support `StartLiveData`'s "from start of run" and "from absolute
time" modes is dead code as currently written.

---

## 2. Evidence

### 2.1 Declaration

`Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`:

```cpp
bool m_ignorePackets{false}; // used by filterPacket() below...
bool m_filterUntilRunStart{false};
```

### 2.2 The only writes are clears

`Framework/LiveData/src/SNSLiveEventDataListener.cpp` (`ignorePacket()`):

```cpp
bool SNSLiveEventDataListener::ignorePacket(
    const ADARA::PacketHeader &hdr, const ADARA::RunStatus::Enum status) {
  // ...
  if (!m_ignorePackets)
      return false;                                    // <-- early exit

  if (m_filterUntilRunStart) {
    if (hdr.base_type() == ADARA::PacketType::Type::RUN_STATUS_TYPE &&
        status == ADARA::RunStatus::NEW_RUN) {
      m_ignorePackets = false;                         // clear #1
    }
  } else {
    if (timeFromPacket(hdr) >= m_startTime) {
      m_ignorePackets = false;                         // clear #2
    }
  }

  if (!m_ignorePackets) {
    replayVariableCache();
  }
  return m_ignorePackets;
}
```

A lexical search across the entire repository for `m_ignorePackets`
yields exactly:

1. the declaration (with `{false}` initializer);
2. the read at the top of `ignorePacket()`;
3. the two clears above;
4. the read in `extractData()`;
5. the final return statement.

**There is no `m_ignorePackets = true;` anywhere.**

### 2.3 The likely intended write site

`start()` parses the requested `startTime` to decide which filter mode
to use, but only sets `m_filterUntilRunStart`, never `m_ignorePackets`:

```cpp
void SNSLiveEventDataListener::start(const Types::Core::DateAndTime startTime) {
  m_startTime = startTime;
  if (m_startTime.totalNanoseconds() == 1000000000) {
    // "from start of previous run" sentinel
    m_filterUntilRunStart = true;
    // m_ignorePackets = true;   // <-- MISSING
  }
  // else if (m_startTime != DateAndTime()) {
  //     m_ignorePackets = true; // <-- MISSING (time-based filter case)
  // }
  m_thread.start(*this);
}
```

The `else` branch in `ignorePacket()` whose comment reads *"Filter based
solely on time"* is, today, unreachable without the missing assignment
in `start()`.

---

## 3. Provenance — not introduced by current refactor

This was checked against multiple points in the upstream history before
filing:

| Tree | Initializer | Any `= true` assignment? |
|---|---|---|
| `ekapadi/mantid` @ `EWM15431_live-listener-interface` (this branch) | `{false}` | None |
| `ekapadi/mantid` @ `ornl-next` | `{false}` | None |
| `mantidproject/mantid` @ current `main` | `{false}` | None |
| `mantidproject/mantid` @ `a86c1e02` (~2018, pre-refactor era) | `{false}` | None |

The defect is therefore **pre-existing in upstream** and predates any of
the work on this branch. It is *not* a regression introduced by
`EWM15431_live-listener-interface`, sub-specs 05/06/07, or any prior
listener refactor that we have made.

---

## 4. Impact

### 4.1 Functional

- `StartLiveData` "Now" mode (no `StartTime`, no historical replay) is
  unaffected — that path was never supposed to set `m_ignorePackets`
  in the first place.
- `StartLiveData` "from start of run" mode: historical packets sent by
  SMS that precede the most recent `NEW_RUN` are **not** filtered out.
  Whatever the user actually sees depends on what SMS happens to send.
- `StartLiveData` with a non-default `StartTime`: packets older than
  `m_startTime` are **not** filtered out.
- Variable-value packets that arrive during what *should* be the
  filtered prefix are not deferred-and-replayed; they are processed
  immediately, in arrival order, with no end-of-filter coalescing.

### 4.2 Why it has gone unnoticed

- The only existing unit suite (`SNSLiveEventDataListenerNoNetworkTest.h`)
  does not exercise the filter paths.
- The disabled legacy integration suite (`SNSLiveEventDataListenerTest.h`,
  to be renamed to `…LegacyTest.h` per the current spec) was network-
  dependent and unregistered, so it caught nothing.
- The behavioural difference is subtle: extra historical events at the
  front of the stream rather than a hard failure.

---

## 5. Interaction with the current refactor work

This was found while writing the implementation spec for the new UDS
integration-test suite (`plans/v3_integration_test/SNSListener-integration-test.md`).
The relevant section is **§6.8 "Historical replay & variable cache"**,
which proposes two tests:

- `test_filterUntilRunStart_dropsPreRunPackets`
- `test_variableCache_replayedAfterStartCondition`

**Both of these tests, as specified, will fail against the current
production code** — not because the integration harness is wrong, but
because the production behaviour they assert was never actually
implemented (or was lost long ago).

Reconciling this with the spec's §0 rule "No production code change":

1. The integration-test PR must **not** "fix" `start()` to set
   `m_ignorePackets`. That is a separate change to production behaviour
   and belongs in its own PR with its own review (and ideally, a
   conversation with the SNS team about whether the documented behaviour
   is in fact the desired behaviour).
2. The two §6.8 tests should be **written and committed in the
   integration-test PR**, but marked with `TS_SKIP("…")` that references
   this defect. The `TS_SKIP` body should contain the test logic
   verbatim, so that re-enabling the tests after the fix is a
   one-line change (delete the `TS_SKIP`).
3. This defect should be filed as a standalone tracking-system entry,
   referenced from both the `TS_SKIP` message and from the spec.

The recommended `TS_SKIP` text:

```cpp
TS_SKIP("Pre-existing defect: SNSLiveEventDataListener::m_ignorePackets "
        "is never set to true in start(), so the m_filterUntilRunStart / "
        "variable-cache replay path in ignorePacket() is unreachable. "
        "See plans/ignore-packets-defect.md and tracking-system entry "
        "<ID-TBD>. Test body retained so re-enable is a one-line change.");
```

A new item should be added to spec §0 ("Agent execution instructions"):

> **§0.8.** Two tests in §6.8 must be guarded with `TS_SKIP` referencing
> `plans/ignore-packets-defect.md`. Do **not** attempt to make them
> pass by modifying production code.

---

## 6. Proposed fix (for the separate defect PR — not this branch)

In `SNSLiveEventDataListener::start()`:

```cpp
void SNSLiveEventDataListener::start(const Types::Core::DateAndTime startTime) {
  m_startTime = startTime;

  if (m_startTime.totalNanoseconds() == 1000000000) {
    // "From start of previous run" sentinel: replay everything, then
    // filter out all packets older than the next NEW_RUN.
    m_filterUntilRunStart = true;
    m_ignorePackets = true;
  } else if (m_startTime != Types::Core::DateAndTime()) {
    // Absolute-time filter: drop everything older than m_startTime.
    m_ignorePackets = true;
  }
  // else: "Now" mode -- no historical filtering, m_ignorePackets stays
  // false (the default).

  m_thread.start(*this);
}
```

The fix must be accompanied by:

- Re-enabling (removing `TS_SKIP` from) the two §6.8 tests.
- A targeted no-network unit test for `start()` itself, asserting that
  the correct combination of `m_ignorePackets` / `m_filterUntilRunStart`
  is produced for each of the three `startTime` inputs (sentinel,
  absolute past time, default-constructed "now").
- A conversation with the SNS team confirming the intended semantics.
  In particular: when the sentinel `1e9 ns` value is used, is the
  intent really to ignore *all* packets until a `NEW_RUN`, or is the
  intent to ignore everything older than the *previous* run's NEW_RUN?
  The current dead code implies the former; the comment in `start()`
  implies the latter.

---

## 7. References

- This branch: `EWM15431_live-listener-interface`.
- Production file (declaration):
  `Framework/LiveData/inc/MantidLiveData/SNSLiveEventDataListener.h`
  — search for `m_ignorePackets`.
- Production file (uses):
  `Framework/LiveData/src/SNSLiveEventDataListener.cpp` — `start()`,
  `ignorePacket()`, `replayVariableCache()`, `extractData()`.
- Companion spec impacted by this defect:
  `plans/v3_integration_test/SNSListener-integration-test.md` §6.8.
