# SEARCH-10 — execution guidance

Task: [`../todo/SEARCH-10-mcts-readd-cut-moves.md`](../todo/SEARCH-10-mcts-readd-cut-moves.md). The
design is fixed in the todo's decision note: trigger-based widening, CAS swap, and a retire list
freed at the search boundary. Read SEARCH-09's todo and instruction too. This task builds on the
uncommitted SEARCH-09 (a) working tree (`EdgeArray::numMoves`, `edgesCut()`, the unknown-bound fold).

## Anchors

- `Node::updateStats`: `search/mcts/node.cpp`, about line 238. It folds `maxBound` over the kept
  edges and `break`s at the first unvisited edge; the SEARCH-09 fold comes after the loop.
- `Node::createEdgesFrom`: `node.cpp`, about line 177. Allocation goes through `EdgeArrayAlloc`
  (`node.cpp:36`); every free must use `delete[] reinterpret_cast<EdgeArrayAlloc *>(p)`.
- `Node::~Node` frees the current array.
- `expandNode`: `search/mcts/search.cpp:413`. The non-root `MovePicker` setup is at 430-441; reuse
  it with `EdgeLimits{1.0f, ...}`.
- `searchNode`: `search.cpp:515`. The playout loop is at 565-655, with `updateStats` at 607, 632
  and 649 while the board is at the child position, and `board.undo` at 655.
- `selectChild`: `search.cpp:312`. It holds `EdgeArray &edges` and returns an `Edge *` that the
  caller later passes to `setChild` / `addVisits`.
- `searchMain`: `search.cpp:1092`. The worker `runOnAllThreads(..., true)` is at 1134 and is the
  quiescent point. `clear`: `search.cpp:1059`. `recycleOldNodes`: `search.cpp:1268`; its footprint
  math covers the current array only.
- `graphMemoryUsed` and `graphClosed()`: `search/mcts/searcher.h`.

## Approach

1. **Baselines** under `build/search10/`. Use the SEARCH-09 (a) working tree as "a", and HEAD
   `42f029b` as "base" (reuse the `build/search09/` outputs where they match).
   - Bench.
   - Golden output with the cut off. Reuse the SEARCH-09 `final_extract.sh` setup.
   - Mate-claim scan with `0.99` over `build/search09/positions.txt`. Reuse `scan.py`.
2. **Tests first** (todo list). Put them in `tests/mcts_bound_tests.cpp`, or in a new
   `tests/mcts_widen_tests.cpp` added to `TEST_DRIVER_SOURCES` and `tests/testmain.cpp`. Record
   which ones fail or do not compile before the code exists.
3. **Implement, in this order:**
   1. `updateStats` returns `bool widenNeeded`. Keep it `[[nodiscard]]`-free, so the existing
      callers that ignore it still compile.
   2. Add `Node::replaceEdgesWithFull(const EdgeArray *expectedOld, moves, policies, numMoves,
      allocCounter, retire)`. `retire` is a callback or a pointer to the searcher's retire list.
   3. Add the retire list and `freeRetiredEdgeArrays()` to `MCTSSearcher`.
   4. Wire it into `searchNode`. Set a local flag when an `updateStats` call in this iteration
      returned true. After `board.undo`, with `!Root && !graphClosed`, regenerate the moves and
      call `replaceEdgesWithFull`.
4. **Verify** every tier in the Definition of done. Then rebuild PGO and run self-play.

## Pitfalls

- **Copy before publish, catch up after.** Build the whole new array, including the copied visits
  (`addVisits` on the private array) and children (`setChild`), before the release CAS. After a
  successful CAS, add `old.getVisits() - copied` per kept edge. Never write to the new array's move
  or policy bits after the CAS (see SEARCH-08: `setP` is constructor-only).
- **CAS against the array you copied from,** not against `nullptr`. If the CAS fails, another
  thread already widened: free your temp array and do not retire anything.
- **Match kept edges by move, not by index.** Do not assume the regenerated order equals the old
  one. Assert that every kept move is present in the regenerated list. If one is missing (it should
  never happen for the same board), abort the widen and keep the old array.
- **Visited prefix:** the kept edges all have visits by construction (the trigger needs every kept
  child to be proven). The appended moves have 0 visits and no child. `selectChild` and
  `updateStats` rely on this prefix.
- **Never free a retired array during a search.** Other threads may hold `EdgeArray &`/`Edge *`
  from `selectChild`, and `extractPVOfChildNode` and the printer read during the search. Free only
  after the worker `runOnAllThreads(..., true)` returns, in `clear(true)` after `waitForIdle`, or in
  the destructor. Confirm that `runOnAllThreads(..., true)` really waits for every worker before
  relying on it.
- **Memory accounting:**
  - On a successful swap, add `edgeArrayFootprint(new)` to `graphMemoryUsed`. Keep the old array's
    footprint counted until it is freed, then subtract it.
  - `recycleOldNodes` and `Node::~Node` see only the current array, which is correct. Retired
    arrays are not owned by any node.
- **Root is never cut,** so it never widens. Keep the `!Root` guard explicit.
- **Transpositions:** one node, one array, so whichever parent's playout triggers the widen, the
  result is the same. The trigger can fire on several threads at once; the CAS settles it.
- **Cost:** regenerating moves needs the network policy at that node. Make sure it runs only when
  the flag is set, never on the hot path.
- **Determinism:** with 1 thread the result must be deterministic run to run. Check it with two
  scan runs on the trigger positions.
- **MCTS is not covered by BENCH.** The golden output, the scan, the stress test and self-play are
  the guards.
- **Tests with asserts live:** `rapfi_tests` forces `-UNDEBUG`. Never do that to `rapfi`. Use
  `FORCE_ASSERTION=ON` for the engine stress build only, not for timing.
- When killing background processes, kill by PID. Never use `pkill -f`/`pgrep -f` with patterns
  that match your own shell.

## Don't touch

- The `Edge` / `Node` / `EdgeArray` sizes (16 / 56 / 8), `selectEdgeCount`, and the defaults
  `mcts_edge_policy_mass` / `mcts_min_edges` / `mcts_expand_min_visits` / graph budget.
- `ValueBound` semantics, `overrideBestByMateBounds`, PUCT/FPU selection. The SEARCH-09 fold stays.
- General progressive widening (SEARCH-04). Widen only on the "every kept reply lost" trigger.
- VCN code, alpha-beta search, `game/pattern.*`, movegen, zobrist.
- `real_test`, `test_mcts.txt` in the repo root.

## Definition of done

- All tests in the todo's list pass in `rapfi_tests` with asserts live, the full suite is green, and
  the fail-first state is recorded.
- Bench `17621937 / 8c1f72e5`. Golden output with the cut off is identical to (a) and base.
- Stress run clean, with the widen count and peak retired bytes reported.
- The scan table includes base, (a), (a)+(b) and `1.0` at `0.99` / 100K, and lists every changed
  position. The 8 SEARCH-09 positions are reported individually, recovered or not. No false mate.
- Self-play of (a)+(b) vs (a) on PGO builds:
  `build/oppo4-ab/s10_fix` vs `s9_fix` (already built), with
  `OPFILE=$PWD/op_std_main.txt ./run.sh s10_fix s9_fix 1 120 tc=0/2`. Pass means no regression
  beyond about 1 SE (±24 Elo).
- Todo: results written and status updated. `TODO.md` and `instruction.md` lines moved to Done with
  the date. The VCN-03 status note now says the edge cut no longer blocks a proof, and gives the
  closed-graph caveat.
- Do not commit unless the user asks. SEARCH-09 is uncommitted too; keep both in the working tree.
