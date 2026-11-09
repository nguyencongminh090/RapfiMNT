# SEARCH-10 — MCTS: add the cut moves back when every kept reply is lost (SEARCH-09 option (b))

**Status:** done 2026-11-09. Trigger-based widening with CAS swap and a retire list freed at the
search boundary. Bench unchanged, 161/161 tests (12 new), cut-off golden identical, all 8 SEARCH-09
proofs recovered with no false mate, stress: assert and ASan 0 reports, TSan only the pre-existing
VCF-TT races (same as a), self-play 32-36-52
(-12 ± 24 Elo). Not committed (SEARCH-09 is uncommitted too).

**Class:** B (MCTS search behaviour). A published `EdgeArray` becomes replaceable, so the decision note
below also covers lifetime, concurrency and memory accounting.

**Unblocks:** VCN-03 option B. A VCN proof needs "every defender reply loses", and after SEARCH-09 (a)
a node whose edges were cut can never prove that.

## Problem

SEARCH-09 (a) folds an unknown bound into every node whose edge list was cut, so such a node is
never proven lost. That is sound, but it costs proofs. In the SEARCH-09 scan (0.99, 100K nodes,
6084 positions) 8 true root mates out of 1464 were lost: 411, 1233, 1242, 1259, 1662, 4293, 4693,
5273 (`build/search09/cmp_on_base_vs_fix.txt`). The node stays unprovable at any node count, because
its edge array is immutable.

## Decision note

