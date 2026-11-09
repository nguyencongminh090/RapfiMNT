# PERF-04 — MCTS retained slab pool after recycle

**Status:** phase 1 done (date 2026-11-09), awaiting user choice. Found in the PERF-03 phase 2 review.
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

## Phase 1 results (2026-11-09)

Data: `build/perf04/phase1_raw.md` (HEAD 2b1bfa0, gcc 15.2.0 Release, 1T unless stated, throttled
power state recorded; only node counts and bytes are used). Section numbers below refer to that file.
Numbers marked *derived* are computed from the cited measurements, not measured directly.

### Baselines

- Bench `17621937 / 8c1f72e5`. Golden (2M nodes, `MAX_MEMORY` 6e9, so the graph never closes)
  byte-identical to `build/perf03/golden_gcc_1.extract`. The instrumented copy gives identical search
  output on every game run (§1).
- Budget closure at 40 / 90 / 200 MB: 113554 / 258816 / 585725 table nodes, equal to PERF-03 (§4).

### What the gap is made of

- **13-move game**, n=16, after the last recycle (§2): gap 11.50 MB = fully empty slabs 3.77 MB
  (32.8 %) + free slots in partly-live slabs 5.93 MB (51.6 %) + bump slabs 1.75 MB (15 %). Survivors
  average 1.0 live node per 32-node slab; 55.3 % of slabs hold 1–2 live nodes. Empty-slab share of
  the gap: 0 % at n=5..9, 14 % at n=10, 27 % at n=13, 33 % at n=16.
- **Long game L1** (§3a, degenerate: the graph collapses to 1–3 nodes): empty slabs reach 14.90 MB
  = 89.4 % of the gap at collapse; 25.4 % / 35 % / 57.7 % at 11435 / 8482 / 3984 live nodes; 0–0.3 %
  while the graph holds ~50K nodes.
- **Long game L2** (§3b): 84.9 % at collapse (10.14 MB), 13.5 % at n=34 (2.41 MB), 0 % at other
  recycles of a large graph.
- **Pool size**: flat at its peak in L1 (9259 slabs, 16.67 MB); in L2 it grows with each new
  live-node peak, up to 17323 slabs (31.2 MB). Free storage is always reused before a new slab.

So survivors are scattered: releasing empty slabs gives back little after a partial recycle and
most of the gap after a near-total recycle.

### Does the real heap exceed the budget?

- Recycles near the soft limit (§5: 13-move game at 25 / 40 / 60 MB 1T and 40 MB 8T, L2 at 40 / 80 MB):
  the real graph heap never exceeds the graph limit (max 0.905 of it). It exceeds the soft limit by at
  most +1.0..+1.5 MB, the same offset as a fresh graph.
- **Constructed worst case** (§8, added in step 4): L2 line, 40 MB, 1T, MAX_NODE 1M. Search 1 is the
  n=40 position: it fills to soft with 191747 nodes (node share 45 %, *derived*). Search 2 is the
  n=4 position, without a clear. The jump back recycles all 191747 nodes. Search 2 then refills to
  soft with 90836 nodes (node share 21 %, *derived*), so it reuses only part of the free list.
  Result: **real graph heap 37.79 MB = 1.008 × graph limit (+0.29 MB), +5.91 MB over soft. It stays
  below MAX_MEMORY 40 MB (−2.21 MB).** At that recycle the fully empty slabs held 9.92 MB, more than
  the 5.91 MB overshoot.
- Reading: the overshoot is bounded by roughly 0.75 × (node share of fill 1 − node share of fill 2)
  × soft (*derived*, 56.25 of 75 B per node is slab storage). With node shares 21–47 % seen at closure
  in §5 / §8, that is about 1.01 × the graph limit. The VCF TT slice is outside the graph limit, so the
  result stays under MAX_MEMORY whenever that slice is above ~1 % of MAX_MEMORY. The slice is capped
  by `mctsVCFTTMaxSizeKB`, so at very large budgets the margin shrinks (*derived*, not measured).

### Candidates

Notation: `H` = real NodeTable heap (slot arrays + slabs, `heapBytes()`). `G` = the node part of
`graphMemoryUsed` (edge arrays are counted the same way under every candidate and drop out). `N` =
live nodes. `F` = free-list nodes. `C0` = the fresh-graph offset (bump-slab slack, at most 31 × 56 B
per shard, plus slot-array load variation). Measured `C0` is about +0.5..+1.5 MB at 1024 shards.

