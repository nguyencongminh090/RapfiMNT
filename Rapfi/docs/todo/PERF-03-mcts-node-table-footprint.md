# PERF-03 — MCTS node table footprint

**Status:** phase 1 done 2026-11-09 (measurements and candidate ranking below); phase 2 waits for
the user's choice of structure. Recorded 2026-11-07.
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

## Phase 1 results (2026-11-09)

HEAD `25ae75c`. Raw data and method: `Rapfi/build/perf03/phase1_raw.md` and the files next to it
(not in the repo). Units: MB = 10^6 bytes everywhere below. Peak RSS from `time -v` is in KiB, so
184100 KiB = 188.5 MB (179.8 MiB). The CPU was power-limited during all runs (PL1 24 W,
`build/perf03/power_state.txt`), so NPS is only usable as a relative number.

Test setup: Standard 15x15, moves `7,7;9,8;6,7;7,9`, `build/search08/cfg`
(`mcts_edge_policy_mass = 0.99`, expand_min_visits 1), `MAX_NODE 2000000`, 1 thread,
`INFO MAX_MEMORY 6000000000` (the graph never closes). Harness: `build/search08/run_fixed.py`.

### Measurement summary

| Quantity | Value | Compiler / build | Source (`build/perf03/`) |
|---|---|---|---|
| BENCH | Nodes `17621937`, Hash `8c1f72e5` (unchanged) | gcc 15.2.0 Release; clang 21.1.8 PGO | `bench_gcc.txt`, `bench_pgo.txt` |
| Golden MCTS (2M visits, 1T, SHOW_DETAIL 2) | deterministic: gcc run1 == run2 == PGO run; this is the phase-2 reference (it differs from `build/search08/golden_base.extract` because HEAD has moved on) | gcc x2, PGO x1 | `golden_gcc_{1,2}.extract`, `golden_pgo_1.extract`, `golden_root_moves_final.txt` |
| Peak RSS, 2M visits, 1T | 184104 / 184096 KiB (gcc), 183796 / 184028 KiB (PGO), i.e. 188.5 MB | gcc, PGO | `rss_head-{gcc,pgo}_n2000000_{1,2}.txt` |
| Peak RSS, MAX_NODE 1 (baseline) | 35776 / 35860 KiB (gcc), 36240 / 36132 KiB (PGO) | gcc, PGO | `rss_head-*_n1_*.txt` |
| Table nodes at the end of the 2M run | 511683 (all reachable, no recycle ran); 262077 with edges, 249606 leaves (49 %) | gcc, instrumented throwaway copy | `rss_instr_gcc_n2000000_*.txt` (line `PERF03STATS`) |
| Edges | 6079250 in 262077 arrays | same | same |
| `graphMemoryUsed` | 156772880 B = 511683 x 104 + 103557848 (`edgeArrayFootprint` sum), exact | same | same |
| Heap in use (mallinfo2 `uordblks`), 2M minus MAX_NODE 1 | 166322096 - 15732432 = 150589664 B (second pair 150582480) | same | same |
| Real bytes per table node, allocator probe | `std::set` node = 32 B rb header + 56 B Node = 88 B requested, usable 88, glibc chunk **96 B**; 1M inserts measure 96.000 B/node | g++ 15.2 and clang++ 21, glibc 2.43 | `probe_gcc.txt`, `probe_clang.txt`, `probe_set_node.cpp` |
| Real bytes per table node, in process | (150589664 - 101461232 real edge chunks) / 511683 = **96.01 B** | gcc instrumented | `phase1_raw.md` section 2B |
| Real bytes per table node, from RSS | 98.5 B (gcc), 97.4 B (PGO); includes page and arena slack | gcc, PGO | `phase1_raw.md` section 2C |
| Real edge-array chunk | `new[]` of 8 + 16n B gives chunk 16n + 16; the estimate is 16n + 24 | glibc 2.43 | `phase1_raw.md` section 2B |
| perf, `NodeTable::findNode` self (tree walk inlined) | 4.87 % (1T), 4.20 % (8T) | clang 21 PGO, `cycles:u` | `report_flat_{1,8}t.txt` |
| perf, `allocateOrFindNode` inclusive (find + emplace + lock + node alloc) | 6.48 % (1T), 5.29 % (8T) | same, dwarf call graph | `full_children_{1,8}t.txt` |
| perf, rwlock self (rdlock + unlock + wrlock) | 0.56 % (1T), 0.39 % (8T) | same | `full_flat_{1,8}t.txt` |
| perf, `_Rb_tree_insert_and_rebalance` + `_Rb_tree_decrement` | 0.47 % (1T), 0.46 % (8T) | same | same |
| perf, malloc side (malloc, `_int_malloc`, operator new) self | 0.23 % (1T), 0.29 % (8T); operator new inclusive 0.31 / 0.34 % (includes edge arrays) | same | same |
| perf, `Accumulator::move` self | 40.5 % (1T), 41.6 % (8T) | same | `report_flat_{1,8}t.txt` |
| Contention check, 8T 5 s | 706 % CPU, 0.28 s sys of 34 s, 688 voluntary context switches | PGO | `timev_pgo_8t.txt` |
| MCTS NPS, 5 x 5 s, median (min..max) | 1T 185153 (179049..192237); 8T 1140588 (1096077..1167338, drifting down) | PGO, throttled | `nps_pgo.log` |
| SKILLS_TREE (hard rule 10) | `"concurrent hash table with pointer stability"`: none matched. `"slab or arena allocator for fixed-size objects"`: none matched. No note backs the design opinions below | | `phase1_raw.md` section 4 |

