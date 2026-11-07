# SEARCH-03 — MCTS default graph memory budget

**Status:** open (recorded 2026-11-07, not started).
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

## Test list

To be written before code (`docs/todo` rule): budget derivation from a given RAM figure, the
"stop inserting" threshold, and counter consistency after a recycle sweep.

## Measure

Peak RSS on a long search (analysis mode, no time limit) and self-play against the current build at
equal time. See the SEARCH-02 table for the harness (`build/oppo4-ab/run.sh`).
