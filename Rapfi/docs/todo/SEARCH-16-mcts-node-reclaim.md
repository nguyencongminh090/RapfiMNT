# SEARCH-16 — MCTS: keep the search useful after the graph memory budget is full

**Status:** done 2026-11-11. Option 5 implemented; `mcts_reclaim_target_percent` default `50` (user
decision after the measurements in "Results"; `0` gives the old closed-graph behaviour).
**Class:** B (search behaviour on budget-limited runs; unbounded runs must stay byte-identical).

## Problem

When `graphMemoryUsed` reaches the budget, the graph closes (SEARCH-03, `searcher.graphClosed()`).
From then on, in `search/mcts/search.cpp`:

- `searchNode` expands no leaf; a visit to a leaf is credited with the leaf's stored value;
- `selectChild` offers no unexplored edge (playouts stay in explored nodes);
- `widenNode` adds no cut move back (SEARCH-10);
- `recycleOldNodes` runs only in `setupRootNode`, i.e. between searches, never inside one.

So after the close no network evaluation is added. Playouts only re-weight the existing graph:
values converge to the fixed point of that graph and then stop moving, and a wrong leaf value can
no longer be corrected (this amplified the VCN-04 error).

Measured (VCN-04 item 6, `build/vcn04/logs/vc2_30m_new.log`; Renju, 8 threads, 8 GB, 30 min asked):

| Time | Best move | Win rate | Visits | n/ms |
|---|---|---|---|---|
| 151 s | F9 | 53.78 % | 42M | 284 |
| 181 s | F9 | 53.69 % | 49M | 277 |
| 301 s | F9 | 56.45 % | 1241M | 4124 |
| 901 s | F9 | 56.64 % | 7632M | 8471 |
| 1129 s (end) | F9 | 56.64 % | 8611M | 7630 |

The graph closed between 181 s and 241 s at about 50M visits (about 160 bytes per visit). The value
was stable from about 300 s; the remaining time changed it by 0.2 points.

Reference read 2026-11-11: KataGomo (branches `Gom2026` and
`gom2026-avx2-vnni-b11-b16-b24-cpu-ptq`, identical `cpp/search/`) has no memory budget for the
tree and ignores `INFO MAX_MEMORY`; it limits the search by visits / playouts / time and collects
unreachable nodes only when the root changes. Nothing to reuse for this problem except the idea of
limiting the search instead of the memory. Knowledge lookup (SKILLS_TREE): no matching note.

## Options (none chosen)

1. **Stop when the closed graph has settled.** End the search some time after the close, when root
   values stop moving. Smallest change; saves time, adds no strength per move.
2. **Expansion threshold that rises with memory use.** `mcts_expand_min_visits` (SEARCH-02) is a
   constant; make the required visit count grow with the fill ratio (example: below 50 % as now,
   75 % 4 visits, 90 % 16 visits). No stop-the-world pass, no dangling pointer, parent statistics
   untouched; the graph slows down instead of closing.
3. **Reclaim inside a search.** When the graph closes: stop all threads at a safe point, choose a
   visit threshold from a histogram so that about 30 to 50 % of the memory is freed, mark from the
   root through edges whose child passes, erase unmarked nodes (`eraseNodesIf`, as
   `recycleOldNodes` does), reset edges that point to erased nodes to "unexplored", reopen.
   - Criterion: low visits, not low Q. Never erase proven nodes (terminal / bound) or nodes on the
     root principal variations.
   - Reversible only: an erased branch can be selected and expanded again. No permanent ban of a
     move (a banned defence at a defender node gives a false win, the VCN-04 class of error).
4. **Collapse instead of erase** (variant of 3 for large subtrees): only branches with at least N
   visits whose upper confidence bound is below the lower bound of the best sibling; drop the
   subtree, keep the branch's own node with its value and visits, so the parent's statistics do
   not change.

User's idea (2026-11-11): before deleting a branch, search it up to a visit threshold and delete
it only if its value is still low. Assessment given to the user: the intent (evidence before
deletion) is kept by option 4's "at least N visits" plus the confidence-bound test; forcing extra
visits is not recommended because it needs memory when memory is full, costs N visits for each of
tens of millions of low-visit leaves, and biases the parent's average. Not decided.

