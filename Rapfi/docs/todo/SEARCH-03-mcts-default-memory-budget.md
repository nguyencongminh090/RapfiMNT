# SEARCH-03 — MCTS default graph memory budget

**Status:** done, partly measured (2026-11-07). No self-play run yet.
**Class:** B (changes when MCTS stops searching).

## Problem

SEARCH-02 shrinks nodes but does not bound them. `setMemoryLimit(0)` leaves the graph unbounded
(`memoryLimitKB == 0`), so a long analysis search can still fill 90-99% of RAM. When a budget is
set, the search loop only `break`s at the limit (`MCTSSearcher::search`) and recycling runs only in
`setupRootNode`.

## Scope

1. A default budget derived from available RAM (for example a fraction of free memory) when no
   `MAX_MEMORY` / `HASH_SIZE` is given.
2. Graceful behaviour at the limit: stop inserting new nodes at about 85% of the budget and keep
   searching the existing graph (evaluate leaves without storing them), instead of ending the
   search.
3. Optionally recycle stale nodes during a search at a safe synchronisation point.

## Decision note (what / expected / how measured)

- **What (1) — default budget.** When no `MAX_MEMORY` / `HASH_SIZE` is set (`memoryLimitKB == 0`),
  the graph budget is resolved at the start of each search as
  `mcts_default_memory_percent` % of (available physical RAM + bytes the graph already holds).
  Percent `0` keeps the old unbounded behaviour; if the platform cannot report available RAM the
  budget stays unbounded. Default `40` (two engines on one machine stay under 80%). An explicit
  budget is used as given. A `bounded` flag replaces the `memoryLimitKB != 0` test, because the
  derived limit can be set while `memoryLimitKB` stays 0.
- **What (2) — closed frontier at 85%.** Once `graphMemoryUsed >= 85%` of the budget the graph is
  *closed*: `selectChild` no longer offers the first unexplored edge, and a leaf is not expanded
  (it is credited with its static value, the path of SEARCH-02's expansion delay). Search keeps
  refining the existing graph; the hard stop at 100% stays as a backstop. When no explored edge is
  selectable at a non-root node the playout ends with a static visit.
  Deviation from the problem statement ("evaluate leaves without storing them"): an evaluated
  child has to be an `Edge::child` target for `updateStats` to see its value, so an unstored
  evaluation would be lost. Closing the frontier achieves the same bound without a second value path.
- **What (3) — mid-search recycle: not done.** Needs a synchronisation point where no thread holds
  node pointers; a separate item. `setupRootNode` now also recycles when the graph is closed.
- **Expected:** peak RSS of a long unbounded analysis search is bounded near the derived budget
  instead of 90-99% of RAM; the search keeps running instead of ending at the limit.
- **How measured:** peak RSS of a long search (analysis, no time limit) under a small forced
  budget and under the default; self-play of the new build against the previous at equal time.

## Test list (written before code)

All in `tests/mcts_memory_tests.cpp` (new header `search/mcts/membudget.h`).

`deriveDefaultGraphBudget(availableKB, graphUsedBytes, percent)` (0 = unbounded):
1. `deriveDefaultGraphBudget_percentZero_returnsUnbounded`
2. `deriveDefaultGraphBudget_unknownAvailable_returnsUnbounded` (available 0 and graph empty)
3. `deriveDefaultGraphBudget_fortyPercent_ofAvailablePlusGraph`
4. `deriveDefaultGraphBudget_percentAbove100_clampedTo100`
5. `deriveDefaultGraphBudget_hugeAvailable_doesNotOverflow`
6. `deriveDefaultGraphBudget_tinyResult_isAtLeastOneByte` (never silently returns the unbounded 0)

`softGraphLimit(limit)`:
7. `softGraphLimit_isEightyFivePercent`
8. `softGraphLimit_hugeLimit_doesNotOverflow`

`graphInsertionClosed(bounded, used, limit)`:
9. `graphInsertionClosed_unbounded_neverCloses`
10. `graphInsertionClosed_belowSoftLimit_open`
11. `graphInsertionClosed_atSoftLimit_closed`
12. `graphInsertionClosed_aboveHardLimit_closed`
13. `graphInsertionClosed_zeroLimitBounded_closed`

Not unit-tested here: the `selectChild` / `searchNode` wiring (anonymous-namespace templates,
verified by the behavioural runs below). The "counter consistency after a recycle sweep" item is
dropped with scope item 3.

## Measure

Peak RSS on a long search (analysis mode, no time limit) and self-play against the current build at
equal time. See the SEARCH-02 table for the harness (`build/oppo4-ab/run.sh`).

## Verification (2026-11-07, gcc Release `build/`)

- Unit tests 115/115 with asserts live (13 new). AB bench unchanged: `17621937` / `8c1f72e5`.
- Closed frontier, `INFO MAX_MEMORY 120000000`, `MAX_NODE 3000000`, 8 threads, one position:
  old build stops at 262K nodes (490 ms, the 100% hard stop, peak RSS 155 MB); new build keeps
  searching to the 3M-node cap (591 ms) with peak RSS 138 MB. Memory stays bounded.
- Caveat: once closed, a playout ends with a static visit, so it is cheap and still counts as a
  node. `MAX_NODE` and `n/ms` are inflated at that point, and a timed search spins on cheap visits
  instead of deepening. Whether the closed graph plays better than stopping is NOT measured;
  needs the self-play harness at equal time (open follow-up, see SEARCH-05).
- Findings that limit the reach of the default budget:
  1. `MCTSSearcher::memoryLimitKB` is never 0 in the Gomocup flow: `setupSearcher` carries the
     alpha-beta TT size (16 MB default) over, so a GUI that sends no `MAX_MEMORY` gets 16 MB, not
     an unbounded graph. The auto budget only applies when the limit is truly unset (selfplay /
     opengen with `hashSizeMb 0`, library use). It is not exercised end to end; only the pure
     functions are tested.
  2. Pre-existing bug fixed on the way (class C): `INFO RULE` with an unset limit rebuilt it as
     `0 + reserved - reserved' -> 1 KiB` (`command/gomocup.cpp`); an unset limit now stays unset.
     No dedicated test (protocol plumbing).
  3. An explicit `MAX_MEMORY` larger than free RAM is still honoured as given; clamping it is
     not part of this item.
- Not done: scope item 3 (mid-search recycle).
