# Sub-spec 01 — Rename legacy `SNSLiveEventDataListenerTest.h`

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 1 of 6. Trivial rename + 1-line comment edit. Builds the
foundation for the new integration suite by clearing the historical
name out of the way without breaking the existing build.

---

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface__agents`; **do not** rebase
   onto `main` / `master`.
2. **Scope fence.** Touch only the two paths listed in §2 below. Do
   **not** modify any file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/`. Do not touch CMake files in this commit.
3. **Static verification only — DO NOT build, DO NOT run tests.** Confirm
   the rename is internally consistent by reading the resulting file and
   checking that no other file in the repo `#include`s the old name.
4. **No build artefacts in the PR.**
5. **The renamed file is retained**, not deleted, and is **not**
   registered in `TEST_FILES`. (It is already unregistered in the
   current `Framework/LiveData/CMakeLists.txt`, behind a `# Needs fixing
   …` comment placeholder. Leave that placeholder alone in this commit
   — it is rewritten in `subspec03`.)
6. **Ambiguity protocol.** If you find another file `#includes` the old
   name, stop and surface it in the PR description before resolving.
7. **No production code changes.**
8. **One commit, scoped to the rename only.** No other edits to this
   file or its companions in this commit.

---

## 1. Goal of this commit

Free the name `SNSLiveEventDataListenerTest.h` for re-use by the new
UDS-driven integration suite (created in `subspec03`), while preserving
the historical, network-dependent suite as in-tree reference material.

---

## 2. Files touched in this commit

| Action | Path |
|---|---|
| Rename | `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` → `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` |
| Edit | `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` (add 1-line header comment) |

**No other files are modified in this commit.** In particular,
`Framework/LiveData/CMakeLists.txt` is **not** edited here. The comment
placeholder there (`# Needs fixing to not rely on network.
SNSLiveEventDataListenerTest.h`) is rewritten in `subspec03` when the
*new* test header is added.

---

## 3. TODO

- [ ] `git mv Framework/LiveData/test/SNSLiveEventDataListenerTest.h
      Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h`
      (use `git mv`, not delete + create, so history follows the file).
- [ ] Open `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h`
      and add, immediately after the existing licence/copyright block
      and before any `#pragma once` / `#ifndef` guard, the following
      single-line comment **verbatim**:

      ```cpp
      // Legacy network-dependent test, retained for reference only; superseded by `SNSLiveEventDataListenerTest.h` (integration) and `SNSLiveEventDataListenerNoNetworkTest.h` (unit).
      ```

      If the existing file has no licence/copyright block, place the
      comment at the very top of the file (before `#pragma once`).
- [ ] Do **not** rename the class inside the file. The CxxTest fixture
      class can keep its current name — it is unregistered and never
      compiled into the test binary, so internal naming is irrelevant.
- [ ] Run `grep -R SNSLiveEventDataListenerTest Framework/ docs/ qt/
      scripts/ buildconfig/` and visually confirm that the only remaining
      mentions of the old name are:
      * the placeholder comment in `Framework/LiveData/CMakeLists.txt`
        (`# Needs fixing to not rely on network. SNSLiveEventDataListenerTest.h`) —
        **leave it alone**, it is rewritten in `subspec03`;
      * any references inside the renamed file itself (e.g. class names
        used by CxxTest), which are tolerated.

      If any *other* `#include` or build-system reference to the old
      header name exists, **stop** and surface it in the PR description
      — it would have been broken by the rename and indicates a hidden
      coupling not captured by this spec.

---

## 4. Definition of done for this commit

1. `Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` exists
   and contains the verbatim header comment above.
2. `Framework/LiveData/test/SNSLiveEventDataListenerTest.h` no longer
   exists in the working tree.
3. `git log --follow
   Framework/LiveData/test/SNSLiveEventDataListenerLegacyTest.h` shows
   the prior history of the file.
4. No other file in the repository is modified by this commit.
5. The diff for this commit consists of: (a) the rename, and (b) the
   single-line header-comment insertion.
