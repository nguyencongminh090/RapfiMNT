# SEARCH-09 — MCTS mate bound unsound under the policy-mass edge cut

**Status:** done 2026-11-09. Option (a): cut count in `EdgeArray` padding, unknown bound folded in
`Node::updateStats`. Bench unchanged, 149/149 tests, cut-off golden identical, self-play tie. No real
false-mate position found (see Results). Not committed.

**Class:** B (search behaviour). Decision note and numbers required before done (CLAUDE.md rule 9).

**Blocks:** VCN-03 option B (native MCTS-VCN): a VCN proof is exactly "every defender reply loses",
the claim this bug makes unsound.

## Problem

- `Node::createEdgesFrom` (`search/mcts/node.cpp:177`) keeps only the leading moves until their
  cumulative policy reaches `mcts_edge_policy_mass` (default `0.99` since SEARCH-05; never fewer
  than `mcts_min_edges = 8`, `search/searchconfig.h:57`; root is never cut).
- `Node::updateStats` (`node.cpp:236`) builds `ValueBound` only from the edges that exist. A cut
  move is invisible to it.
- So a node whose **kept** children are all proven wins for the opponent gets
  `bound.upper` = mated, even when a cut move saves it (false proven loss).
- One level up, that child's false proven loss becomes a false proven win of the parent
  (`maxBound.lower >= VALUE_MATE_IN_MAX_PLY`). `selectBestmoveOfChildNode` then calls
  `overrideBestByMateBounds` (`search/mcts/search.cpp:889`), which multiplies every non-proven
  child's selection value by `1e-9`. **The engine plays the falsely proven move** and prints `+M`.
- Only the proven-loss direction (`upper`) is affected. A single proven-winning child is a valid
  proof whether or not siblings were cut.
- Only nodes with more than 8 legal replies can be cut, so nodes facing a four (few replies) are
  safe. Candidates: a defence against an open three with scattered counter-fours, or a quiet node,
  where the only saving move has policy below the 1 % tail.

## Decision note

- **What (option (a), chosen 2026-11-09):** a node whose edge list was cut cannot become a proven
  loss (or a capped draw) from its kept children alone. The cut moves count as one unexplored child
  with bound `(-inf, +inf)`: `Node::updateStats` folds `ValueBound {}` after the edge loop when
  `EdgeArray::edgesCut()`. `lower` (proof of a win) is untouched; `upper` becomes `+inf`.
  Option (b) (add the cut moves back) is out of scope.
- **Where the cut is recorded (deviation from the draft above):** not a spare bit in `Node`, but a
  new `uint32_t numMoves` in the 4-byte padding of `EdgeArray` (before the 8-byte-aligned
  `edges[]`), with `bool edgesCut() const { return numEdges < numMoves; }`. Written in
  `createEdgesFrom` before the release CAS, so immutable once published (no atomics, no new race).
  `static_assert(sizeof(EdgeArray) == 8)` guards the allocation math. `Edge` 16 and `Node` 56 bytes
  unchanged. The count (not a bool) also serves VCN-03 and a later option (b).
- **Expected:** default-off path (`mcts_edge_policy_mass >= 1`) bit-identical; with the cut, no
  false `+M` / `-M`; strength equal (false mate overrides are rare). Known cost: a cut defender node
  can no longer be proven lost even when every cut reply also loses, so some true proofs are found
  later or not at all.
- **How measured:** synthetic failing unit tests; MCTS fixed-node golden output with the cut off;
  mate-claim scan over 6084 positions before/after with `0.99` (and `1.0` as reference); self-play
  fix vs base with `0.99`, SEARCH-05 stage-1 settings.

## Function → test-case list (write before code)

- `updateStats_allKeptChildrenProvenWinForOpponentEdgesCut_noProvenLoss`
- `updateStats_allChildrenProvenWinForOpponentNoCut_provenLoss` (current behaviour kept)
- `updateStats_oneChildProvenLossForOpponentEdgesCut_provenWinKept`
- `createEdgesFrom_cutApplied_setsCutFlag`, `createEdgesFrom_noCut_cutFlagClear`
- position test: only saving reply has policy < 1 % in a node with > 8 replies; MCTS with
  `0.99` must not print a mate score nor override the best move.
- Added with the instruction file (2026-11-09), in `tests/mcts_bound_tests.cpp`:
  `updateStats_edgesCut_keptChildProvenLoss_parentWinLowerUnchanged`,
  `updateStats_edgesCut_allKeptChildrenDraw_upperNotCapped`, `edgeArray_sizeUnchanged`,
  `createEdgesFrom_minEdgesFloorCoversAllMoves_cutFlagClear` (boundary `numMoves == minEdges`).

## Verification

- bench bit-identical (`17621937` / `8c1f72e5`; built-in config is alpha-beta).
- unit tests with asserts live.

## Results — 2026-11-09

Measurement files: `build/search09/` (not in the repo). gcc Release for unit tests, bench, golden and
scan; PGO (`scripts/pgo_build.sh x64-clang-AVX512VNNI`) for self-play and a second golden/bench check.
Base = HEAD `42f029b`.