Recommendation given: try option 2 first, measure, then decide whether 3 / 4 is needed.

## Findings (2026-11-11, second session)

Harness and logs: `build/search16/` (`run.sh`, `sweep.sh`, `ex<T>.toml`, `logs/`); the scripts expect
the weights and the binary of `build/vcn04/net` in the working directory. Binary:
`build/vcn04/net/pbrain-rapfi` (PGO, sources of 4ff8684).

### Open questions answered from the code

- **An edge cannot be reset in the middle of an edge array (option 3 as written is wrong).**
  `Node::updateStats` (`node.cpp`, the `childN == 0` branch) and `selectChild` (`search.cpp`, the
  `!childNode` branch) both stop at the first unvisited edge: visited edges must be a prefix of
  the array. If an edge in the middle is reset, every visited edge after it leaves the parent's
  statistics and is no longer selectable, and `selectChild` treats the reset edge as "the first
  unexplored edge". A design that erases a child must either compact the parent's edge array
  (keeping the policy order of the unexplored tail) or never touch a parent's edge array.
- **A collapsed node loses its value when it is expanded again (option 4).** A leaf keeps `q`
  while it is only visited (`incrementVisits`), but the first `updateStats` after a new expansion
  restarts from `utility` (the raw evaluation, weight 1) plus the new children. The accumulated
  value is replaced at once, while the parent's edge still weights the node by its old visits.
  Also `readyToExpand` reads the node's own visits, so a collapsed node with many visits expands
  again on its next visit. For a node with few visits the loss is small (it is what a new node
  does); for a large subtree it is not.
- **A re-expansion can loosen a proven bound.** `updateStats` recomputes `bound` from the
  children; a node that lost its children would go back to (-inf, inf). So a node with a
  non-trivial bound must keep its edge array.
- **Sweep cost.** `recycleOldNodes` between two searches on a full 1 GB graph, 8 threads: 3.33M
  nodes erased in at most 0.31 s (the time includes the command and the root evaluation), about
  93 ns per node. Scaled to 8 GB (about 27M nodes): about 2.5 s for one pass, against about 200 s
  to fill the graph.
- **Byte-identical runs.** Every option acts only after `graphClosed()` is true inside a search;
  an unbounded graph never closes, and the bench runs alpha-beta.

### Option 2 measured: a larger expansion threshold delays the close, it does not remove it

Constant `mcts_expand_min_visits` (config only), Renju, the VCN-04 item 6 position, ordinary
search (`YXNBEST 1`), 8 threads, `INFO MAX_MEMORY` 1 GB, 75 s asked (about 67 s used):

| Threshold | Close (s) | Visits at the close | n/ms before the close | Root at the end |
|---|---|---|---|---|
| 1 (default) | 25 to 30 | about 7M | about 290 | F9 57.11 %, frozen from 40 s |
| 4 | 25 to 30 | about 21M | about 880 | F9 58.06 % |
| 16 | 40 to 45 | about 62M | about 1600 | F9 55.93 %, still moving |
| 64 | not closed at 65 s | 171M at 65 s | about 2700 | J7 57.04 % (another best move) |

A threshold of 4 gives three times the visits but closes at the same time: the time goes to
evaluations, and the memory bounds the number of evaluated nodes (a bare node is 75 B, a visit
costs 120 to 160 B today, so the remaining memory holds at most about twice the nodes). A
threshold of 16 moves the close by a factor of 1.5; 64 moves it further but changes the best move,
and SEARCH-05 measured constant thresholds 2 and 3 at about -40 Elo. A threshold that rises only
above 50 % fill gains less than these constant ones. So option 2 cannot keep a 30 minute search
useful; the earlier recommendation ("option 2 first") is withdrawn. Only reclaim gives new
evaluations for the whole search.

### Option 5 (proposed): trim low-visit nodes back to leaves, then sweep

Reclaim without touching any parent's edge array, so the prefix rule holds everywhere:

1. When the graph closes inside a search, all search threads park at the top of the playout loop
   in `MCTSSearcher::search` (between two root playouts a thread holds no node or edge pointer).
