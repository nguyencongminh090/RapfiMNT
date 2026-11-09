# PERF-04 — execution guidance

Task: [`../todo/PERF-04-mcts-slab-pool-trim.md`](../todo/PERF-04-mcts-slab-pool-trim.md).
Background: PERF-03 todo, sections "Phase 2 verification and review" (bound on the gap, finding 3)
and `build/perf03/phase2/verify_raw.md` check 8.

The task has two phases with a **user decision between them**, as in PERF-03. Phase 1 measures how
much each candidate would actually reclaim and ranks them. It changes no tracked file except the
todo. Do not start phase 2 until the user picks a candidate.

## Model split

| Step | Model | Why |
|---|---|---|
| Phase 1 step 1: baselines, rebuild the game harness | Sonnet | Scripted, reuses `build/perf03/phase2/` |
| Phase 1 step 2: instrumented "what would be freed" measurement | Sonnet | Throwaway copy, counting only |
| Phase 1 step 3: rank candidates, decide accounting invariant, classify, write the report | Opus | Sets the memory contract of the graph budget |
| Phase 2: test list, tests, core change in `NodeTable` and accounting | Opus | Free-list rebuild, slab release and counter symmetry; bugs give use-after-free or a wrong budget |
| Phase 2 verification runs (TSan/ASan, golden, game sequence, RSS, cycles, budget closure, PGO) | Sonnet | Scripted |
| Sanitizer finding or golden / recycle-sequence diff: root cause | Opus | Hard rule 4 |
| Final diff review before merge | Opus | |

## Current state (read before starting)

- `search/mcts/nodetable.h`:
  - `ShardData` has `slots`/`capacity`/`numNodes` (open addressing of tagged `Node *`), `slabs`
    (singly linked, newest first, `Slab { Slab *next; storage[SlabNodes * sizeof(Node)] }`,
    `SlabNodes = 32`), `slabUsed` (bump index into the newest slab), `freeList` / `numFree`
    (LIFO list threaded through dead node storage).
  - `allocateNodeStorage`: free list first, then the bump slab, then a new slab (48-bit address guard).
  - `eraseNodesIf` → `freeNodeStorage` (destroy, push on free list) → `rebuildAfterErase`
    (slot array resized to the survivors at load ≤ 3/4, min 16).
  - `releaseAll` (from `clearShard` and `~ShardData`) is the only place slabs are freed.
  - `getShardStats` and `heapBytes` are the measurement hooks.
- `search/mcts/search.cpp`: `recycleOldNodes` (~1560) marks reachable nodes with `recursiveApply`
  (~1222; it follows every edge with a non-null child), then calls `eraseNodesIf` per shard through
  `processAllShards`. Its predicate subtracts `nodeFootprint()` plus the edge-array footprint from
  `graphMemoryUsed` before the node is destroyed. `allocateOrFindNode` (~380) adds `nodeFootprint()` on
  insert.
- `search/mcts/membudget.h`: `graphInsertionClosed` closes at 85 % of the limit;
  `deriveDefaultGraphBudget(availableKB, graphUsedBytes, percent)` treats `graphMemoryUsed` as memory
  the process already owns.
- `nodeFootprint() = sizeof(Node) + NodeTableEntryOverhead` = 56 + 19 = 75 B (`parameter.h`), an
  average that already includes the slot-array share and slab slack at the reference size.

## Phase 1 — measure and rank (no behaviour change)

Outputs under `build/perf04/`, not in the repo.

1. **Baselines from HEAD** (main, PERF-03 merged):
   - Bench `17621937 / 8c1f72e5`. Golden: reuse `build/perf03/golden_gcc_1.extract` (setup in
     `build/perf03/phase1_raw.md`); confirm HEAD still matches it.
   - The 13-move game sequence of PERF-03 check 8 (`build/perf03/phase2/multi.py`, `multi_d2.py`,
     `review_ab/`): per move, live nodes, `graphMemoryUsed`, `heapBytes()`, `mallinfo2().uordblks`,
     peak RSS. This is the "before" curve of the gap.
   - Budget-limited closure at 40 / 90 / 200 MB (PERF-03 check 7 method), and the same game run
     with a budget small enough that recycles happen near the soft limit: does the real heap exceed
     the configured budget, and by how much?
