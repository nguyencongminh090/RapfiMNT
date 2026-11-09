# PERF-04 — MCTS retained slab pool after recycle

**Status:** phase 2: core done, verification runs pending (phase 1 done 2026-11-09). User choice 2026-11-09: (a). Found in the PERF-03 phase 2 review.
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

Phase 2 list for (a): see "Phase 2 — candidate (a)" below. Phase 1 outline, before the choice: recycle frees an empty slab and keeps live nodes at their addresses;
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

## Phase 2 — candidate (a): free fully empty slabs after recycle

**User choice 2026-11-09: (a)**, class B. `graphMemoryUsed` and `nodeFootprint()` stay unchanged.

### Decision note (class B, hard rule 9)

- **What changes:** after an erase, `NodeTable` frees every slab of the shard that holds no live
  node, including the bump slab, and drops those slabs' entries from the free list. Nothing else
  changes: the public API, slab size and alignment, the slot arrays, `graphMemoryUsed`.
- **Expected effect:** real table heap follows the survivors after a recycle. The invariant is
  "every slab holds at least one live node", so `H ≤ G + C0 + 31·56 B · N_s` (phase 1 table).
  Search output does not change on unbounded runs or on runs with an explicit MAX_MEMORY,
  because `graphMemoryUsed` is untouched and nothing orders by `Node *` (audit item 9).
  Default-budget runs (no MAX_MEMORY) depend on MemAvailable, as before (next point).
- **`deriveDefaultGraphBudget`:** its `graphUsedBytes` input is `G`, which is unchanged. The
  default budget can move only through `availablePhysicalMemoryKB()`. It rises if glibc
  returns the freed 1800 B chunks to the OS, and stays the same if they stay in the arena (likely
  without `malloc_trim`). Before (a), the retained pool counted neither as available nor as owned,
  so the default budget was low by up to the gap. (a) can only reduce that error, never make the
  budget count memory twice.
- **How it is measured:** unit tests below; bench `17621937 / 8c1f72e5`; golden byte-identical; ASan
  on the suite. Then, by the verification agent: the A/B recycle sequence byte-identical against
  the 1b6370a binary (unbounded and budget-limited), the gap curve, peak RSS, budget closure, cycles
  per visit, TSan.
- **No reserve, no threshold.** An empty bump slab is freed as well, so a shard with no live node
  owns no memory. Churn (one `new` per 32 regrown nodes) happens between searches for the frees and
  during the search for the `new`s, as on a fresh graph. A per-shard reserve costs up to
  1024 × 1800 B and would weaken the invariant. Add one only if the verification shows a cost in
  recycle time or cycles per visit.

### Use-after-free audit (complete, written before any slab is released)

A slab is freed only inside `eraseNodesIf` (called from `recycleOldNodes`) or in
`releaseAll` (`clear(true)`, destructor; unchanged). So the question is which `Node *` can be
dereferenced after `recycleOldNodes` and points to an erased node.

1. **`graph.root`** — `setupRootNode` reassigns it by `allocateOrFindNode` before calling
   `recycleOldNodes`, and the marking starts from it, so the root is always marked.
   `clear(false)` sets it to null. `clear(true)` frees everything and nulls it too.
2. **Edge `childWord`s of survivors** — `recursiveApply` (search.cpp ~1222) follows every edge with
   a non-null child of every non-leaf node it visits, and it sets the node's age only after all
   children have returned. A child returns early only if its own age was already set, which happens
   only after its own children were done. The graph is acyclic (each child has one more move), so by
   induction a marked node has a marked subtree. `runOnAllThreads(..., true)` waits for every
   marking thread before the erase starts. So every child of a survivor is a survivor.
   Edges of erased nodes go away with them (`~Node` frees the edge array).