- **What:**
  - **Trigger.** In `Node::updateStats`, when the array is cut (`edgesCut()`) and the bound folded
    from the kept children alone has `upper <= VALUE_MATED_IN_MAX_PLY` (every kept reply is a proven
    win for the opponent), report "widen needed". `updateStats` already computes that bound before
    the SEARCH-09 fold, so the check is free. It is reported through a return value; `Node` gets no
    new field.
  - **Widen.** `searchNode` (non-root, graph not closed) widens the node once its board is back at
    that node's position. It regenerates the moves with the same `MovePicker` setup as `expandNode`
    but with no cut, builds a full `EdgeArray` and swaps it in with a CAS against the old pointer.
    - **Layout of the new array:** the kept edges come first, in their old order, with their visits
      and child pointers copied, so the visited edges still form a prefix. The cut moves follow,
      unvisited, in descending policy order.
    - **Policies:** every edge in the new array takes its policy from the full regenerated
      distribution. That includes the kept edges, so the policies sum to about 1.
    - **Result:** `numMoves == numEdges`, so the node is provable again.
  - **Catch-up after the swap.** The thread that wins the CAS re-reads the old kept edges and adds
    any visit delta to the new array. This narrows the window for `addVisits` calls lost on the old
    array.
  - **Reclamation (quiescent state).** The old array is not freed at once. It goes on a retire list
    in `MCTSSearcher`, guarded by a mutex, and stays counted in `graphMemoryUsed`. The list is freed
    only when no search thread can hold a pointer into it: right after the worker
    `runOnAllThreads(search, true)` returns in `searchMain`, in `clear(true)`, and in the
    destructor.
  - **Rejected:** epoch-based reclamation and hazard pointers (SEARCH-04's general plan). They are not
    needed, because a replacement is rare (only nodes whose every kept reply is lost) and the search
    boundary is a free grace period. Also rejected: a chained tail array, which breaks the
    visited-prefix assumption in `selectChild` and `updateStats`.
  - **Graph closed (SEARCH-03):** no widening, so the memory guarantee holds. The node stays
    unprovable, which is the sound SEARCH-09 (a) result.
- **Expected:**
  - **Cut off:** with `mcts_edge_policy_mass >= 1` the trigger never fires, so the output is
    bit-identical (golden). Bench is unchanged (the built-in config is alpha-beta).
  - **Cut on:** with `0.99` the 8 lost proofs come back, or the root claims get at least as close
    to the `1.0` reference as base HEAD. There are no false mates, because each widened node proves
    from all of its replies.
  - **Strength and memory:** strength equal to SEARCH-09 (a) within noise. Retire-list memory is
    small (report the peak).
- **How measured:**
  - Unit tests (list below).
  - Cut-off golden output.
  - A re-run of the SEARCH-09 mate-claim scan, comparing base HEAD, (a), (a)+(b) and `1.0`.
  - A widen-event count and the peak retired bytes, from a temporary counter that is not committed,
    or from a `MESSAGEL` in a debug build.
  - A multi-thread stress run with asserts and sanitizers.
  - Self-play of (a)+(b) vs (a), using the SEARCH-05 stage-1 settings.
- `Edge` stays 16 bytes, `Node` 56 bytes and `EdgeArray` 8 bytes.

## Function → test-case list (write before code)

- `updateStats_edgesCutAllKeptChildrenProvenWinForOpponent_reportsWidenNeeded`
- `updateStats_edgesCutOneKeptChildUnknown_noWidenNeeded`
- `updateStats_noCutAllChildrenProvenWinForOpponent_noWidenNeededProvenLoss`
- `replaceEdgesWithFull_keptEdgesFirst_visitsAndChildrenCopied`
- `replaceEdgesWithFull_cutMovesAppendedUnvisitedInPolicyOrder`
- `replaceEdgesWithFull_success_edgesCutFalseAndPoliciesFromFullList`
- `replaceEdgesWithFull_oldPointerAlreadyReplaced_returnsFalseNewArrayFreed`
- `replaceEdgesWithFull_success_oldArrayRetiredMemoryAccounted`
- `updateStats_afterReplaceAllRepliesProvenWinForOpponent_provenLoss` (the proof the fix enables)
- `freeRetiredEdgeArrays_releasesAllAndSubtractsFootprint`
- Position/scan test: SEARCH-09 positions 411, 1233, 1242, 1259, 1662, 4293, 4693, 5273 at `0.99`,
  100K nodes, 1 thread. Each root mate claim matches the `1.0` reference: `411` at 2M nodes, the
  others at 100K. No position claims a mate that `1.0` at 2M refutes.

## Verification

- Bench bit-identical (`17621937` / `8c1f72e5`).
- Unit tests with asserts live.
- Golden identical with the cut off.
- Stress: 8 threads, cut on, with `FORCE_ASSERTION=ON`, and under ASan, plus TSan if the toolchain
  allows. Run on positions that trigger widening; the widen count must be > 0. No crash, no
  sanitizer report.
- Mate-claim scan and self-play as in the decision note.

## Results — 2026-11-09

Measurement files: `build/search10/` (not in the repo). gcc Release (`build/`) for unit tests,
bench, golden and scan; PGO (`scripts/pgo_build.sh x64-clang-AVX512VNNI`) for self-play and a second
bench/golden check. "a" = SEARCH-09 (a) working tree (byte-identical to `build/search09/pbrain-fix-gcc`),
"base" = HEAD `42f029b`, "a+b" = this change. The base / a / `1.0` scan rows reuse the SEARCH-09
outputs (same binaries, same harness).

Implementation (as in the decision note):

- `Node::updateStats()` returns `widenNeeded` (cut array and the kept-only bound has
  `upper <= VALUE_MATED_IN_MAX_PLY`, node not terminal). Callers that ignore it still compile.
- `Node::replaceEdgesWithFull(expectedOld, moves, policy, numMoves, allocCounter, retired)`: kept
  edges matched by move (abort before allocating if one is missing), copied first in the old order
  with visits and child (visits read first, abort if a kept edge is unvisited), cut moves appended
  unvisited in the regenerated order, every policy from the full list; `numEdges == numMoves`.
  Release CAS against `expectedOld` (loser frees its array), then `edgeArrayFootprint(new)` is added
  and the visit deltas of the old edges are carried over, then the old array is retired.
- `RetiredEdgeArrays` (`node.h`): mutex + vector + byte count; `MCTSSearcher::retiredEdgeArrays`,
  `MCTSSearcher::freeRetiredEdgeArrays()` subtracts the freed footprint. Freed after the worker
  `runOnAllThreads(search, true)` in `searchMain` (checked: it runs the task on the main thread and
  then `waitForIdle()` on every worker, whose `running` flag is set under the mutex by `runTask`),
  in `clear(true)` after `waitForIdle`, and by the member destructor.
- `widenNode()` in `search.cpp`: the same `MovePicker<MAIN>` setup as `expandNode`, no cut; called in
  `searchNode` after `board.undo` when an `updateStats` of this iteration returned true, only for
  `!Root`, and only if the array is still cut and `graphClosed()` is false (re-sampled). In debug
  builds it asserts every kept move is regenerated.
- `Edge` / `Node` / `EdgeArray` stay 16 / 56 / 8 bytes (static_asserts unchanged).

| Check | Result |
|---|---|
| Tests (`rapfi_tests`, asserts live) | 161/161 (12 new in `tests/mcts_widen_tests.cpp`); also 161/161 under ASan and TSan builds, no sanitizer report |
| Bench gcc Release (a+b) | 17621937 / 8c1f72e5 (a: same) |
| Bench PGO (a+b) | 17621937 / 8c1f72e5 |
| Golden, cut off (`1.0`), 4 positions, 300K nodes, final root list | a+b gcc = a gcc = base = a+b PGO = SEARCH-09 fix PGO (identical) |
| Determinism, 1 thread, 100K, 10 trigger positions, two runs | identical rows |
| Stress 8 threads, cut on, `FORCE_ASSERTION=ON`, 8 positions x 2 consecutive searches, 300K | clean exit, no assert; widen count 28 over 16 searches, peak retired 16224 bytes in one search (4293, 8 widens) |
| ASan (+LeakSanitizer, clean END/exit) same setup, 200K | 0 reports; widen count 27, peak retired 13608 bytes |
| TSan same setup, 50K | 370 warnings, all in the lockless VCF TT (`hashtable.cpp` probe/store/key); none touches SEARCH-10 code. Baseline a under the same TSan setup: same classes (381 warnings). Pre-existing, by design |

Fail first (`tests_failing_first_stub.txt`): against HEAD-plus-SEARCH-09 the new tests do not
compile (`updateStats` returns `void`, no `replaceEdgesWithFull` / `RetiredEdgeArrays` /
`freeRetiredEdgeArrays`). With the signatures landed as stubs (`updateStats` returns false, replace
returns false and touches nothing, free does nothing), `RAPFI_TEST_ISOLATE=1`: 8 fail at runtime
(`updateStats_edgesCutAllKeptChildrenProvenWinForOpponent_reportsWidenNeeded`, all
`replaceEdgesWithFull_*` success-path tests, `updateStats_afterReplaceAllRepliesProvenWinForOpponent_provenLoss`,
`freeRetiredEdgeArrays_releasesAllAndSubtractsFootprint`); 4 guards pass and must keep passing
(`updateStats_edgesCutOneKeptChildUnknown_noWidenNeeded`,
`updateStats_noCutAllChildrenProvenWinForOpponent_noWidenNeededProvenLoss`,
`updateStats_edgesCutUnvisitedKeptEdge_noWidenNeeded`,
`replaceEdgesWithFull_keptMoveMissingFromFullList_returnsFalseNothingChanged`).
Tests added beyond the list: `updateStats_edgesCutUnvisitedKeptEdge_noWidenNeeded`,
`replaceEdgesWithFull_keptMoveMissingFromFullList_returnsFalseNothingChanged`; the replace tests use
a regenerated list in a different order to check matching by move.

Mate-claim scan (`scan.py`, 6084 positions, `0.99`, 1 thread, 100K nodes):

| Binary | root +M | root -M | any mate in multipv |
|---|---|---|---|
| base `0.99` | 736 | 728 | 1934 |
| a `0.99` | 731 | 725 | 1930 |
| a+b `0.99` | 735 | 728 | 1933 |
| `1.0` | 735 | 730 | 1934 |

- a vs a+b: 17 positions changed. 7 regain a mate claim, same move as `1.0`: `1233 -M16`,
  `1242 +M19`, `1259 -M12`, `1662 +M21`, `4293 +M19`, `4693 -M18`, `5273 +M23`. `1106` and `5905`
  return to the base / `1.0` move and mate (`4,9 +M11`, `11,5 +M17`). 8 have an ev change of 1–2
  (331, 411, 3059, 4291, 4420, 4682, 4971, 5109), same move. No position loses a claim.
- base vs a+b: 8 changed: `411` (base `+M27`, a+b 357 at 100K) and the 7 ev wiggles above. No mate
  claim of a+b is absent from base.
- The 8 SEARCH-09 positions: 7 recovered at 100K (above). `411` is recovered at 2M nodes: a+b
  `+M27` with `6,2`, the `1.0` reference (a still reports 502 at 2M).
- No false mate: the only a+b claims that `1.0` at 100K lacks are `1276` and `1698`, both claimed by
  base too and confirmed true in SEARCH-09 (`verify_candidates.txt`). Claims seen only in the
  8-thread stress runs were checked with `1.0` at 2M after the claimed move: `4293 10,2 +M17`
  (opponent `-M10`), `1259 3,7 -M8` (opponent `+M7`; a faster loss in a lost position, still a true
  claim), `4693 2,9 -M18` (opponent `+M17`), `411 9,2 +M27` (forced `10,1`, then `+M25` with `6,2`).

Deviations from the instruction file:

- The "every kept move is regenerated" assert lives in `widenNode` (debug builds only), not in
  `replaceEdgesWithFull`. Inside the function it would abort the abort-path unit test under live
  asserts; the function itself just returns false and keeps the old array.
- `replaceEdgesWithFull` also aborts (frees its temp array, returns false) when a kept edge is
  unvisited or has no child, to protect the visited-prefix invariant. The trigger guarantees this
  never happens in the search.
- `retired` is a `RetiredEdgeArrays &` (new class in `node.h`: mutex, vector, byte count), not a
  callback or pointer. The destructor free is the `RetiredEdgeArrays` member destructor; there is no
  explicit `~MCTSSearcher`.
- Two tests beyond the list (see above). The position/scan test is the scan and the 2M check, run
  from `build/search10/`, not a frozen test in `rapfi_tests` (a full MCTS search is too slow there).

Self-play: `build/oppo4-ab/s10_fix` (a+b, PGO) vs `s9_fix` (a, PGO), same `s5_pm99` config at `0.99`
(config.toml identical), `OPFILE=op_std_main.txt run.sh s10_fix s9_fix 1 120 tc=0/2` (Standard, 1
thread, 256 MB, concurrency 6, SPRT [-10, 10]); log `build/oppo4-ab/logs/s10_fix_vs_s9_fix_r1_tc_0_2.txt`.
W-L-D 32-36-52, score 0.483, about -12 ± 24 Elo (1 SE), LLR -0.407 [-2.944, 2.944]. Inside 1 SE:
no regression shown, no gain either (widening fires only in nodes whose kept replies are all lost).

Outcome against "Expected": cut off bit-identical (golden, bench); with the cut all 8 lost proofs
come back (7 at 100K, `411` at 2M), every new claim is a true mate, retired memory stays in the
kilobytes per search, strength equal within noise. A cut node whose every kept reply loses is now
widened and can be proven lost; with a closed graph (SEARCH-03) it is not widened and stays
unprovable (the sound SEARCH-09 (a) result).

Harness notes: `stress.py` (consecutive searches in one engine process, clean END, stderr to a
file, 2 s pause before each new `BOARD`). One early 1-thread assert-build run (first driver version:
no pause, stderr on an unread pipe, machine loaded by the scan and three builds) went idle after
the first search; the engine sat waiting at 0 % CPU. It did not reproduce: the same 1-thread
assert-build run over all 8 positions completes with and without the pause (identical rows, which
also match the release scan), and so do a and a+b release runs without the pause. The TSan run did
hang on a full stderr pipe, hence stderr to a file. A dropped `BOARD` (protocol input during the end
of a search) is the likely cause; it is not attributed to SEARCH-10. The widen counter was a
temporary `#ifdef SEARCH10_STATS` `MESSAGEL` in `freeRetiredEdgeArrays`, used only in the stress
builds and removed afterwards; the gcc Release binary built from the final source is byte-identical
to the one measured.
