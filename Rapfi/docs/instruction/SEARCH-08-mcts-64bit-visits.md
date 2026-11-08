# SEARCH-08 — execution guidance

Task: [`../todo/SEARCH-08-mcts-64bit-visits.md`](../todo/SEARCH-08-mcts-64bit-visits.md).

## Approach

1. **Baselines first, from HEAD, before touching code** (keep the outputs under
   `build/search08/`, not in the repo):
   - `echo BENCH | ./pbrain-rapfi` must be Nodes `17621937`, Hash `8c1f72e5`.
   - Peak RSS of one fixed 2M-node MCTS search (Standard, default config), as in SEARCH-05.
   - Golden output: single thread, fixed node count, fixed position; save the root move list
     (move, visits, Q). This is the phase-1 bit-identity guard.
   - NPS for MCTS, 1 and 8 threads, fixed time, median of several runs, on a PGO build.
   - `sizeof(Edge)`, `sizeof(Node)` (a `static_assert` or a one-off print).
2. **Write the test list as tests that fail first** (`tests/mcts_visits_tests.cpp`, add to
   `TEST_DRIVER_SOURCES`). Several need new accessors; add them as thin wrappers over today's behaviour.
3. **Do the derived-state audit in the todo file** before editing `Edge`.
4. **Step A: widen locals and `Node::n` only.** `Edge` untouched (`edgeVisits` still `uint32_t`).
   Build, tests, bench, golden diff.
5. **Step B: repack `Edge`** behind the existing accessors (`getMove`, `getP`, `getVisits`, `addVisits`,
   `getChild`, `setChild`) so callers do not change. `static_assert(sizeof(Edge) == 16)`. Then widen the
   `getVisits`/`addVisits` signatures to `uint64_t` and fix the callers the compiler flags.
6. **Step C: drop the cap.** Remove `MaxRootVisits` (`3u << 30`, commit 629b5d0), add the far-away guard.
7. Phase 2 (`double` accumulators) only after steps A-C are clean, as a separate commit with its own
   measurement.
8. Verify with the todo's Measure list, then the 20-minute run across 2^32 root visits. Rebuild the
   release with `scripts/pgo_build.sh x64-clang-AVX512VNNI` after the last source change.

## Pitfalls

- **Do not clip or freeze an edge.** `searchNode` only descends through a transposed child when
  `childEdgeVisits >= childNodeVisits` (~line 620). A frozen edge stops the tree from deepening.
- **Policy bits share the visit word.** A carry out of bit 47 corrupts `getP()`. Keep the guard far
  below 2^48 and assert it in tests, not only in comments.
- **`setP` after publication is a data race** once the policy lives in an atomic word that other threads
  `fetch_add`. Edges are built before the release CAS in `Node::createEdgesFrom`; keep `setP` constructor-only.
- **`setChild` must not drop the move bits.** `getMove` is read by `selectChild` and root-move code with
  no lock.
- **`nVirtual` is not widened.** `finishVisit` does `fetch_add(-newVisits)` and relies on unsigned wrap.
- **Memory-budget math:** `edgeArrayFootprint`, `EdgeArray` alignment and the `static_assert` at
  `node.h:121-122` assume 16 bytes. SEARCH-03 and `tests/mcts_memory_tests.cpp` depend on it.
- **MCTS is not covered by BENCH.** The built-in config is alpha-beta, so a green bench says nothing
  about this change. The golden-output diff and the self-play/long-run checks are the real guards.
- **Float accumulation:** with weights near 4e9, `qSum` loses small edges (ulp 256). Do not hide this by
  changing to `double` in phase 1; it would break the bit-identity guard.
- **Tests with asserts live:** `rapfi_tests` forces `-UNDEBUG`; never do that to the `rapfi` target.
- Plain `ninja` builds are only compile checks (gcc here). Strength and NPS numbers come from the PGO build.

## Don't touch

- `game/pattern.*`, bitkeys, zobrist, `movegen.cpp`, the alpha-beta search.
- `mcts_edge_policy_mass`, `mcts_expand_min_visits` and graph-budget defaults (SEARCH-02/03/05).
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`); read them, do not edit or commit them.

## Definition of done

- Tests from the todo pass with asserts live, including the concurrent and the above-2^32 cases.
- Bench `17621937 / 8c1f72e5`; phase-1 golden output identical; RSS unchanged; NPS within noise.
- The 20-minute run shows continuous `EV`/`W` and monotonic `V` across 2^32 root visits.
- Todo and `TODO.md`/`instruction.md` lines moved to `## Done` with the date and these numbers.
- Commits in conventional style (`search:`, `test:`, `docs:`), one per step A, B, C.
