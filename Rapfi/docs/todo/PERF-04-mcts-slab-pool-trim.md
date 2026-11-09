# PERF-04 — MCTS retained slab pool after recycle

**Status:** open (recorded 2026-11-09, not started). Found in the PERF-03 phase 2 review.
**Class:** B (changes memory behaviour on budget-limited runs; search output on unbounded runs must
stay byte-identical).

## Problem

Since PERF-03, each `NodeTable` shard stores nodes in fixed slabs of 32 nodes (`search/mcts/nodetable.h`).
`eraseNodesIf` (called from `recycleOldNodes`, `search/mcts/search.cpp`) destroys unreachable nodes
and puts their storage on the shard's free list. Slabs are released only by `clearShard` /
the destructor, i.e. on `clear(true)`.

`graphMemoryUsed` subtracts the footprint of every recycled node, so after a recycle it reads lower
than the real heap. Measured in PERF-03 verification (`build/perf03/phase2/verify_raw.md`, check 8):
13-move game, ~500K visits per move, the gap (table heap − nodes × 75 B) grew to +11.4 MB while the
live graph shrank to 7108 nodes; 89 % of slab capacity was on free lists. The pool stays at its
peak (6560 slabs, ~11.8 MB) and does not grow further, because free storage is reused first.

Bound (from the PERF-03 review): real heap ≤ `graphMemoryUsed` + Σ shards (peak nodes − live
nodes) × 56 B + up to 31 nodes of slab slack per shard. Worst case: a node-heavy search fills the
budget, then an edge-heavy search overshoots by roughly the earlier node share (~1/3 of the budget).
`graphMemoryUsed` was never a hard process cap, and the old `std::set` also kept freed memory in the
allocator, so PERF-03 was merged with this open.

## Scope

Candidates, to be measured before choosing:

- **(a) Free fully empty slabs after recycle.** Needs a per-slab live count (or a sweep of the free
  list grouped by slab) and removing that slab's entries from the free list. Real heap then follows
  live nodes, at the cost of slab churn.
- **(b) Account the free list.** Count free-list bytes in `graphMemoryUsed` (or a separate counter
  that `graphClosed()` adds). The budget becomes honest; memory is not returned to the OS.
- **(c) Hybrid:** (b) always, plus (a) when the free share of a shard exceeds a threshold.

Out of scope: the edge-array path and `EdgeArrayAllocOverhead` (separate item), Node/Edge layout,
defaults, the shard-count compare in `MCTSSearcher::clear`.

## Test list

To be written before code: recycle frees an empty slab and keeps live nodes at their addresses;
partially used slabs are kept; free-list entries of a freed slab are gone (no reuse of freed
memory); accounting identity after insert / recycle / clear under the chosen rule; `graphClosed()`
fires at the same real heap level before and after a recycle.

## Measure

- Rerun PERF-03 check 8 (13-move game): gap (heap − accounted) per move, before / after.
- Peak RSS on the 2M-node golden setup and golden output (must stay byte-identical when unbounded).
- Cycles per visit A/B (PGO, interleaved), 1 and 8 threads.
- Budget-limited closure: nodes / visits at closure, before / after.
