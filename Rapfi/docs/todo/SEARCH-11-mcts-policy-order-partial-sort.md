# SEARCH-11 — MCTS edges not fully sorted by policy (partial sort keeps score < 0 unsorted)

**Status:** open (recorded 2026-11-09, user). Found during VCN-03 slice 3. Not yet measured on the real
network.

**Class:** B (MCTS search behaviour: edge order, edge cut).

## Problem

- With normalised policy on, `MovePicker::scoreAndSortMoves` (`search/movepick.cpp:555`) and the
  ROOT path (`movepick.cpp:267`) call
  `fastPartialSort(curMove, endMove, 0, ScoredMove::PolicyComparator {})`.
- `fastPartialSort` (`search/movesort.h`) sorts only the moves with `score >= limit` (`0`). For
  `n <= MAX_MOVES / 4` (121) that is the insertion branch. The moves with negative `score` stay in
  generation order after the sorted prefix. The comparator orders by `policy`, but the limit tests
  `score`.
- MCTS assumes that every edge list is in descending policy order:
  - `selectEdgeCount` (the SEARCH-02/05 cut) keeps a prefix by cumulative mass, so it can drop a
    move with a higher policy than one it keeps;
  - `selectChild` treats the first unexplored edge as the highest remaining prior
    (`search/mcts/search.cpp`, comment near "first unexplored child"), so it can pick the wrong
    unexplored move and give it the wrong FPU/PUCT term;
  - the root (`ROOT` picker) is not cut, but `selectChild` at the root has the same ordering
    assumption.
- Alpha-beta uses `ScoreComparator` on the same partial sort on purpose (moves below the limit stay
  unordered). This task concerns only the normalised-policy (MCTS) path.
- PERF-02 replaced the insertion branch for `ScoreComparator` with a byte-identical vectorised
  version, so this behaviour predates PERF-02.

## Evidence so far

- MAIN stage: reproduced in a test precondition (VCN-03 slice 3) with a fake evaluator that returns
  negative logits.
- DEFENDFOUR / DEFENDB4F3 and ROOT: inferred from code (the same call).
- Real network: unknown. Whether its `score` (the raw policy logit scale) is often negative was
  inferred only from a comment near `vcnThreatBonus` (about -111..387). This must be measured first.

## Decision note (draft)

- **What:** first measure; then, if the real network produces negative scores in MCTS expansion,
  make the normalised-policy path sort the whole list. Two options: a full stable sort by
  `PolicyComparator` when `useNormalizedPolicy`, or a `limit` that admits every move on that path.
  Fix it in the picker or sort layer (CLAUDE.md rule 4), not in MCTS.
- **Expected:** alpha-beta unchanged (bench bit-identical, since only the normalised path changes).
  The MCTS edge order and cut change wherever negative scores occur. Strength equal or better.
- **How measured:**
  - **Step 1, before any fix:** on a position set (for example `build/search09/positions.txt`),
    count expansions where the picker output is not in descending policy order, and the policy mass
    that is misplaced or wrongly cut at `0.99`. Use a temporary counter, not committed.
  - **Then:** the bench, MCTS golden (it will change: record why), self-play fix vs base with the
    SEARCH-05 stage-1 settings, peak RSS.
- VCN-03 slice 3 already re-sorts the whole list when a PASS is generated. Once this is fixed, that
  re-sort may become redundant. Keep it until then.

## Function → test-case list (write before code)

- `movePickerMain_normalizedPolicyNegativeScores_fullyDescendingByPolicy`
- `movePickerRoot_normalizedPolicyNegativeScores_fullyDescendingByPolicy`
- `movePickerDefendFour_normalizedPolicy_fullyDescendingByPolicy`
- `movePickerMain_scoreComparatorPath_unchanged` (alpha-beta order bit-identical)
- `createEdges_pickerOutput_policiesNonIncreasing` (MCTS-level guard)

## Verification

- Bench `17621937 / 8c1f72e5`. Unit tests with asserts live.
- Step-1 counts recorded before deciding the fix.
- Self-play and RSS as above if the MCTS output changes.