Spot checks done for this report: the `PERF03STATS` lines, the RSS lines, the probe outputs, the flat
perf lines for `findNode`, rwlock and `Accumulator::move`, and the `graphMemoryUsed` identity
all match `phase1_raw.md`.

### What the numbers say

- A table node really costs **96 B**: 56 B `Node` + 32 B rb-tree header + 8 B malloc header, with
  no padding. The overhead beyond `sizeof(Node)` is **40 B**, not the 48 B in `NodeTableEntryOverhead`.
- Heap split at the 2M run: table 511683 x 96 = 49.1 MB (33 %), edge arrays 101.5 MB (67 %).
  Edge arrays are out of scope (Don't touch: edge-array path, SEARCH-08/10).
- **Ceiling for this task.** Even a table with zero overhead keeps the 56 B `Node`
  (511683 x 56 = 28.7 MB). The most any table structure can save is 40 B x 511683 = **20.5 MB,
  10.9 % of peak RSS**. Every candidate below is a fraction of that.
- **Speed ceiling.** The whole table path is at most 5.3-6.5 % of cycles. Making it free would
  give at most 1 / (1 - 0.065) = **+7 % NPS**. Lock code is 0.4-0.6 % and shows no contention
  growth from 1T to 8T. Almost all the table cost is the cache-missing tree walk inside `findNode`
  (about 9 levels for about 500 nodes per shard).
- **Measurement limit.** Under the 24 W limit, the 1T runs spread over 7 % (179k..192k) and the 8T
  series drifts 6 %. A gain of a few percent cannot be resolved with wall-clock NPS on this
  machine. Phase 2 should compare with interleaved A/B runs and `perf stat` cycles per visit.
- **Strength metric when the budget binds:** graph nodes per fixed budget. Today the accounted
  cost is 156772880 / 511683 = 306.4 B per graph node (104 node + 202.4 edges). The real cost is
  294.3 B (96 + 198.3). The column "nodes per budget" below assumes the node constant is set to
  the candidate's real cost. It gives two values: edge estimate unchanged / edge estimate also
  corrected (separate item, see Accounting).

### Candidates

Shard size at the 2M run: 511683 / 1024 = about 500 nodes per shard.

| Rank | Candidate | Bytes/node | Saved at 2M run | % of peak RSS | Share of the 20.5 MB ceiling | Nodes per budget | NPS effect (bound) | Risk | Test cost |
|---|---|---|---|---|---|---|---|---|---|
| 1 | (b) per-shard open addressing of tagged `Node *` + slab pool | about 73 (range 67-77) | about 11.8 MB (9.7-14.8) | 6.2 % (5.2-7.9) | about 58 % | +11.3 % / +12.9 % | removes the tree walk: plausibly +2..4 %, at most +7 %; not resolvable here | medium (concurrency and accounting; pointer stability is kept by design) | high: the full phase-2 list, TSan/ASan, probe length after recycles |
| 2 | Accounting fix only (`NodeTableEntryOverhead` 48 -> 40) | 96 (unchanged) | 0 | 0 | 0 | +2.7 % / +4.1 % | 0 | very low | low: one accounting test, budget tests re-run |
| 3 | (a) pool allocator for the `std::set` | about 89 (88 + slab slack) | about 3.6 MB (4.1 gross) | 1.9 % (2.2 gross) | about 18-20 % | +5.2 % / +6.7 % | node malloc only, at most +0.3 % | low | low to moderate |
| 4 | Close as not worth it | 96 | 0 | 0 | 0 | 0 | 0 | none | none |
| - | (c) inline nodes, fixed-capacity open addressing | 56 / LF: 62 at 0.9, 75 at 0.75, 112 at 0.5 | cannot be sized for this run (see below) | - | - | - | similar to (b) | **high**, rejected | high |

#### (a) Pool allocator for `std::set` (per-shard slab of 88 B tree nodes)

- Bytes: the tree node stays 88 B (32 B header + 56 B Node, 8-aligned, so the stride is 88 B).
  Only the 8 B malloc header goes away: 96 -> 88 B, gross saving 8 x 511683 = 4.09 MB = 2.2 %.
- Slab slack x 1024 shards is the trap. Fixed 64-node slabs, half full on average:
  1024 x 32 x 88 = 2.88 MB, which cancels 70 % of the saving. Lazily grown slabs
  (8, 16, 32, ... nodes, capped at 64) keep the slack near 1024 x 8 x 88 = 0.72 MB at this shard
  size, about 1 B/node. Net about 7 B/node = 3.6 MB = 1.9 % of RSS.
- NPS: the tree walk does not change, and the tree depth and node size stay the same. Only the
  node `malloc` goes away, which is at most about 0.3 % of cycles (see Inconsistencies). Expect
  no measurable change.
- Pointer stability: kept. Slabs are never reallocated, and the set still links nodes by pointer.
- Concurrency: allocation already runs under the shard's unique lock, and erase (recycle/clear)
  runs one shard per task between searches. A per-shard pool needs no extra synchronisation. If
  two threads miss on the same hash, the loser's speculatively built node goes back to the pool
  under the same lock.
- Test cost: low to moderate. Allocator tests (reuse after erase, clear frees the slabs, lazy
  growth), pointer-stability and accounting tests, and the golden comparison.
- Verdict: the smallest change, but it buys only about 3 points of nodes-per-budget over the
  accounting fix alone. That is not a good trade for writing and testing an allocator.

#### (b) Per-shard open addressing of `Node *` + slab pool of `Node`

- Bytes: `Node` in a slab is 56 B, with no header. The slot is 8 B. The power-of-two capacity
  grows at load 0.75, so the load factor sits in 0.375..0.75 and a slot costs 8 / LF =
  10.7..21.3 B/node. At this run, 500 nodes per shard gives capacity 1024 and LF 0.49, so
  16.4 B/node. Lazy slab slack at a 16-node step is 1024 x 8 x 56 = 0.46 MB, about 1 B/node.
  Total about 73 B/node, so 96 - 73 = 23 B x 511683 = 11.8 MB = 6.2 % of RSS. The range over
  load factors is 67..77 B, which saves 9.7..14.8 MB (5.2..7.9 %).
- The 8 B slot must carry a hash tag. Without one, every probe dereferences the node to compare
  hashes, which is still fewer misses than the tree walk. A 16 B slot (pointer + hash) would cost
  56 + 32.8 = 89 B/node and lose almost all the saving. The tag can go in the upper 16 bits of
  the pointer. `Edge` already relies on 48-bit user pointers (`Edge::PointerBits = 48`,
  `pointerTooWide` check, `node.h:104`), so this adds no new platform assumption.
- Rehash moves only the pointers. The transient is per shard: old + new slot arrays of one shard,
  at most about 16 KB at this size, about 1/1024 of the table.
- NPS: this is the only candidate that touches the measured hot spot. `findNode` self is
  4.2-4.9 %, mostly tree-walk misses. With 1-2 probes and one node dereference, plausibly half or
  more of that goes away, together with `_Rb_tree_insert_and_rebalance` (about 0.35 %). The
  expected gain is +2..4 %, bounded by +7 %. That is below what NPS can resolve on this machine,
  so phase 2 must measure cycles per visit and must not promise a speedup.
- Pointer stability: kept by design. Nodes stay at their slab address, and only slots move on
  rehash.
- Concurrency: keep the per-shard `std::shared_mutex` exactly as today. `findNode` holds the
  shared lock, and emplace and rehash hold the unique lock. The emplace race then stays identical
  to today (check under the unique lock, one winner, the loser gets the winner's pointer), and no
  new memory-ordering contract is introduced. Lock-free reads are explicitly out of scope: lock
  cost is only 0.4-0.6 %, and lock-free reads would bring the release/acquire pitfall.
- Recycle and clear run when no search is running. After erasing the stale nodes, rebuild the
  shard's slot array from the live nodes instead of leaving tombstones. Probe lengths then do not
  grow over a long game, and node addresses do not change. Freed nodes go to the shard free list.
- Risk: medium. The hard parts (race, tag collisions, rebuild-on-recycle, accounting) are
  testable, and wrong code gives rare wrong results rather than crashes, hence TSan/ASan.
- Test cost: high. The full phase-2 list in the instruction, plus tag-collision tests (two hashes
  with the same tag in one shard) and probe length after several recycles.

#### (c) Nodes inline in a fixed-capacity open-addressing table (rejected)

- Bytes: 56 / LF per node, using `Node`'s own hash field as the key, with reserved sentinel
  values for empty and tombstone. That is 62 B at LF 0.9, 75 B at 0.75, 93 B at 0.6 and 112 B at
  0.5. Below LF of about 0.58 it costs more than today's 96 B.
- Capacity must be fixed up front, because rehash and backward-shift deletion move nodes, which
  is forbidden. It would have to come from the memory budget, but the budget's node/edge split is
  not known in advance: 33/67 here, and it changes with the position, `mcts_edge_policy_mass` and
  the search length. Sizing for the worst case means a low LF most of the time.
- Hashing spreads nodes over all slots, so every page is touched early and the whole capacity
  becomes RSS almost at once. In this run (6 GB budget, 49 MB of real table), a table sized from
  the budget would raise RSS by an order of magnitude instead of lowering it.
- Tombstones from recycles pile up over a long game. They cannot be compacted without moving
  nodes, so probe lengths only grow until `clear`.
- Verdict: not worth the risk. The best case (about 62 B at LF 0.9) is only about 10 B/node
  better than (b), and it is reachable only with long probe chains and a correct size guess.

### Accounting inaccuracy (separate from the structure choice)

- `NodeTableEntryOverhead = 48` (`search/mcts/parameter.h:60`) against a real 40 B. The comment
  there says "allocator header (~16 B)"; the real header is 8 B.
- `edgeArrayFootprint` = 8 + 16n + `EdgeArrayAllocOverhead` (16) = 16n + 24, against a real chunk of
  16n + 16, so 8 B too high per array.
- Together they overstate the graph heap by 8 x 511683 + 8 x 262077 = 6.19 MB (measured
  156772880 - 150589664 = 6.18 MB), i.e. **4.1 %**. `graphClosed()` therefore fires when the
  real graph heap is about budget / 1.041, about 4 % early. This is not purely a bug.
  `graphMemoryUsed` leaves out the about 36 MB process baseline and malloc and page slack (the
  RSS-based estimate is 97-98 B/node, not 96). Against a total-process memory limit, the
  over-count partly acts as a safety margin. Correcting it is a class-B behaviour change on
  budget-limited runs only, and it does not change the unbounded golden run.
- Where it belongs: the node constant belongs to PERF-03, because phase 2 step 3 already says to
  set it to the measured value of whatever structure is chosen (for "accounting fix only", that
  is all of phase 2). `EdgeArrayAllocOverhead` is in the edge-array path, which is Don't-touch
  for PERF-03, so it should be a separate item. It is not recorded yet; open it only if the user
  wants it. Not fixed in phase 1.

### Inconsistencies found in the inputs

- The hand-off summary's "peak RSS ~184 MB" is 184 **thousand KiB** = 188.5 MB (179.8 MiB). This
  report uses 188.5 MB.
- The hand-off summary's "node malloc/free ~0.8-0.9 %" overstates the node share. In a fresh 1T
  search no node is ever freed: there is no recycle, and an emplace after a miss in the same
  thread cannot lose. So the free side (0.56-0.69 % self) is other allocations. The node
  allocation share is at most the malloc side, 0.23-0.29 % self (operator new inclusive
  0.31-0.34 %, which also includes edge arrays).
- The measured over-count of 6.183 MB against the predicted 6.190 MB differs by about 7 KB. That
  is heap noise outside the graph, which is fine.
- Address dependence: nothing sorts, hashes or compares by `Node *` (`search/mcts/`). The only
  iteration over the table is `processAllShards` (clear, and recycle at `search.cpp:1590`), whose
  result does not depend on order. A new node layout should therefore keep the golden output
  identical. Phase 2 confirms this with the golden comparison.

### Recommendation

**(b) per-shard open addressing of tagged 8 B `Node *` slots + lazily grown per-shard slab pool,
keeping the per-shard `shared_mutex`, rebuilding the shard's slot array after recycle (no
tombstones), and setting the node constant to the measured value.**

Reason: it is the only candidate that gets a meaningful part of the 20.5 MB ceiling (about 58 %,
11.8 MB, 6.2 % of RSS, +11-13 % nodes per budget). It is also the only one that removes the
measured hot spot, the `findNode` tree walk at 4.2-4.9 % of cycles. Keeping the mutex keeps the
emplace race and the memory-ordering contract the same as today, so the new risk is limited to
the table code itself. (a) costs almost as much testing for about one third of the saving and no
speed gain. (c) cannot be sized and violates pointer stability without tombstones.

Honest caveat: the whole task is small. The table is one third of graph memory, and the edge
arrays (two thirds) are out of scope. If about 6 % RSS plus an NPS gain that cannot be measured on
this machine does not justify medium-risk concurrent-table work, take **rank 2, accounting fix
only** (`NodeTableEntryOverhead` 48 -> 40, +2.7 % nodes per budget, very low risk) and close
PERF-03.

**Classification: B** for every option. Node storage changes with bit-identical output while the
graph is unbounded, but the accounting constant changes behaviour on budget-limited runs. It is
not A, because no hash, position identity, encoding or compile-time table changes, so no
derived-state audit is needed.

**Decision: pending user choice** ((b) / accounting fix only / (a) / close).

**Decision (2026-11-09, user): option (b).**

## Phase 2 — option (b)

### Design

- **Shard data.** Each shard keeps its `std::shared_mutex` exactly as before (separate array,
  shared lock for `findNode`, unique lock for `tryEmplaceNode` and for the recycle/clear hooks).
  No lock-free reads.
- **Slot array.** Open addressing, linear probing, power-of-two capacity, allocated lazily on
  the first insert (16 slots). A slot is one `uint64_t`: the low 48 bits are the `Node *`, the
  upper 16 bits are a tag (`hash >> 48`). `0` is the empty slot. The start index is
  `(hash >> shardBits) & (capacity - 1)`, so it does not reuse the shard bits. A tag match is only
  a filter: the node's own hash is compared before a hit is returned. A pointer wider than 48 bits
  aborts, the same guard as `Edge`.
- **Growth.** The capacity doubles when an insert would take the load above 3/4. The rehash moves
  only slot words, never a `Node`.
- **Slab pool.** Nodes live in per-shard slabs of raw storage built with placement new. Slabs are
  allocated lazily, one fixed-size slab at a time, and are never moved or reallocated until the
  shard is cleared. Freed nodes go on a LIFO free list (next pointer stored in the dead storage),
  and the free list is used before the bump pointer.
- **Recycle.** `NodeTable::eraseNodesIf(shard, pred)` calls `pred(node)` on each live node while
  the node is still intact (the caller reads the edge footprint there), destroys the nodes for
  which it returns true, puts their storage on the free list and rebuilds the shard's slot array
  from the survivors (no tombstones). Survivors keep their addresses.
- **Clear.** `NodeTable::clearShard(shard)` destroys every live node (which frees its edge array,
  as `~Node` does today) and releases the slot array and all slabs. `~NodeTable` does the same.
- **Iteration.** `NodeTable::forEachNode(shard, f)`. The container is not exposed.

### Test list (phase 2, `tests/mcts_nodetable_tests.cpp`)

Unit tests use a one-shard table (`NodeTable(0)`) unless the test is about shards, seeded
`std::mt19937_64` hashes, and each test builds its own table.

1. `findNode_emptyTable_returnsNull` — no slot array yet (capacity 0 must not be masked).
2. `findNode_missingHashInNonEmptyShard_returnsNull`.
3. `tryEmplaceNode_newHash_insertsOnceAndFindReturnsSamePointer`.
4. `tryEmplaceNode_existingHash_returnsSameNodeNotInserted`.
5. `tryEmplaceNode_newHash_constructsNodeWithHashAndAge`.
6. `tryEmplaceNode_sameTagDifferentHash_bothFoundSeparately` — two hashes with the same upper 16
   bits and the same probe start, differing only in middle bits.
7. `tryEmplaceNode_manyInsertsOneShard_slotArrayGrowsAndHeldPointersStayValid` — hold pointers,
   insert enough to force several rehashes; every held pointer still reads its own hash and
   `findNode` returns the same pointer.
8. `tryEmplaceNode_manyInsertsOneShard_slabsGrowAndHeldPointersStayValid` — same, checking that
   new slabs were added and old nodes did not move.
9. `tryEmplaceNode_manyInserts_loadFactorStaysAtMostThreeQuarters`.
10. `tryEmplaceNode_sameHashFromManyThreads_exactlyOneInsertsAllGetSamePointer` — 8 threads,
    barrier start, repeated over many hashes.
11. `tryEmplaceNode_disjointHashesFromManyThreads_nothingLost` — 8 threads, 4 shards.
12. `eraseNodesIf_staleAge_removesStaleAndKeepsReachedAtSameAddress`.
13. `eraseNodesIf_predicateSeesEdgesBeforeDestruction` — the predicate can read the edge array
    of the node it erases.
14. `eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab`.
15. `eraseNodesIf_manyRecycleRounds_probeLengthStaysBounded` — 20 rounds of insert + erase-half;
    load factor at most 3/4 and the mean probe length stays small (no tombstone build-up).
16. `clearShard_nonEmpty_emptiesShardAndReleasesStorage` — no nodes, no slots, no slabs, find
    returns null, insert works again.
17. `forEachNode_afterInsertsAndErase_visitsEachLiveNodeOnce`.
18. `heapBytes_afterInserts_equalsSlotsPlusSlabs` — the measuring hook used for the footprint
    constant.
19. `recycleOldNodes_afterMoveForward_graphMemoryUsedEqualsLiveFootprints` — end to end through
    the engine (real `allocateOrFindNode`, `createEdgesFrom` and `recycleOldNodes`): after a search,
    a forward move and a second search, `graphMemoryUsed` equals the sum over live table nodes of
    `nodeFootprint()` plus their edge-array footprints.
20. `clear_afterSearch_tableEmptyAndGraphMemoryUsedZero` — end to end.

Failing first (2026-11-09, `build/perf03/phase2/tests_failing_first.txt`): the hooks
`eraseNodesIf` / `clearShard` / `forEachNode` were first added to the old `std::set` table and
`recycleOldNodes` / `clear` switched to them (no behaviour change). Against that table the full
file does not compile: tests 7, 8, 9, 14, 15, 16 and 18 use the new statistics API
(`getShardStats`, `SlabNodes`, `SlabBytes`, `heapBytes`). With those 7 excluded by a temporary
local `#if 0`, the other 13 pass on `std::set` (221/221, 208 before). So the `test:` commit
alone does not build `rapfi_tests`; the `perf:` commit makes it build.