2. **What each candidate would reclaim**, with an instrumented throwaway copy (counting only, the
   instrumented copy must not change behaviour). After every recycle in the game, per shard:
   - slabs with zero live nodes (excluding the bump slab), and their bytes;
   - the distribution of live nodes per slab (how fragmented the survivors are);
   - free-list bytes.
   This tells whether "free empty slabs" reclaims most of the gap or almost nothing because
   survivors are spread over many slabs. Also run one longer game (≥ 30 moves) to see whether the
   pool stabilises.
3. **Look up design knowledge** (hard rule 10), delegated to an Agent:
   `SKILLS_TREE/tools/find -k knowledge "slab allocator reclaim empty slabs fragmentation"` and
   `"memory budget accounting allocator retained memory"`. Name the notes used or say none matched.
4. **Report and stop (Opus).** Append "Phase 1 results" to the todo. Cover at least:
   - **(a) Free fully empty slabs after recycle.** Needs node → slab mapping: either aligned slabs
     (size and waste trade-off: `SlabBytes` is 1800, power-of-two alignment wastes ~12 %) or a sort
     of slab addresses and a binary search during `eraseNodesIf` (cost is paid between searches only).
     The free list must be rebuilt without entries in freed slabs. Reclaim = step 2 numbers.
   - **(b) Honest accounting, no release.** Count retained storage in the budget. Two forms:
     (b1) recycle subtracts only the edge-array footprint, and an allocation from the free list adds
     nothing; (b2) replace the per-node constant with exact table accounting: add/subtract the real
     bytes when a slab or slot array is allocated, rehashed or released. Under (b2) `graphMemoryUsed`
     follows real table allocations exactly, and `NodeTableEntryOverhead` disappears from the table
     path. Effect: after a recycle the graph closes earlier than today; memory is never returned.
   - **(c) Hybrid:** (b) always, plus (a) when a shard's free share exceeds a threshold.
   - **Close as not worth it**, if step 1 shows the real heap stays within the configured budget in
     practice.
   For each: bytes reclaimed in the game, the budget invariant it guarantees (state it as an
   inequality between real table heap and `graphMemoryUsed`), effect on golden (must be none when the
   graph is unbounded), effect on budget-limited runs, risk, and test cost. Recommend one and classify
   it (A / B / C, `docs/process/change-process.md`). Then stop for the user's choice.

## Phase 2 — implement the chosen candidate (only after the user picks one)

1. Write the test list in the todo, then the tests in `tests/mcts_nodetable_tests.cpp` (and
   `tests/mcts_memory_tests.cpp` for budget behaviour), and see the new ones fail first. At least, as
   they apply to the choice:
   - `eraseNodesIf_allNodesOfSlabErased_slabReleased`, `eraseNodesIf_slabPartlyLive_slabKept`;
   - survivors keep their addresses across the release;
   - no free-list entry points into a released slab (allocate until the free list is empty; every
     returned address lies in a live slab);
   - the bump slab is never released while it is the allocation target, or the bump state is reset
     correctly when it is;
   - the chosen accounting invariant holds after insert, recycle, re-insert from the free list, and
     clear (compare with `heapBytes()` or the exact counter);
   - `graphClosed()` behaviour before and after a recycle matches the invariant.
   Seed every PRNG; each test builds its own objects; asserts live (`rapfi_tests`).
2. Keep the `NodeTable` public API. Keep all slab and free-list work inside `eraseNodesIf` /
   `rebuildAfterErase` / `releaseAll`, under the shard's unique lock, between searches.