| | (a) free empty slabs | (b1) keep free storage charged | (b2) exact table accounting | (c) hybrid | close |
|---|---|---|---|---|---|
| Reclaim (13-move game) | 3.77 MB of 11.50 at n=16; 0 at n=5..9 | 0 | 0 | as (a) above the threshold | 0 |
| Reclaim (near-total recycle) | 14.90 MB (L1), 10.14 MB (L2), 9.92 MB (§8) | 0 | 0 | as (a) | 0 |
| Invariant | `H ≤ G + C0 + 31·56 B · N_s`, where `N_s` = survivors at the last recycle (every kept slab has ≥ 1 live node); no bound relative to `G` alone | `H ≤ G + C0` (`G` = 75 B × (N + F) over-counts free slots by 19 B) | `H = G` exactly | as (b) | today: `H ≤ G + C0 + 56 B · F` |
| Golden (unbounded) | unchanged (no closure; nothing orders by `Node *`, to confirm in phase 2) | unchanged | unchanged | unchanged | unchanged |
| Budget runs, explicit MAX_MEMORY | **`G` untouched, so closure and search output are identical to today** (40/90/200 MB counts stay) | first fill unchanged; after a recycle the pool stays charged at its peak, so later searches get about soft − pool for edges (L2 at 40 MB n=40: table heap 15.60 MB of soft 31.87 MB) | closure counts move even before any recycle (the 75 B average is replaced by real step allocations; slot-array doubling makes `G` jump) | as (b) | unchanged |
| `deriveDefaultGraphBudget` | `G` unchanged; MemAvailable may rise only if glibc returns pages | counts the pool as owned memory (more accurate than today, which under-counts by the gap) → slightly larger default budget | same as (b1), exact | as (b) | unchanged |
| Risk | stale `Node *` becomes a use-after-free (audit below); free list must be rebuilt; bump slab handling; slab churn on collapse → regrow (L2 n=17..26) | no UAF; behaviour change on every budget run after a recycle; needs to know whether storage came from the free list (`tryEmplaceNode` does not say; needs a table-side counter, public API kept) | no UAF; `rehash` / `rebuildAfterErase` / `releaseAll` must return deltas; `NodeTableEntryOverhead` leaves the table path; largest behaviour change | sum of (a) and (b) | known `C0 + 56 B · F` overshoot stays; measured max 1.008 × graph limit |
| Test cost | ~6 new `NodeTable` tests (release, partly-live kept, survivors keep addresses, no free entry in a released slab, bump slab, heap follows survivors), rewrite of `eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab` (its first 100 nodes fill 3 whole slabs, which (a) releases), TSan/ASan on recycles, A/B recycle sequence byte-identical **including budget runs** | accounting tests + budget tests; self-play of the budget-limited change (hard rule 9) | as (b1) + closure re-measurement | both | none (doc note) |

Notes on (a):
- **Reclaim lands in the right place.** The overshoot needs a large recycle after a node-heavy
  fill, and in that case the empty slabs dominate: 9.92 MB empty at the §8 recycle against 5.91 MB
  over soft. With (a), search 2 in §8 would allocate new slabs for its 90836 nodes, as on a fresh
  graph, so its heap would be close to `G + C0`. This is *counted, not measured*. A partial recycle
  (13-move game) gives back at most a third, but then `G` is far below soft, so the gap is harmless.
- **`uordblks` vs RSS.** Slabs are 1800 B chunks from the main glibc arena. Freeing them lowers the
  allocator's in-use bytes, which is the "real heap" metric here, and lets edge arrays reuse that
  memory. RSS probably does not drop without `malloc_trim`. VmHWM never drops in any current run
  either.
- **Node → slab mapping.** Sorting the slab addresses and binary-searching each survivor is already
  proven in the instrumented copy (`slabStats`, consistency-checked on every recycle). It runs
  between searches over 6.5K–17K slabs. Aligned slabs would waste ~12 % (1800 B → 2048 B).
- **Churn.** L2 collapses to 27 nodes and regrows to 106812 nodes two moves later. That costs about
  5.6K `delete` + `new` of 1800 B per collapse–regrow cycle (*derived*, slab counts in §3b). Measure
  recycle time and cycles per visit in phase 2. Add a small per-shard reserve only if it shows up
  (1024 shards × reserve).

### Initial stale-`Node *` holder audit for (a) (full audit in phase 2)

1. `graph.root`: `setupRootNode` reassigns it (`allocateOrFindNode`) before `recycleOldNodes`, and
   marking starts from it. `clear(false)` sets it to null.
2. Survivors' edge `childWord`s: `recursiveApply` follows every non-null child of every reachable
   non-leaf node. The age `exchange` runs after the children, so a node seen as marked has its whole
   subtree marked. Every child of a survivor is a survivor.
3. `RetiredEdgeArrays` (old arrays still hold child pointers): `searchMain` frees them after
   `runOnAllThreads` returns, before the next `setupRootNode`, so the list is empty at recycle.
   Phase 2 should assert `retiredEdgeArrays.count() == 0` on recycle entry.
