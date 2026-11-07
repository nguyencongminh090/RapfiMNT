# PERF-03 — MCTS node table footprint

**Status:** open (recorded 2026-11-07, not started).
**Class:** B or C, depending on the final structure (representation change inside `NodeTable`).

## Problem

`NodeTable` is `std::set<Node>` per shard: one allocation per node, with red-black-tree bookkeeping
(`NodeTableEntryOverhead = 48` bytes estimated, not measured) and lock contention on shared mutexes.
After SEARCH-02 the leaf nodes become a larger share of memory.

## Scope

- Measure the real bytes per node (allocator + tree), not the estimate.
- Candidate: open-addressing table or arena storage keeping pointer stability (`Node` is neither
  copyable nor movable).

## Test list

To be written before code: lookup / insert / race on the same hash, pointer stability, clear and
recycle, counter consistency (`graphMemoryUsed`).

## Measure

Peak RSS and nodes per second on the SEARCH-02 fixed-node run.
