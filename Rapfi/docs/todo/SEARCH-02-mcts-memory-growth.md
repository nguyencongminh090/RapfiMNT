# SEARCH-02 — MCTS graph memory growth (edge-array size, expansion delay)

**Status:** done, opt-in (2026-11-07). Defaults unchanged.
**Class:** B (MCTS policy wiring). Defaults reproduce today's behaviour exactly; the AB bench
(`17621937` / `8c1f72e5`) is not touched because AB does not use these paths.

## Problem

A long MCTS search takes 90-99% of the machine's RAM. NNUE is fast, so nodes are created at a rate
proportional to NPS. Mid-search garbage collection cannot help: `recycleOldNodes` only runs in
`setupRootNode`, and any threshold-based collector chases an allocation rate it cannot outrun.
The memory budget (`graphMemoryLimit`) only stops the search, and `memoryLimitKB == 0` means
unbounded.

## Where the bytes go (from source, to be confirmed by measurement)

- `Node` + `std::set` entry: about `sizeof(Node) + 48` bytes.
- `EdgeArray`: `sizeof(Edge) = 16` bytes per edge, and `createEdges` stores **every** generated
  move, up to `MAX_MOVES`. A full 225-move node costs about 3.6 KB, roughly 30x a leaf node.
- Edge arrays are created on the node's second visit (`expandWhenFirstEvaluate = false`), so a
  never-revisited leaf costs only the node.

## Decision note (what / expected / how measured)

- **What (2) — policy-mass edge cut.** `createEdges` keeps the leading edges (list is sorted by
  policy) until their cumulative normalised policy reaches `mcts_edge_policy_mass`, but never
  fewer than `mcts_min_edges`; kept policies are renormalised to sum to 1. Applies to non-root
  nodes only: root keeps every move because root-move bookkeeping and best-move selection read
  all edges. Default `mcts_edge_policy_mass = 1.0` means off.
- **What (1) — expansion delay.** A leaf with fewer than `mcts_expand_min_visits` visits is not
  expanded: the visit is credited to the node with its static value (the same path a terminal node
  takes). Default `1` is today's behaviour (expand on the second visit).
- **Not done: progressive widening.** Adding edges later means replacing a published `EdgeArray`
  while other threads hold references to it. That needs a reclamation scheme (epoch or hazard
  pointers), which is a separate item.
- **Expected:** edge-array bytes per expanded node shrink by the ratio of kept edges to generated
  edges; a larger expansion delay lowers the share of nodes that own an edge array.
- **How measured:** fixed-node MCTS searches on a set of positions, comparing (a) estimated graph
  bytes (`graphMemoryUsed`), (b) peak RSS, (c) strength: self-play match of the new defaults versus
  the old ones at equal time. Numbers go in the Verification section.
- **Knowledge note:** `SKILLS_TREE/tools/find -k knowledge` has no note on MCTS memory or
  widening (checked 2026-11-07). The design follows the source, not a library note.

## Test list (written before code)

All in `tests/mcts_memory_tests.cpp`, named `<function>_<scenario>_<expectedBehavior>`.

`selectEdgeCount(policy, n, massKeep, minEdges)`:
1. `selectEdgeCount_massOne_keepsAllEdges`
2. `selectEdgeCount_massReachedMidList_cutsAfterTheEdgeThatReachesMass`
3. `selectEdgeCount_massReachedBeforeMinEdges_keepsMinEdges`
4. `selectEdgeCount_fewerMovesThanMinEdges_keepsAllMoves`
5. `selectEdgeCount_zeroMoves_returnsZero`
6. `selectEdgeCount_minEdgesZero_keepsAtLeastOneEdge`
7. `selectEdgeCount_massNonPositive_keepsOnlyMinEdges`

`readyToExpand(visits, minVisits)`:
8. `readyToExpand_defaultMinVisits_trueFromFirstVisit`
9. `readyToExpand_belowMinVisits_false`
10. `readyToExpand_atMinVisits_true`
11. `readyToExpand_nonPositiveMinVisits_alwaysTrue`

`Node::createEdgesFrom`:
12. `createEdgesFrom_noLimits_copiesAllMovesAndPolicies`
13. `createEdgesFrom_massCut_keepsPrefixAndRenormalisesToOne`
14. `createEdgesFrom_massCut_countsFootprintOfKeptEdgesOnly`
15. `createEdgesFrom_emptyList_returnsTrueAndLeavesNodeLeaf`
16. `createEdgesFrom_secondCall_keepsFirstArrayAndAddsNoFootprint`

Run with asserts live (`rapfi_tests` is built with `-UNDEBUG`).

## Verification

Done so far (2026-11-07, gcc Release `build/`, 1 thread, freestyle, 15x15):

- Unit tests: 102/102 with asserts live (16 new in `tests/mcts_memory_tests.cpp`).
- AB bench unchanged: Nodes `17621937`, Hash `8c1f72e5`.
- Defaults (`mass = 1.0`, `expand_min_visits = 1`) leave the MCTS path as before.

Memory, one position (`7,7 9,8 6,7 7,9`), `MAX_NODE 2000000`, `INFO MAX_MEMORY 6000000000`,
peak RSS from `/usr/bin/time`:

| config | peak RSS | time |
|---|---:|---:|
| base | 486 MB | 14.5 s |
| mass 0.99 | 217 MB | 14.6 s |
| mass 0.95 | 161 MB | 13.2 s |
| expand 2 | 311 MB | 9.5 s |
| expand 3 | 235 MB | 7.3 s |
| mass 0.99 + expand 2 | 151 MB | 8.6 s |
| mass 0.97 + expand 3 | 105 MB | 6.7 s |

Note: with the default memory setting a search stopped at about 60K nodes because the graph
budget was reached, so the budget works but ends the search instead of changing its behaviour.

Strength (stopped by the user at 230 of 300 games, conclusion: equal): self-play, freestyle,
256 MB per engine, `tc=0/2`, 1 thread, `run.sh s2cand s2base`; cand = `mass 0.97` +
`expand_min_visits 3`, base = defaults. Score at stop: cand 105 - base 103 - draw 22 (0.504),
LLR within [-2.944, 2.944]. Equal strength at about 1/4.6 of the memory (105 MB vs 486 MB on the
2M-node measurement). Defaults are left unchanged (opt-in). Only one rule, one time control and
one memory size were tested.

MCTS versus alpha-beta (2026-11-07): `run.sh s2cand s2ab 0 100 tc=0/2`, same conditions (freestyle,
256 MB, 1 thread). cand (MCTS, `mass 0.97` + `expand 3`) 43 - alpha-beta 50 - draw 7 (0.465, about
-24 Elo, error bar roughly +-70 at 100 games), LLR -0.435 inside [-2.944, 2.944]. Not
distinguishable from equal, with a slight lean to alpha-beta.