2. A visit threshold V is chosen from a histogram of node visits so that the pass frees the
   target share of the budget (first value to measure: down to 50 % of the soft limit).
3. Mark from the root as `recycleOldNodes` does. A reachable non-root node with an edge array, at
   most V visits and a trivial bound is trimmed: its edge array is freed and it becomes a leaf
   that keeps `q`, `d`, visits and `bound`. The traversal does not descend below it.
4. Sweep: nodes not marked are erased (`eraseNodesIf`, the existing code). Retired edge arrays
   (SEARCH-10) are freed at the same quiescent point. The threads resume and the graph is open.

Properties: criterion is low visits, not low value; nothing is banned, a trimmed node expands
again on its next visit (one policy evaluation, children evaluated again); parents' statistics do
not change; proven nodes and the root keep their edges; memory moves to the lines the selection
still visits. This is option 3's criterion with option 4's mechanics, applied only to low-visit
nodes, where the value loss on re-expansion is small. The user's "evidence before deletion" is
the visit count itself: a node is trimmed because the selection stopped visiting it, and the
trim is undone as soon as the selection returns.

Open design points: the config key and default (off until measured); whether a trimmed node
should store `q` in `utility` so that a re-expansion starts from the searched value; a parallel
or single-thread pass; the lowest V and the behaviour when the high-visit part alone exceeds the
budget (the graph then closes as today).

## Open questions (answered above, kept for the record)

- `Node::updateStats`: what happens to a parent's value when an edge's visits are reset to 0
  (option 3), and when a collapsed node is expanded again (option 4: is the stored value lost at
  once)? Not read yet.
- Cost of one mark-and-sweep pass at about 50M nodes (not measured; `recycleOldNodes` does the
  same work between moves).
- The edge is 16 bytes with no value field (`search/mcts/node.h`); a design that needs a per-edge
  value summary changes the edge layout (SEARCH-08 / PERF-03 budget math).
- How to keep unbounded runs and runs that never fill the budget byte-identical.

## Decision note (rule 9) — 2026-11-11, option 5 chosen by the user

