# PERF-03 — execution guidance

Task: [`../todo/PERF-03-mcts-node-table-footprint.md`](../todo/PERF-03-mcts-node-table-footprint.md).

The task has two phases with a **user decision between them**. Phase 1 only measures and does not
change behaviour. Do not start phase 2 until the user picks a structure from the phase-1 report.

## Current state (read before starting)

- `search/mcts/nodetable.h`: `NodeTable` is `1 << numNodeTableShardsPowerOfTwo` shards (default 10,
  `search/searchconfig.h:75`, TOML `num_node_table_shards_power_of_two`). Each shard is a
  `std::set<Node, NodeCompare>` and a `std::shared_mutex`. `findNode` takes the shared lock,
  `tryEmplaceNode` takes the unique lock.
- Only callers: `allocateOrFindNode` (`search/mcts/search.cpp:380`, find, then emplace on a miss),
  `MCTSSearcher::clear` (`table.clear()` per shard through `processAllShards`) and
  `recycleOldNodes` (`search.cpp:1558`, walks each shard and erases nodes whose age is not
  `globalNodeAge`). Clear and recycle run between searches on all threads, one shard per task;
  they never run while a search is running.
- Accounting: `nodeFootprint() = sizeof(Node) + NodeTableEntryOverhead` (`node.h:471`,
  `parameter.h:60`, overhead `48` is a guess). It is added on insert and subtracted on recycle;
  `graphMemoryUsed` drives `graphClosed()` and the default budget (SEARCH-03).
- `sizeof(Node) == 56` is enforced by a `static_assert` (`node.h:467`) that names the `std::set` node
  size class (SEARCH-08). `Node` cannot be copied or moved. `Edge::child` and `graph.root` hold raw
  `Node *`, so **a node must never move while it is in the table**.

## Phase 1 — measure (no behaviour change)

Keep all outputs under `build/perf03/`, not in the repo.

1. **Baselines from HEAD**:
   - `echo BENCH | ./pbrain-rapfi` gives Nodes `17621937`, Hash `8c1f72e5`. MCTS is not covered
     by BENCH, so this only shows that AB did not change.
   - Golden MCTS output: 1 thread, fixed node count, fixed position, **memory large enough that the
     graph never closes**. Save the root move list (move, visits, Q). Reuse
     `build/search08/run_fixed.py` and `golden_extract.sh`.
   - Peak RSS (`/usr/bin/time -v`) of one fixed 2M-node search, Standard, with the SEARCH-05
     settings. Also record the node count and `graphMemoryUsed` at the end of the search.
   - MCTS NPS on the PGO build (`scripts/pgo_build.sh x64-clang-AVX512VNNI`), 1 and 8 threads,
     fixed time, median of 5 runs or more. Check the CPU power state first, as in
     `build/search08/power_state.txt`: a 24 W PL1 limit costs about 33 % NPS and hides real changes.
2. **Measure the real bytes per node** with three methods that cross-check each other:
   - Allocator: `malloc_usable_size` of one `std::set<Node>` node, or a counting allocator in a
     throwaway build, for glibc as linked.
   - Whole process: (RSS after the search − RSS of the same setup at 0 nodes) / reachable nodes,
     with the edge-array bytes subtracted using `edgeArrayFootprint`.
   - Optional: `heaptrack` or `valgrind --tool=massif` on a short run.
   Report the result next to the estimate `56 + 48 = 104` bytes.
3. **Measure lock and lookup cost.** With `perf record` on 8 threads, find the share of cycles
   spent in `findNode`, `tryEmplaceNode`, the tree walk and the `shared_mutex` code. If that share is
   small, a new structure saves memory but not speed, and the report must say so.
4. **Look up design knowledge** (hard rule 10). Delegate to an Agent:
   `SKILLS_TREE/tools/find -k knowledge "concurrent hash table with pointer stability"` and
   `"slab or arena allocator for fixed-size objects"`. Name the notes you use, or say that none matched.
5. **Report to the user and stop.** Write the numbers and a ranked list of candidates in the todo
   file, each with the bytes per node it saves (computed from step 2), its risk and its test cost.
   The candidates to cover are at least:
   - **(a) Pool allocator for the `std::set`**: a per-shard slab of fixed-size tree nodes. This
     removes the malloc header and fragmentation, keeps the tree and the pointer stability, and is
     the smallest change.
   - **(b) Per-shard open-addressing table of `Node *` plus a slab pool of `Node`**: the table may
     rehash because it only holds pointers, and the nodes stay where they are in the slab.
     Recycle puts free nodes back on a free list.
   - **(c) Nodes stored inline in a fixed-capacity open-addressing table**: smallest footprint, but
     it can never grow or rehash, and deletion with backward shift moves nodes, which is forbidden.
     It only works with tombstones and a capacity set from the memory budget. Explain why it is or
     is not worth the risk.
   Recommend one and give the reason. Classify it (A / B / C, `docs/process/change-process.md`).
   A change of node storage with bit-identical search output is normally B; do the derived-state
   audit if you call it A.

