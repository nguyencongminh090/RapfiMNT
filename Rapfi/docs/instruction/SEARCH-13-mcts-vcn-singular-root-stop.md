# SEARCH-13 — execution guidance

Task: [`../todo/SEARCH-13-mcts-vcn-singular-root-stop.md`](../todo/SEARCH-13-mcts-vcn-singular-root-stop.md).
Class B (MCTS search behaviour, VCN mode only). Written 2026-11-09 from the todo, VCN-03 slice 5
(§9) and a read of `search/mcts/search.cpp` and `search/ab/search.cpp`.

## What to change

In `MCTSSearcher::search(SearchThread &)`, main-thread block, the stop

```cpp
if (th.rootMoves.size() == 1
    && th.engine.nodesSearched() >= SearchCfg.numNodesAfterSingularRoot)
    th.engine.stopThinking();
```

(`search/mcts/search.cpp:1488-1490`) gets `&& !options.vcnMode.enabled()`, the same exemption AB
has at `search/ab/search.cpp:462`. `options` is already in scope (`SearchOptions &options =
th.options()` at the top of the function). Add a one-line comment pointing at the AB rule and
SEARCH-13. Nothing else in production code.

## Approach

1. **Baselines from HEAD first**, outputs under `build/search13/`, not in the repo.
   - `echo BENCH | ./pbrain-rapfi` must report Nodes `17621937`, Hash `8c1f72e5`.
   - MCTS golden with VCN off: `build/vcn03s5/golden.sh BINARY TAG cfg_off` (4 Standard positions,
     1 thread, 300K nodes). Copy the script set (`golden.sh`, `run_fixed.py`, `golden_extract.sh`,
     `final_extract.sh`, `golden_positions.txt`, `cfg_off`) into `build/search13/` and run it there;
     do not write into `build/vcn03s5/`.
   - Part C forced-root cells, before: copy `build/vcn03s5/partc/` (`run_vcn.py`, `positions.py`,
     `cfg_vcn`) into `build/search13/partc/`. Run MCTS only on P1, P4, P7, P8, P9 at `YXVCN 4/3/2`,
     5 s and 30 s, 1 and 8 threads (the slice-5 grid, restricted to these cells). The binary is a
     PGO release build of HEAD (`scripts/pgo_build.sh x64-clang-AVX512VNNI`), as in slice 5. Keep
     the AB reference from the slice-5 table (`docs/todo/VCN-03-mcts-vcn.md`, "Per cell"); no AB
     rerun needed.
2. **Write the todo's two tests first and watch the first fail.** Put both in
   `tests/mcts_vcn_tests.cpp`. Since SEARCH-12 a forced root no longer aborts in `rapfi_tests`, so
   these tests may use one.
   - Use `MctsEngineFixture`, `searchOptions(board, maxNodes, level)` and `runMctsSearch` (about
     lines 222-270). Use a fixed node budget well above the stop, e.g. `maxNodes = 20000`.
   - Position: a forced root where the attack outcome is not settled within 100 nodes. P4 (counter
     four, block without four) is the slice-5 case; take its moves from
     `build/vcn03s5/partc/positions.py` / `tests/vcn_measure.sh`. P9 is the `vcn_tests.cpp`
     position `vcnSearch_vc3_forcedRootMoveWithTimeLimit_winFound`.
   - `mctsVcn_vc3_forcedRootMoveWithNodeBudget_searchesPastSingularLimit`: assert
     `rootMoves.size() == 1` (the root really is forced) and `Search::Engine.nodesSearched()` is far
     above `numNodesAfterSingularRoot + maxNumVisitsPerPlayout` (the stop is checked after each
     playout batch of up to 100 visits). Asserting "reached `maxNodes`" is also fine, unless the
     search ends earlier for another reason; check that first (see Pitfalls). Do not assert a mate
     value unless it is reproducible at that budget with the `HashEvaluator`; the node count is the
     behaviour under test.
   - `mctsSearch_vcnOff_singularRoot_stopsAfterNumNodesAfterSingularRoot`: same board, VCN off
     (`VCNLevel::None`), same budget; assert the count is at most `numNodesAfterSingularRoot +
     maxNumVisitsPerPlayout` (set both through `ScopedSearchCfg`, about line 457, if you want exact
     numbers). This pins current behaviour and passes before and after.
   - Record in the todo that the first test failed before the fix (and its node count).
3. **Implement** the one-condition change.
4. **Verify** (Definition of done). Rebuild the PGO release for the Part C after-run.
5. **Part C after-run**: the same cells, budgets and threads as step 1 with the new binary. Report
   proven / correct / wrong per budget and thread count for the 15 forced-root cells, before and
   after, with the slice-5 classification rule (VCN-03 §9 Slice 5: a cell's reference is
   `vcn_measure.sh`'s expected answer, else AB's proven class; a non-proven value is never wrong).
   Expected: P4 VC3 `-M8`, P4 VC2 `+M13` classes proven, and zero wrong. Also note the wall time
   per cell: forced roots now use the budget instead of ending after about 100 ms.

## Pitfalls

- Only the VCN case changes. With VCN off the stop must stay bit-identical (golden, bench, the
  second test).
- Other stop paths: check whether MCTS stops on a proven root (or on mate) on its own. If it does
  not, a proven forced root now runs to the full budget; that is expected for this task, not a
  bug to fix here. Record it in the todo if it turns out to cost the whole budget on proven cells.
- AB's rule also has `!options.isAnalysisMode() && !ctx.inPonder && !rmp.enabled()` and calls
  `ctx.markPonderingAvailable()`. MCTS's stop has neither. Do not add them here (out of scope, it
  would change VCN-off behaviour); record as a possible follow-up if it matters.
- MCTS is not covered by BENCH. A green bench only shows AB and shared code were not disturbed.
- 8-thread runs are not reproducible; one run each, as in slice 5. Do not draw conclusions from a
  single mate-distance difference.
- `rapfi_tests` forces `-UNDEBUG`. Never do that to the `rapfi` target.

## Don't touch

- The AB singular-root rule, `numNodesAfterSingularRoot` default, `searchconfig.h`, the VCN leaf
  rules and pass prior (VCN-03 slices 2-4), `Node::createEdges`.
- SEARCH-11, SEARCH-14.
- `build/vcn03s5/` (read and copy only; it holds the slice-5 record).
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`): read only, never edit or
  commit.

## Definition of done

- Both tests pass in `rapfi_tests` with asserts live, the full suite is green, and the first test
  failed before the fix (recorded in the todo).
- Bench `17621937 / 8c1f72e5`. MCTS golden with VCN off identical.
- Part C forced-root cells: before/after table in the todo, zero wrong after.
- Todo: status updated, results written. `TODO.md` line and this file's `instruction.md` line moved
  to `## Done` with the date and results. Optionally correct VCN-03 §3.5 ("singular-root early stop
  is fine") with a one-line pointer to SEARCH-13.
- Commits in conventional style: `test:` (failing test), `search:` (fix), `docs:` (results).
  Branch first if on `main` and the user asked for commits; otherwise leave the changes
  uncommitted and report.