3. **`RetiredEdgeArrays`** (old arrays still hold child words) — `searchMain` calls
   `freeRetiredEdgeArrays()` after `runOnAllThreads(search)` returns. `clear(true)` also frees
   them. The next recycle runs in the next `setupRootNode`, so the list is empty at recycle.
   Phase 2 adds `assert(retiredEdgeArrays.count() == 0)` at the entry of `recycleOldNodes`
   (compiled out in release).
4. **`createEdgesFrom`** copies child words from an old array during a search only.
5. **Root-move data** — `th.rootMoves` hold `Pos`, values and PV moves, no `Node *`.
   `updateRootMovesData` reads `graph.root` after the search, before any recycle.
6. **`scratch` (SearchScratch)** holds counters, a time and an int, no `Node *`.
   **`GraphState`** holds only `root` (item 1), plus a move list, a key and the age.
   `MCTSSearcher::makeSearchData` returns null, so there is no per-thread MCTS data.
   `selectedPath` is local to `search()`.
7. **Outside `search/mcts/`** — no engine `Node *` holder (phase 1 grep, item 6 of the initial
   audit). There is no `static` or `thread_local` `Node *` in `search/mcts/*.cpp` (grep).
8. **Erase predicate and shard parallelism** — the predicate in `recycleOldNodes` reads only the
   node itself. Each shard is erased under its own unique lock, and the release touches only that
   shard's slabs. A slab holds nodes of one shard only, so no other shard's predicate can read a
   freed slab.
9. **Nothing orders by `Node *`** — freed slabs change the addresses later nodes get, so this is
   what byte-identical output rests on. A grep of `search/mcts/` for `sort`, `std::set`, `std::map`,
   `less<`, `unordered_`, `hash<` and pointer-to-integer casts finds only policy / root-move
   sorts by value (search.cpp 523, 1682) and the child-word packing in `node.h`. The slot position
   depends on the hash, not the address. `rehash` walks the old slots by index.
10. **Tests holding `Node *`:**
    - `mcts_visits_tests.cpp` 523–572 hold `root` / `child` across two searches of the same
      position. `setupRootNode` returns early (same position and key, unbounded), so no recycle runs.
      The pointers are reachable anyway.
    - `mcts_vcn_tests.cpp` 315 (`vcnRoot`): the next ordinary search has a new root key and recycles,
      so `vcnRoot` may be erased and its slab freed. The test does not dereference `vcnRoot` after
      that search; it compares the new root by key.
    - `mcts_vcn_tests.cpp` 1405 (`root`): dereferenced right after the search, before the next
      `clear(true)`.
    - `mcts_bound_tests` / `mcts_widen_tests` build their nodes outside the table.
    - `mcts_nodetable_tests.cpp`: the pointer vectors in the growth and thread tests see no erase.
      `eraseNodesIf_staleAge_...` dereferences only survivors. `eraseNodesIf_thenInsert_...`
      compares freed addresses with new ones. Under (a) its first 100 nodes fill whole slabs that are
      freed, so it is rewritten (below).

**Conclusion:** no `Node *` that can be dereferenced after a recycle points to an erased node, so
freeing empty slabs creates no use-after-free in engine code. As a test aid, freed node storage is
poisoned under ASan (bytes 8..55; the first 8 hold the free-list link) and unpoisoned on reuse. A
stale read of a recycled node inside a kept slab is then reported too, which was invisible before.

### Test list (written before the code)

`tests/mcts_nodetable_tests.cpp`. Slab layout is controlled through the fresh single-shard
order: on `NodeTable(0)` with no erase, insert *i* lands in slab *i*/32, slot *i*%32. Labels:
**red** = fails before the change; **guard** = passes before and after (pins behaviour that must
not change).