3. Verify: unit tests, TSan on the thread tests (pattern `build/perf03/phase2/build_san.sh`,
   `nt_loop.cpp`, `tsan.supp`), ASan + UBSan (`-fno-sanitize=enum`, see the known AB hit) on the suite and on
   the real game sequence with recycles and `YXHASHCLEAR`; bench; golden byte-identical; the
   PERF-03 A/B recycle sequence (`build/perf03/phase2/review_ab/`) byte-identical against
   `pbrain-B-gcc` when unbounded; the game curve of the gap after the change; budget-limited
   closure; cycles per visit A/B (PGO, interleaved, power state recorded); PGO release rebuild.

## Pitfalls

- **Releasing a slab turns any stale `Node *` into a use-after-free.** Today a stale pointer to a
  recycled node reads reused or dead storage inside a live slab, which ASan cannot see. Before
  releasing slabs, audit every holder of `Node *` that survives a recycle: `graph.root`, edge
  `childWord`s of survivors (`recursiveApply` marks every non-null child, so they should all be
  survivors), `RetiredEdgeArrays`, root-move data, anything in `scratch`/`GraphState`. Write the
  audit into the todo. Consider ASan poisoning of freed node storage (PERF-03 follow-up 2) as a
  test aid.
- **The free list is threaded through dead storage.** Releasing a slab without rebuilding the list
  leaves entries that point into freed memory. Rebuild the list from scratch after the release, or
  unlink per slab.
- **The bump slab** (`d.slabs`, `slabUsed`) is partly unused storage that is not on the free list.
  Count it correctly, and do not release it while it is the allocation target without resetting
  `slabUsed`.
- **Accounting must stay symmetric.** Today: +75 on insert, −75 on recycle. Any change (b1, b2)
  must update both sides and `clear()`, and keep `graphMemoryUsed` from underflowing (it is
  `size_t`). Under (b2), `rehash` and `rebuildAfterErase` change the counter, so they need access to
  it or must return a delta.
- **Budget changes are behaviour changes.** Any accounting change moves the point where the graph
  closes and changes MCTS output on budget-limited runs (class B, hard rule 9: decision note and
  numbers). The golden run must stay unbounded and byte-identical.
- **`deriveDefaultGraphBudget` uses `graphMemoryUsed`** as memory already owned. Releasing memory
  or counting retained storage changes the default budget on the next search; check both directions.
- **Slab churn.** Freeing a slab that the next search refills costs `new`/`delete` per slab. Keep a
  small reserve per shard or a threshold if cycles per visit or recycle time moves.
- **1024 shards.** Any per-shard reserve or bookkeeping is multiplied by 1024.
- **CPU throttling** (PL1 24 W): compare cycles per visit, not wall NPS.

## Don't touch

- `Node` / `Edge` layout and size, the edge-array path, `RetiredEdgeArrays`,
  `EdgeArrayAllocOverhead` (separate follow-up).
- The slot-array design of PERF-03 (tagging, probing, growth), except where the choice needs a hook.
- The shard-count compare in `MCTSSearcher::clear` (`search.cpp`, separate follow-up).
- Search defaults, AB search, pattern, eval.
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`).

## Definition of done

- Phase 1: report, ranking and the user's choice recorded in the todo.
- Phase 2: tests from the todo pass with asserts live, under TSan and ASan. Bench is
  `17621937 / 8c1f72e5`. Golden and the unbounded recycle sequence are byte-identical. The game
  curve shows the gap closed as the chosen invariant promises. Budget-limited closure and cycles
  per visit are reported against the PERF-03 numbers.
- Todo and the `TODO.md` / `instruction.md` lines moved to `## Done` with the date and the numbers.
- Commits in conventional style on a branch (`perf-04-slab-pool`): a `docs:` commit for phase 1,
  then `test:` and `perf:` commits for phase 2. No push or merge without the user.