- **What:** reclaim inside a search (option 5). New config key `mcts_reclaim_target_percent`
  (int, default `0` = off, today's behaviour). When it is above 0 and the graph closes inside a
  search, the search threads park at the top of the playout loop (`SafepointGate`), and one pass
  (`MCTSSearcher::reclaimNodes`) runs on all of them:
  1. free the retired edge arrays (SEARCH-10; the point is quiescent);
  2. histogram over the trimmable nodes (not the root, expanded, not terminal, bound
     (-inf, inf)): estimated bytes freed per visit bucket = edge array + explored children
     x `nodeFootprint()`; choose the smallest bucket bound V whose cumulative estimate reaches
     `used - target`, target = `softGraphLimit(limit) * percent / 100`;
  3. mark from the root (new node age), without descending below a trimmable node with at most
     V visits;
  4. sweep each shard: erase unmarked nodes (as `recycleOldNodes`), release the edge array of
     marked trimmable nodes with at most V visits (`Node::releaseEdges`), keeping `q`, `d`,
     visits, `bound`, `utility`.
  The pass is skipped for the rest of the search when it frees less than 1/8 of the soft limit
  or leaves the graph closed (no thrash), or when the remaining turn time is shorter than twice
  the estimated pass time. No parent edge array is written, so the visited-prefix rule holds.
- **Expected:** with the key on, the root value keeps moving after the first close and the node
  rate does not jump to the closed-graph rate; a bounded long search beats the closed graph in
  self-play at a small memory budget. With the key off (default) every run is byte-identical.
- **How measured:** bench `17621937 / 8c1f72e5`; fixed-node MCTS output identical to the
  pre-change binary with the key off; unit tests with asserts live, plus TSan and ASan runs of
  the reclaim tests; timeline of the item 6 position at 1 GB with the key on against
  `build/search16/logs/ex1.log`; self-play key on vs off at a small budget and a time control
  that fills it. The default changes only if self-play does not lose.
- **Knowledge note:** none matches (SKILLS_TREE lookup of the first session).

## Test list (written before the code)

`search/mcts/reclaim.h`, file `tests/mcts_reclaim_tests.cpp`:

`visitBucket(visits)` / `bucketMaxVisits(bucket)`:
1. `visitBucket_belowEight_isTheVisitCount`
2. `visitBucket_oneToManyVisits_monotonicWithoutGaps`
3. `bucketMaxVisits_everyBucket_isTheLargestVisitCountOfTheBucket`
4. `visitBucket_largestVisitCount_insideTheHistogram`

`ReclaimHistogram`:
5. `thresholdFor_emptyHistogram_zero`
6. `thresholdFor_zeroNeed_zero`
7. `thresholdFor_needMetByFirstBucket_thatBucketBound`
8. `thresholdFor_needSpansBuckets_smallestSufficientBound`
9. `thresholdFor_needAboveTotal_boundCoveringThreeQuartersOfTheBytes`
9b. `thresholdFor_needAboveTotalAndBytesInTopBucket_topBucketBound` (added with the 3/4 cap, see results)
10. `merge_twoHistograms_sumsBuckets`

`reclaimTargetBytes(limit, percent)`:
11. `reclaimTargetBytes_fiftyPercent_halfOfSoftLimit`
12. `reclaimTargetBytes_nonPositivePercent_zero`
13. `reclaimTargetBytes_percentAbove100_softLimit`

`Node::releaseEdges()`:
14. `releaseEdges_expandedNode_leafAgainAndReturnsFootprint`
15. `releaseEdges_leaf_returnsZero`
16. `releaseEdges_expandedNode_keepsValueVisitsAndBound`
17. `releaseEdges_thenCreateEdgesFrom_expandsAgain`

`nodeTrimmable(node, maxVisits)`:
18. `nodeTrimmable_leaf_false`
19. `nodeTrimmable_visitsAtMax_true`
20. `nodeTrimmable_visitsAboveMax_false`
21. `nodeTrimmable_provenBound_false`
22. `nodeTrimmable_terminal_false`

`SafepointGate`:
23. `park_nothingRequested_returnsAtOnce`
24. `tryBegin_singleActiveThread_trueAtOnce`
25. `tryBegin_secondCallerWhileHeld_false`
26. `tryBegin_otherThreadsRunning_returnsOnlyWhenAllParked`
27. `tryBegin_otherThreadLeaves_isNotWaitedFor`
28. `enter_whileStopped_blocksUntilEnd`
29. `runJob_parkedThreads_everyThreadRunsItOnceWithDistinctIndex`
30. `end_parkedThreads_resume`
31. `tryBegin_manyRoundsFromCompetingThreads_oneCoordinatorAtATime`

`MCTSSearcher::reclaimNodes(target, run)` on a hand-built graph (root, a high-visit child, a
low-visit expanded child with its own children, one grandchild shared with the high-visit child):
32. `reclaimNodes_usedAtOrBelowTarget_changesNothing`
33. `reclaimNodes_lowVisitNode_trimmedToLeafKeepingStats`
34. `reclaimNodes_childOnlyUnderTrimmedNode_erased`
35. `reclaimNodes_childSharedWithKeptNode_kept`
36. `reclaimNodes_rootWithFewVisits_keepsEdges`
37. `reclaimNodes_provenLowVisitNode_keepsEdges`
38. `reclaimNodes_keptParent_edgesAndStatsUnchanged`
39. `reclaimNodes_afterPass_graphMemoryUsedEqualsLiveFootprint`
40. `reclaimNodes_retiredEdgeArrays_freedAndUncounted`
41. `reclaimNodes_parallelRun_sameResultAsSingleThread`

Search integration (engine fixture of `mcts_nodetable_tests.cpp`):
42. `search_reclaimOffSmallBudget_noPassAndGraphStaysClosed`
43. `search_reclaimOnSmallBudget_passRunsAndCountersMatchLiveFootprint`
44. `search_reclaimOnSmallBudgetFourThreads_passRunsAndCountersMatchLiveFootprint`
45. `search_reclaimOnUnboundedGraph_noPass`
46. `search_reclaimOnLongSearch_graphRefillsAndSecondPassRuns` (a second pass needs the graph
    to grow again, that is trimmed nodes expanded again)
47. `searchConfig_defaultReclaimTargetPercent_isZero`

## Results — 2026-11-11

Code: `search/mcts/reclaim.h` (new: visit histogram, `reclaimTargetBytes`, `nodeTrimmable`,
`SafepointGate`), `Node::releaseEdges`, `MCTSSearcher::reclaimNodes` / `tryReclaim` and the
safe point at the top of the playout loop in `MCTSSearcher::search`, config key
`mcts_reclaim_target_percent` (`searchconfig.h`, `config.cpp`), tests
`tests/mcts_reclaim_tests.cpp`. Harness and logs: `build/search16/` (`run2.sh`, `run/`, `sp/`).

### Deviations from the decision note

- **One round trims at most the lower 3/4 of the trimmable bytes; a pass runs up to 4 rounds.**
  Found with the first binary at 16 MB on a position near 90 % (many proven nodes): the
  histogram does not see what is freed below a trimmed node when that memory belongs to proven
  nodes or to children with more visits than their parent, so the need could not be met and the
  threshold went to the largest bucket (`visits <= 2621439, trimmed 2, 12 -> 0 MB`: the principal
  line was trimmed and the graph emptied). `ReclaimHistogram::thresholdFor` now caps the need at
  3/4 of the histogram, and `tryReclaim` repeats rounds until the graph is at most halfway
  between the target and the soft limit. Same position after the change: thresholds 6, 13, 23,
  31, 47 ... and `12 -> 5 MB` every pass. Test 9 replaced, 9b added.
- A pass is given up for the rest of the search when the time left is below twice the estimated
  pass time, measured against `timectl.optimum()` (the MCTS stop condition), not `maximum()`.
- Test 46 was replaced before it was written (see the list): the engine fixture cannot name one
  trimmed node, a second pass shows the regrowth.
- Not done: storing `q` in `utility` on a trim (open design point). No measurement asks for it.

### Checks

- Unit tests: `build/s16-assert` (gcc, RelWithDebInfo, `FORCE_ASSERTION=ON`), **319/319**
  (48 new), `--check-asserts-live` aborts as it must.
- ASan (`build/s16-addr`): 319/319, no report. TSan (`build/s16-thre`): 319 tests pass; 1145
  reports, every one in `search/hashtable.cpp` (the VCF TT races already known from PERF-03's
  review), none in the reclaim code or the gate.
- Bench: gcc Release `17621937 / 8c1f72e5`; PGO `x64-clang-AVX512VNNI` `17621937 / 8c1f72e5`.
- Key off: fixed 300K nodes, 1 thread, Renju, the item 6 position, new PGO binary against
  `build/vcn04/net/pbrain-rapfi`: the final ten root lines and the move are identical
  (`logs/gold_old.log`, `logs/gold_new.log`).

### Timeline, key on (`rc50.toml`), 8 threads, 1 GB, item 6 position, 75 s asked

`logs/rc50_1g.log`: four passes, each about 0.4 s for 800 MB (`803 -> 366 MB`, thresholds 7, 15,
23, 31; about 500K nodes trimmed and 1.3M erased in the first). The node rate stays at 250 to
320 n/ms for the whole search (the closed graph of `ex1.log` goes to 2400 to 13400 n/ms, that is
no evaluation). The root value keeps moving (55.1 / 56.8 / 57.6 / 56.6 / 55.5 / 54.9 % at 10 s
steps; the closed graph is frozen at 57.11 % from 40 s). The value itself is not a quality
measure: two runs of this setup ended at 50.9 % and 54.9 % (8 threads are not deterministic).
For reference, `build/vcn04/logs/ord_30m.log` (8 GB, the graph open until about 150 s) falls from
55 % to 50.1 % while it is open and jumps to 57 to 58 % after the close: the closed-graph value is
not the value of the open search.

### Self-play, `mcts_reclaim_target_percent = 50` against `0`, same PGO binary

`build/search16/sp/run.sh` (the `oppo4-ab` runner, colour swap, SPRT [-10, 10]):

| Leg | Setup | Games | W-L-D | Score | Elo (about) | SPRT |
|---|---|---|---|---|---|---|
| 1 | Renju, 1 thread, 16 MB, tc=0/2 | 111 | 79-31-1 | 0.716 | +160 | H1 accepted (LLR 3.44) |
| 2 | Standard, 2 threads, 32 MB, tc=0/2 | 60 (cap) | 38-21-1 | 0.642 | +100 | not ended (LLR 1.08) |

No game was lost on time or by a crash in either leg.

### Long search: 8 GB, 30 min asked, 8 threads, item 6 position, key 50 (commit a83d406)

`logs/rc50_8g_30m.log` against `build/vcn04/logs/ord_30m.log` (the VCN-04 binary, same position
and limits, graph closed from about 150 s). Both searches ended at 1129 s and played F9.

| Time | Key 50: win rate | Visits | n/ms | Closed graph: win rate | n/ms |
|---|---|---|---|---|---|
| 120 s | 51.68 % | 45M | 381 | 50.72 % | 337 |
| 240 s | 49.69 % | 147M | 615 | 58.19 % | 3444 |
| 480 s | 49.72 % | 250M | 522 | — | — |
| 720 s | 51.47 % | 404M | 562 | — | — |
| 960 s | 51.57 % | 591M | 615 | — | — |
| end (1129 s) | 51.30 % | 740M | 655 | 58.55 % | 8389 |

Nine passes, at 156, 271, 377, 470, 568, 730, 867, 990 and 1114 s:

| Pass | Threshold (visits <=) | Trimmed | Erased | MB before -> after |
|---|---|---|---|---|
| 1 | 6 | 4.04M | 9.21M | 6478 -> 3096 |
| 2 | 15 | 3.11M | 12.68M | 6478 -> 2974 |
| 3 | 27 | 2.33M | 12.65M | 6478 -> 2983 |
| 4 | 47 | 1.90M | 13.60M | 6478 -> 2883 |
| 5 | 63 | 1.52M | 12.50M | 6478 -> 3029 |
| 6 | 111 | 1.87M | 16.20M | 6478 -> 3041 |
| 7 | 127 | 1.66M | 12.99M | 6478 -> 3346 |
| 8 | 159 | 1.58M | 13.03M | 6478 -> 3456 |
| 9 | 191 | 1.41M | 12.36M | 6478 -> 3552 |

- Pause per pass: at most 3.5 to 4.1 s (time between the root lines printed before and after
  each pass, so an upper bound), about 35 s of 1129 s in total (3 %).
- The root value stays between 48.5 % and 51.7 %, the range of the open graph before any close
  (the old run falls to 50.1 % before its close); the closed graph ends at 58.55 %.
- The threshold rises with every pass (6 to 191), and from pass 7 the pass ends above the 50 %
  target (3240 MB): the part of the graph above the threshold grows. Not followed further.
- The visit rate after the first pass is 510 to 655 n/ms against about 385 before it. The cause
  was not determined (the log has no count of network evaluations), so the visit rate is not a
  count of new evaluations here.

### Not measured

- Budgets where the graph does not fill: the key cannot act there (no pass runs), so no leg.
- Other target percentages (30, 70; configs are in `build/search16/run/`), freestyle, VCN mode.
- Pondering and `YXHASHCLEAR`-style commands during a pass.

### Default changed to 50 (user decision, 2026-11-11)

`mctsReclaimTargetPercent` default `0` -> `50`. The user decided against a further VCN run before
the change. Consequences: a budget-limited MCTS search that fills its graph now differs from the
previous binary; a search that never closes the graph and the alpha-beta bench do not. Tests
319/319 (asserts live; `searchConfig_defaultReclaimTargetPercent_isFifty` replaces test 47, and
`recycleOldNodes_budgetRunAfterSlabRelease_graphClosedFollowsLiveFootprint` sets the key to 0
because it needs a graph that is still closed after the search); bench gcc Release
`17621937 / 8c1f72e5`. Not re-run after this one-line change: PGO build, sanitizers, self-play
(the self-play legs already compare 50 against 0).