| Check | Base (HEAD) | Fix | Result |
|---|---|---|---|
| `sizeof(EdgeArray)` / `Edge` / `Node` | 8 / 16 / 56 | 8 / 16 / 56 (`static_assert`ed) | pass |
| Bench (gcc Release, built-in config) | 17621937 / 8c1f72e5 | 17621937 / 8c1f72e5 (PGO base and fix too) | pass |
| Tests (`rapfi_tests`, asserts live) | 140/140; new suite: 3 of 6 runtime tests fail, 3 accessor tests do not compile | 149/149 | pass |
| Golden, cut off (`1.0`), 1 thread, 300K nodes, `SHOW_DETAIL 2`, final root list (exact visits, 6-digit winrate, PV), 4 Standard positions incl. 2 with a proven mate at the root | deterministic over 2 runs | identical (gcc base vs fix, PGO base vs fix, gcc vs PGO) | pass |
| Mate-claim scan, `0.99`, 100K nodes, 6084 positions: root `+M` / `-M` | 736 / 728 | 731 / 725 | see below |
| Self-play fix vs base, PGO, `0.99`, Standard, 1 thread, 256 MB, `tc=0/2`, 120 games, SPRT [-10, 10] | | W-L-D 36-31-53, score 0.521, about +15 ± 24 Elo (1 SE), LLR 0.517 [-2.944, 2.944] | pass (tie inside noise) |

Failing first (`RAPFI_TEST_ISOLATE=1`, HEAD, accessor tests gated out; `tests_failing_first_head.txt`,
`tests_compile_fail_head.txt`):

- fail at runtime (`upper` is `mated_in(3)` / `mate_in(2)` / `0` instead of `+inf`):
  `updateStats_allKeptChildrenProvenWinForOpponentEdgesCut_noProvenLoss`,
  `updateStats_edgesCut_keptChildProvenLoss_parentWinLowerUnchanged`,
  `updateStats_edgesCut_allKeptChildrenDraw_upperNotCapped`.
- guards that pass at HEAD and must keep passing: `updateStats_allChildrenProvenWinForOpponentNoCut_provenLoss`,
  `updateStats_oneChildProvenLossForOpponentEdgesCut_provenWinKept`, `edgeArray_sizeUnchanged`.
- do not compile at HEAD (no `numMoves` / `edgesCut()`): `createEdgesFrom_cutApplied_setsCutFlag`,
  `createEdgesFrom_noCut_cutFlagClear`, `createEdgesFrom_minEdgesFloorCoversAllMoves_cutFlagClear`
  (the last one is the boundary `numMoves == minEdges` with `massKeep < 1`).

Golden: the periodic (time-driven) root lists differ between two runs of the same binary, so the
compared output is the final root list of the fixed-node search (`final_extract.sh`), which is
deterministic.

Mate-claim scan (`scan.py`, `positions.txt`): 6084 Standard positions, 1 thread, 100K nodes. The set
is the first 100 openings of `op_std_main.txt` plus, from the SEARCH-05 `s5*_pm99` self-play games,
every position where `s5_pm99` moved in under 300 ms and the position two plies earlier. 3412 of them
have a single root move and stop after `numNodesAfterSingularRoot` (100) nodes.

- Base `0.99` vs fix `0.99`: 10 positions changed (`cmp_on_base_vs_fix.txt`). 8 lose a mate claim
  (`411 +M27`, `1233 -M16`, `1242 +M19`, `1259 -M12`, `1662 +M21`, `4293 +M19`, `4693 -M18`,
  `5273 +M23`), same move in all 8. 2 keep a mate with another move (`1106 +M11 -> +M17`,
  `5905 +M17 -> +M19`).
- All 8 lost claims are true mates: `1.0` proves the same mate at 100K nodes for 7 of them, and for
  `411` at 2M nodes (`+M27`, same move), where the fix still reports 502 at 2M. These are proofs the
  fix loses (the expected cost of option (a)), not false proofs removed.
- Positions where base `0.99` claims a mate and `1.0` at 100K does not: `411`, `1276`, `1698`. All three
  are true mates (`verify_candidates.txt`): `411` and `1698` are proven by `1.0` at 2M nodes;
  `1276` has a single root move, and after it `1.0` at 2M proves `-M20` for the opponent.

**Position test: not reproduced.** No position in the scan shows a false mate under `0.99` at HEAD.
No position test was frozen; the synthetic unit tests are the failing tests. The bug stays a proven
soundness hole (unit tests), not an observed one.

Self-play: `build/oppo4-ab/s9_fix` vs `s9_base` (PGO, same `s5_pm99` config at `0.99`),
`OPFILE=op_std_main.txt run.sh s9_fix s9_base 1 120 tc=0/2`; log
`build/oppo4-ab/logs/s9_fix_vs_s9_base_r1_tc_0_2.txt`. No regression; the result does not show a gain
either, as expected for a rare event.

Outcome against "Expected": the fix is a soundness guarantee with no observed upside in this sample.
0 false proofs were found to remove; 8 true proofs out of 1464 root mate claims (about 0.5 %) were
lost at 100K nodes, all with the same move played. Option (a) is sound but not complete: a cut
defender node can never be proven lost at any node count (`411` still reports 502 at 2M nodes), which
matters for VCN-03 option B (see its status note).