| Test | Checks | Label |
|---|---|---|
| `eraseNodesIf_allNodesOfSlabErased_slabReleased` | 3 full slabs, erase the 32 nodes of the middle one: `numSlabs` 3 → 2, `heapBytes` drops by `SlabBytes`, `numFreeNodes == 0` | red |
| `eraseNodesIf_slabPartlyLive_slabKept` | erase 31 of the middle slab's 32: `numSlabs == 3`, `numFreeNodes == 31` | guard |
| `eraseNodesIf_slabReleased_survivorsKeepAddresses` | 4 full slabs; erase slabs 1 and 3 whole and the even nodes of slab 0: every survivor is found at its old address with its hash; erased hashes are gone | guard |
| `eraseNodesIf_slabReleased_freeListHoldsOnlyKeptSlabSlots` | 3 full slabs; erase slab 1 whole and the first 8 nodes of slabs 0 and 2: `numFreeNodes == 16`; draining the free list (16 inserts, no new slab) returns exactly the 16 freed addresses of kept slabs, never one in the freed slab | red |
| `eraseNodesIf_bumpSlabEmptied_slabReleased` | 32 + 10 nodes (bump slab at `slabUsed` 10), erase the 10: `numSlabs == 1`, `numFreeNodes == 0` | red |
| `eraseNodesIf_bumpSlabReleased_nextInsertsStartNewSlabAndKeepSurvivors` | same arrangement, then 33 inserts: 3 slabs, no free node, no new node inside slab 0, every survivor's hash intact and found. Catches a stale `slabUsed` after the bump slab is freed, which would place new nodes over live ones in slab 0. It passes before the change because there the bump slab is kept | guard |
| `eraseNodesIf_bumpSlabPartlyLive_keptAndBumpContinues` | 32 + 10 nodes, erase 5 of the bump slab's: `numSlabs == 2`, `numFreeNodes == 5`; 5 inserts reuse the freed addresses, 22 more fill the bump slab with no new slab, the next one adds a slab | guard |
| `eraseNodesIf_allNodesErased_shardOwnsNoMemory` | 100 nodes, erase all: `numSlabs == 0`, `numFreeNodes == 0`, `slotCapacity == 0`, `heapBytes() == 0`; then an insert works and owns one slab | red |
| `eraseNodesIf_partlyFreedSlabsThenInsert_reusesFreedStorageWithoutNewSlab` | rewrite of `eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab` (see below) | guard |
| `recycleOldNodes_afterMoveForward_noShardKeepsSlabWithoutLiveNode` | engine, 1024 shards: search, move forward two moves, search (recycle): every shard has `numSlabs <= numNodes` (each kept slab holds a live node), and `graphMemoryUsed` equals the live footprint (accounting identity unchanged) | red |
| `recycleOldNodes_budgetRunAfterSlabRelease_graphClosedFollowsLiveFootprint` | engine, explicit memory limit small enough to close the graph in search 1; move forward, search 2 recycles: `graphMemoryUsed` equals the live footprint, `graphClosed()` equals `graphInsertionClosed(true, liveFootprint, limit)`, every shard `numSlabs <= numNodes`. The memory limit is reset to 0 before the fixture ends (the fixture copies it into the next searcher) | red (slab bound) |

The existing `recycleOldNodes_afterMoveForward_graphMemoryUsedEqualsLiveFootprints`,
`heapBytes_afterInserts_equalsSlotsPlusSlabs` and `clearShard_nonEmpty_...` stay as guards for
the accounting identity and `releaseAll`.

**Rewrite of `eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab`.** It erases the first 100
of 400 inserts. Those fill slabs 0–2 whole, which (a) frees, so only 4 free slots stay and the
100 new inserts would need new slabs. The test's purpose is "freed storage is reused before a new
slab". It is kept by erasing `i % 4 == 0` among the 400 inserts instead: every slab keeps 24 live
nodes (the bump slab, `slabUsed` 16, keeps 12), so nothing is freed and all original asserts
(100 free before, 0 after, same slab count, every new node at a freed address) still hold. It is
renamed to `eraseNodesIf_partlyFreedSlabsThenInsert_reusesFreedStorageWithoutNewSlab` to name the
scenario.

**Failing-first method.** `RUN_TEST` aborts at the first failed assert, so each red test is run
alone against the unchanged `nodetable.h` (temporary local filter, not committed). Output goes to
`build/perf04/phase2/tests_failing_first.txt`.