4. `createEdgesFrom` copies children from the old array during a search only (`node.cpp` ~345).
5. `selectedPath` is local to `search()`. `th.rootMoves` hold `Pos` / values / PV moves, not `Node *`.
6. No `Node *` holder in engine code outside `search/mcts/`. Grep run over `*.cpp`/`*.h` (excluding
   `build/`, `external/`, `search/mcts/`) for `MCTS::Node|mcts/node.h|mcts/searcher.h|graph\.root|nodeTable`:
   the only engine hit is `search/searchengine.cpp` including `searcher.h` (it owns the searcher). The
   other hits are tests. Inside `search/mcts/`, `Node *` lives only in `graph.root`, edge child words,
   and locals of `search.cpp` / `node.cpp`.
7. The erase predicate in `recycleOldNodes` reads only the node itself (age, `numEdges`). Shards are
   erased in parallel, each under its own lock, so a release inside `eraseNodesIf` after the sweep
   does not race with another shard's predicate.
8. Tests: `mcts_visits_tests.cpp` 523–572 hold `root` / `child` across two searches of the same
   position. There is no recycle in between (same root, not over budget), and `child` is reachable
   anyway. `mcts_vcn_tests.cpp` 315 (`vcnRoot`) and 1405 (`root`) dereference the pointer only
   before the next search (the test at 315 already compares the later root by key). The `NodeTable`
   tests that hold `Node *` vectors (lines ~160, 184, 226, 295, 345) are to
   be checked in phase 2. Line 345 is known to conflict (see test cost).

### Recommendation

**(a) free fully empty slabs after recycle, class B.**

The trade, plainly: the instruction's test for "close" is that the real heap stays within the
configured budget in practice. Every natural run meets it (max 0.905 × the graph limit). Only the
constructed case crosses the graph limit, by 0.8 %, and it stays under MAX_MEMORY. Choosing (a)
means taking on code with use-after-free risk to remove that one constructed 0.8 % overshoot and to
make the heap follow the graph after a collapse.

- It is the only candidate that removes the measured overshoot (§8: 1.008 × the graph limit, after a
  near-total recycle) without changing `graphMemoryUsed`. Closure points and search output on every
  budget-limited run with an explicit MAX_MEMORY therefore stay byte-identical. That is a stronger
  check than (b) allows. Self-play is not needed, on one condition: the A/B recycle sequence on
  budget runs (explicit MAX_MEMORY) must come out byte-identical. Any diff brings back the self-play
  of hard rule 9.
- (b1) / (b2) give an honest invariant (`H ≤ G + C0` / `H = G`) but return no memory. They change
  output on every budget run after the first recycle ((b2) even before), and they charge the
  historic peak pool against later searches. That is a class-B behaviour change that needs self-play,
  for a problem measured at 0.8 % of the graph limit. (c) carries the (b) cost.
- (a) gives no gmu-relative invariant in general. Partly-live slabs remain (13-move game: 5.93 MB),
  and the bound is `H ≤ G + C0 + 31·56 B · N_s`. State that in the code comment and not a stronger one.
- Class B, not A: no encoding, position identity or compile-time table changes. Not C: it changes
  memory behaviour on budget runs, and a slip is a use-after-free, so hard rule 9 measurements apply
  (gap curve, recycle sequence A/B, cycles per visit).
- **Alternative if the user prefers no code:** close as not worth it. The overshoot stays below
  MAX_MEMORY in every measured run, including the constructed worst case. Document the bound
  `H ≤ G + C0 + 56 B · F` and the measured max of 1.008 × the graph limit next to `graphMemoryUsed`.
  That is a class-C doc change.
- Knowledge lookup (hard rule 10): both SKILLS_TREE queries ("slab allocator reclaim empty slabs
  fragmentation", "memory budget accounting allocator retained memory") returned **none matched**
  (§6). This recommendation rests on the measurements above, not on a note.

### Where the data is thin

- The worst case (§8) is one constructed sample: 1T, one position pair, 40 MB. Its scaling to large
  budgets (TT slice capped) is derived, not measured.
- The 8T budget run is a single non-deterministic sample.
- L1 is degenerate (forced endgame, graph collapses to 1–3 nodes), which inflates its 89 %. L2 is the
  more representative long game.
- The effect of (a) in §8 is counted from empty-slab bytes, not measured with a release.
- RSS effect of (a) is unknown (glibc may keep freed chunks).

### Optional extra measurement before deciding (cheap, not required)

Repeat §8 at 200 MB, and once at 8T 40 MB. Use the same command with `MEM=200000000`, **MAX_NODE ≥
3000000** (closure at 200 MB took 2.37M root visits, §4), and a matching `MAX_MEMORY` 1-node
baseline. **The run is valid only if search 1 shows `gmu / soft = 1.00`.** Otherwise the backward
jump does not recycle, because a recycle on a backward move needs `overBudget`. This checks that the
1.008 ratio does not grow with budget size or threads. Each run is two searches (minutes).