## Phase 2 — implement the chosen structure (only after the user picks one)

1. Write the test list in the todo file, then write the tests and **see them fail first**
   (`tests/mcts_nodetable_tests.cpp`, add it to `TEST_DRIVER_SOURCES`). At least:
   - `findNode_missingHash_returnsNull`, `tryEmplaceNode_newHash_insertsOnce`,
     `tryEmplaceNode_existingHash_returnsSameNodeNotInserted`.
   - Race: N threads emplace the same hash, and exactly one gets `inserted == true`. All of them
     get the same pointer. N threads emplace disjoint hashes and nothing is lost.
   - Pointer stability: hold `Node *` values and insert many more nodes into the same shard
     (forcing any rehash or slab growth). The held pointers still read their own hash.
   - Recycle: erase by age keeps the reached nodes at the same addresses, and freed slots are reused.
   - Clear: the table is empty and all memory goes back to the pool or the OS, as the design specifies.
   - Accounting: after insert, recycle and clear, `graphMemoryUsed` equals the sum of the
     footprints of the live nodes and edge arrays.
   Seed every PRNG. Each test builds its own objects. Run with asserts live (`rapfi_tests`).
2. Keep the public API of `NodeTable` (`findNode`, `tryEmplaceNode`, `getNumShards`,
   `getShardByShardIndex`, and per-shard iteration with erase for recycle). If recycle needs a new
   iteration hook, add it to `NodeTable` rather than exposing the container.
3. Update `NodeTableEntryOverhead` (or replace `nodeFootprint`) with the **measured** value of the
   new structure. Update the comment on the `sizeof(Node)` `static_assert` if the reason in it
   (the `std::set` size class) is no longer true. Do not change `sizeof(Node)` itself.
4. Verify:
   - Unit tests pass with asserts live. Run the race tests under the TSan build pattern from
     `build/s10-tsan` and the ASan pattern from `build/s10-asan`.
   - Bench is unchanged. The golden output from phase 1 is **identical** (the table must not change
     search order when the graph never closes).
   - Peak RSS on the 2M-node run and NPS at 1 and 8 threads on PGO, against the phase-1 numbers.
   - Budget runs: the graph closes later for the same budget only by the amount of the real
     overhead change. Check `tests/mcts_memory_tests.cpp` and the SEARCH-03 default budget still pass.
5. Rebuild the release with `scripts/pgo_build.sh x64-clang-AVX512VNNI` after the last source change.

## Pitfalls

- **Pointer stability is the main risk.** Any structure that moves a `Node` (vector growth,
  rehash of inline storage, backward-shift deletion, compaction) creates dangling `Edge::child` and
  `graph.root` pointers. These show up as rare wrong results, not crashes.
- **The emplace race must stay exact.** Two threads that miss in `findNode` both call
  `tryEmplaceNode`. Only one may create the node and add to `graphMemoryUsed`. The loser must get the
  winner's pointer.
- **Memory order:** today the shard mutex publishes a new node to the threads that find it. A
  lock-free lookup needs a release store of the pointer after construction and an acquire load on
  find. Do not use relaxed ordering on the pointer slot.
- **Changing the accounting changes behaviour on budget-limited runs.** A smaller footprint means the
  graph closes later, which changes MCTS output when the budget is reached. Keep the golden run
  unbounded, and report the budget effect on its own.
- **Recycle iterates while it erases.** Open addressing with tombstones must clean up or rebuild,
  or the probe lengths grow over a long game. Measure the probe length after several recycles.
- **Shard count:** 1024 shards with a small table each. A per-shard pool with a large first slab
  multiplies the empty-table memory by 1024. Size the slabs lazily.
- **Plain `ninja` builds are only compile checks.** RSS and NPS numbers come from the PGO build.
  Record the compiler for every number.

## Don't touch

- `Node` and `Edge` layout and size (SEARCH-08), the edge-array path and `RetiredEdgeArrays`
  (SEARCH-10).
- `mcts_edge_policy_mass`, `mcts_expand_min_visits`, `mctsDefaultMemoryPercent` and other defaults
  (SEARCH-02/03/05).
- AB search, `game/pattern.*`, eval.
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`). Read them only; do not
  edit or commit them.

## Definition of done

- Phase 1: numbers and the ranked candidate list in the todo file, and the user's choice recorded.
- Phase 2: tests from the todo pass with asserts live, and under TSan and ASan. Bench is
  `17621937 / 8c1f72e5`. The golden output is identical. Peak RSS is lower by about the amount
  phase 1 predicted. NPS is within noise or better.
- Move the todo and the `TODO.md` / `instruction.md` lines to `## Done` with the date and the numbers.
- Commits in conventional style (`perf:`, `test:`, `docs:`): a measurement-only `docs:` commit for
  phase 1, then `test:` and `perf:` commits for phase 2.