**Failing first (2026-11-09, recorded before any change to `nodetable.h`).** Built in `build/`
(gcc 15.2.0 Release, `rapfi_tests` with `-UNDEBUG`; `--check-asserts-live` aborts as it should).
Each test was run alone through a temporary local `RT_ONLY` filter in `tests/testutil.h`, which was
reverted and not committed. Record: `build/perf04/phase2/tests_failing_first.txt`.

| Test | Before the change |
|---|---|
| `eraseNodesIf_allNodesOfSlabErased_slabReleased` | **fails**: `s.numSlabs == 2` |
| `eraseNodesIf_slabReleased_freeListHoldsOnlyKeptSlabSlots` | **fails**: `afterErase.numFreeNodes == 16` |
| `eraseNodesIf_bumpSlabEmptied_slabReleased` | **fails**: `s.numSlabs == 1` |
| `eraseNodesIf_allNodesErased_shardOwnsNoMemory` | **fails**: `s.numSlabs == 0` |
| `recycleOldNodes_afterMoveForward_noShardKeepsSlabWithoutLiveNode` | **fails**: `noShardHasMoreSlabsThanNodes` |
| `recycleOldNodes_budgetRunAfterSlabRelease_graphClosedFollowsLiveFootprint` | **fails** on `slabBound`. Its earlier asserts pass: graph closed after search 1, `graphMemoryUsed == live footprint`, `graphClosed()` matches, so the setup is valid |
| `eraseNodesIf_slabPartlyLive_slabKept`, `..._survivorsKeepAddresses`, `..._nextInsertsStartNewSlabAndKeepSurvivors`, `..._bumpSlabPartlyLive_keptAndBumpContinues`, `eraseNodesIf_partlyFreedSlabsThenInsert_...` | pass (guards, as labelled) |

### Core change (2026-11-09, commits 281719c `test:`, 428956b `perf:`)

Everything is in `search/mcts/nodetable.h`, plus one assert in `search.cpp`.

- **Where:** `rebuildAfterErase` first calls `releaseEmptySlabs`. It runs only when
  `eraseNodesIf` erased something, under the shard's unique lock. `releaseAll` shares the
  slab-freeing helper `releaseAllSlabs`. The public API is unchanged.
