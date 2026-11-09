# SEARCH-11 — MCTS edges not fully sorted by policy (partial sort keeps score < 0 unsorted)

**Status:** closed 2026-11-09 (user), measured negligible, no code change. Recorded 2026-11-09, user. Found during
VCN-03 slice 3. Step 1 on the real network: 1.23 % of expansions not in descending policy order, but
only 0.012 % of cuts keep a worse prefix (mean misplaced mass 0.0009); see "Step 1 result". Possible
future option: a build-assert check of the order, or a full sort if its NPS cost is under 0.5 %.

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

## Step 1 result (2026-11-09, temporary counter, not committed)

- Counter in `Node::createEdgesFrom` (all non-root expansions): checks that `policyList` is
  non-increasing, and compares the policy mass of the kept prefix with the ideal top-k mass for the
  same k chosen by `selectEdgeCount`. (Root goes through `createEdges`; not counted.)
- Set: 122 positions (every 50th line of `build/search09/positions.txt`), real network
  `model260809.bin`, `cfg_on` (`mcts_edge_policy_mass = 0.99`), 100K nodes, 1 thread each.
- Result: 788,380 expansions with more than one move; **9,731 (1.23 %) not in descending policy
  order**. Of 595,612 expansions where the 0.99 cut dropped moves, **70 (0.012 %) kept a prefix
  with less mass than the ideal top-k**, average misplaced mass **0.0009**.
- Reading: the misordering is real but sits in the low-policy tail (score < 0 moves), so the cut
  loses almost nothing. The effect on the first-unexplored-is-best rule was not measured separately;
  it involves the same low-prior moves.
- Recommendation (open for the user): low value. Either close as measured-negligible, or fix only
  for correctness (full stable sort on the normalised-policy path) if the change is wanted.

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