- **Node → slab:** the shard's slab pointers are sorted (`std::less`), and each survivor is
  binary-searched (`upper_bound − 1`, with an assert that it lies in that slab's storage). Slab size
  (1800 B) and alignment are unchanged.
- **Fast paths:**
  - A shard with no live node frees all its slabs and resets the pool.
  - If `numFree + bump unused < 32`, no slab can be empty, and the mapping is skipped.
  - In release builds, nothing more is done when no slab is empty.
- **Free list:** walked once; entries in empty slabs are unlinked in place, so the kept entries keep
  their LIFO order. When no slab is freed, the pool is unchanged.
  - Debug builds also check, for every slab, that live + free equals the slots handed out (32, or
    `slabUsed` for the head).
- **Bump slab:** freed like any other slab when it holds no live node. If the head was freed,
  `slabUsed = SlabNodes`, so the next allocation takes the free list and then a new slab. A kept
  head keeps its `slabUsed`.
- **Invariant:** every slab holds at least one live node at all times. A new slab is created only by
  an allocation that puts a node in it, and nodes leave only through `eraseNodesIf`. So the
  table heap ≤ slot arrays + one slab per live node, i.e. `H ≤ G + C0 + 31·56 B · N_s`, stated in
  the `NodeTable` class comment. No stronger bound is claimed.
- **No reserve or threshold** (decision note above). Churn is left to the verification
  measurements.
- **ASan test aid:**
  - `freeNodeStorage` poisons bytes 8..55 of dead storage; the first 8 hold the free-list link.
  - `allocateNodeStorage` unpoisons it on reuse.
  - Slabs are unpoisoned before `delete`.
  - Active under gcc `__SANITIZE_ADDRESS__` or clang `__has_feature(address_sanitizer)`. Both
    binaries of the ASan build link `__asan_poison_memory_region`.
- **`recycleOldNodes`** asserts `retiredEdgeArrays.count() == 0` on entry (audit item 3).
- **Not touched:** `graphMemoryUsed`, `nodeFootprint()`, Node/Edge layout, the edge-array path,
  the slot-array design, search defaults.

### Core verification (run by the implementer, 2026-11-09)

Data under `build/perf04/phase2/`.

| Check | Result | Command / file |
|---|---|---|
| Unit tests, asserts live | **238/238 pass**; `--check-asserts-live` aborts as it should | `build/` (gcc 15.2.0 Release, `-UNDEBUG` on `rapfi_tests`): `ninja rapfi_tests && ./rapfi_tests` → `tests_after_impl.txt` |
| New tests failed first | 6 red tests fail before the change, 5 guards pass | `tests_failing_first.txt` (table above) |
| Bench | **Nodes 17621937, Hash 8c1f72e5** | separate gcc 15.2.0 Release build `rel/` (`cmake -S Rapfi -B build/perf04/phase2/rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DNO_COMMAND_MODULES=ON -DBUILD_TESTS=OFF`, AVX512+VNNI on), `echo BENCH \| rel/pbrain-rapfi` → `bench_rel.txt` |
| Golden (2M nodes, 1T, SHOW_DETAIL 2, cfg_golden, unbounded) | **byte-identical** to `build/perf03/golden_gcc_1.extract` (11103 lines, md5 f3e620af…); peak RSS 173468 KB | `python3 build/search08/run_fixed.py build/perf04/phase2/rel/pbrain-rapfi … nodes=2000000 threads=1 detail=2 cfg=cfg_golden` + `golden_extract.sh` → `golden_rel_1.{txt,extract}` |
| ASan + UBSan (`-fno-sanitize=enum`) on the suite | **238/238 pass, stderr empty** (no ASan or UBSan report), with poisoning active | `build_asan.sh` (pattern of `build/perf03/phase2/build_san.sh`) → `asan/`; `asan_tests.{txt,stderr}` |

The golden run is a single search, so it does not exercise a recycle. Recycle byte-identity is
the A/B sequence below. **The release (NDEBUG) path of `releaseEmptySlabs` has not run yet.**
`rapfi_tests` and the ASan binaries have asserts live, so every recycle so far took the debug
path, which always walks the free list and checks it. The release-only early return ("no empty
slab") and the walk without counters run for the first time in the A/B recycle sequence on
`rel/pbrain-rapfi`.

Placement: the engine-level tests are in `mcts_nodetable_tests.cpp`, not `mcts_memory_tests.cpp`,
because only the former has an engine fixture. The `instruction.md` index line is unchanged.

### Pending (verification agent, scripted)

- TSan on the thread tests (`build/perf03/phase2/nt_loop.cpp`, `tsan.supp`).
- ASan on the real game sequence with recycles and `YXHASHCLEAR`: engine binary
  `build/perf04/phase2/asan/pbrain-rapfi`.
- PERF-03 A/B recycle sequence byte-identical, unbounded and budget-limited (explicit MAX_MEMORY).
  - New binary: `build/perf04/phase2/rel/pbrain-rapfi`.
  - Baseline: `build/perf04/base_build/pbrain-rapfi`. It was built at 2b1bfa0; the engine
    sources are identical at 1b6370a (`git diff 2b1bfa0 1b6370a` touches only `.md`). The PERF-03
    `pbrain-B-gcc` is the same source line.
  - Any diff brings back the self-play of hard rule 9.
- Gap curve after the change (13-move game, L1, L2, §8 worst case), peak RSS, budget closure at
  40 / 90 / 200 MB (counts must equal phase 1), cycles per visit A/B (PGO, interleaved, power
  state recorded), recycle time vs churn, PGO release rebuild.
