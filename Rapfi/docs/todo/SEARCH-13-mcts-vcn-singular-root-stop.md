# SEARCH-13 — MCTS singular-root stop blocks VCN proofs at a forced root

**Status:** partly done 2026-11-09 (code and tests landed, uncommitted; the expected P4 VC3/VC2 proofs did NOT
appear, see Results). Recorded 2026-11-09 by the user. Found in VCN-03 slice 5 (§9 "Slice 5", finding).

**Class:** B (MCTS search behaviour, VCN mode only).

## Problem

- `MCTSSearcher::search` stops the search once `th.rootMoves.size() == 1 &&
  nodesSearched() >= SearchCfg.numNodesAfterSingularRoot` (`search/mcts/search.cpp:1487-1489`,
  default 100, `search/searchconfig.h:73`), whatever the time budget.
- Under VCN a forced root move (for example the block of a five) says nothing about whether the
  attack still wins. In VCN-03 slice 5 the roots P1, P4, P7, P8 and P9 have one legal move. P4 VC3
  and P4 VC2 stay unproven under MCTS (`-67`, `610`), while AB proves `-M8` / `+M13`.
- AB had the same problem and was exempted for VCN in VCN-02 item 4 (`search/ab/search.cpp:462`,
  `sd.singularRoot && !options.vcnMode.enabled()`; test
  `vcnSearch_vc3_forcedRootMoveWithTimeLimit_winFound`). VCN-03 §3.5 called the MCTS stop "fine",
  which is wrong for VCN.

## Decision note (draft)

- **What:** skip the singular-root stop when `options.vcnMode.enabled()`, the same as AB. Keep the
  behaviour unchanged with VCN off.
- **Expected:** P4 VC3/VC2 proven by MCTS within the slice-5 budgets, and no wrong answer. With VCN
  off, bit-identical.
- **How measured:**
  - the slice-5 Part C harness (`build/vcn03s5/partc/`) on the forced-root cells;
  - bench;
  - the MCTS golden output with VCN off.
- Low priority: VCN-03 concluded that alpha-beta stays the VCN searcher.

## Function → test-case list (write before code)

- `mctsVcn_vc3_forcedRootMoveWithNodeBudget_searchesPastSingularLimit`
- `mctsSearch_vcnOff_singularRoot_stopsAfterNumNodesAfterSingularRoot` (current behaviour kept)

## Verification

- Bench `17621937 / 8c1f72e5`. Unit tests with asserts live. The MCTS golden with VCN off is
  identical.
- Part C forced-root cells: report proven/correct/wrong before and after.

## Results (2026-11-09)

**Change.** `search/mcts/search.cpp`, main-thread block of `MCTSSearcher::search`: the singular-root
stop is now `th.rootMoves.size() == 1 && !options.vcnMode.enabled() && nodesSearched() >=
numNodesAfterSingularRoot`, with a comment pointing at the AB rule and SEARCH-13. Nothing else in
production code. VCN off is unchanged.

**Tests** (`tests/mcts_vcn_tests.cpp`, position P4 = counter four, block without four, black to
move, one root move, `HashEvaluator`, 1 thread, 20000 nodes):
`mctsVcn_vc3_forcedRootMoveWithNodeBudget_searchesPastSingularLimit` and
`mctsSearch_vcnOff_singularRoot_stopsAfterNumNodesAfterSingularRoot`.
- Fail first: the first test FAILED at HEAD, asserts live, with **100 nodes searched** (budget 20000;
  `build/search13/failfirst.txt`). After the fix it searches 20000 nodes. The second test passes before
  and after.
- After: `rapfi_tests` 205/205 (203 + 2), `--check-asserts-live` aborts on its sentinel
  (`build/search13/asserts_live.txt`).

**Bit-identity.** Bench (gcc, `build/pbrain-rapfi`) Nodes 17621937, Hash 8c1f72e5 at HEAD and after;
clang PGO builds of HEAD and after also give 17621937 / 8c1f72e5. MCTS golden with VCN off
(`cfg_off`, 4 Standard positions, 300K nodes, 1 thread): the `.final` (last root print) files of all
four positions are identical to HEAD. The full `.extract` is identical for positions 1-3; for position 0
the intermediate prints differ, but they differ between two runs of the HEAD binary as well (periodic
prints are timing-dependent), so that is not a regression signal.

**Part C** (`build/search13/partc/`, `results_base.tsv` / `results_after.tsv`; PGO release builds of
HEAD and of the fix, real network via `cfg_vcn`, MCTS only, P1/P4/P7/P8/P9 at `YXVCN 4/3/2`, one run
each, 8-thread runs not reproducible). Classification rule of VCN-03 slice 5.

| Budget | Threads | Before: proven / correct / wrong | After: proven / correct / wrong |
|---|---|---|---|
| 5 s | 1 | 13 / 13 / 0 | 13 / 13 / 0 |
| 5 s | 8 | 13 / 13 / 0 | 13 / 13 / 0 |
| 30 s | 1 | 13 / 13 / 0 | 13 / 13 / 0 |
| 30 s | 8 | 13 / 13 / 0 | 13 / 13 / 0 |

(15 cells; 13 proven; the two unproven are P4 VC3 and P4 VC2.) **No cell changed class. The stated
expectation (P4 VC3 `-M8`, P4 VC2 `+M13` proven) was not met:** P4 VC3 went `-67 / -99 / -67 / -146`
to `-625 / -686 / -522 / -789`, P4 VC2 `610 / 491 / 610 / 446` to `730 / 742 / 778 / 747` (5 s 1 thread,
5 s 8, 30 s 1, 30 s 8), all still plain values with no mate bound, although the search now
runs 24M-950M nodes (VC2) and 93M-2144M nodes (VC3) on that root. Zero wrong answers.

Wall time per cell: before about 65-120 ms on every cell (stop at 100 nodes); after the full budget on
every cell (4.53-4.60 s at 5 s, 27.0-27.1 s at 30 s), including the 13 cells that were already proven
within the first 100 ms print. MCTS has no stop on a proven root, so a proven forced root now uses
the whole budget; expected for this task, not fixed here.

**Remaining.** The singular-root stop is no longer the blocker for P4 VC3/VC2. Why MCTS does not prove
`-M8` / `+M13` there with the real network and billions of nodes is open (not investigated: a VC3
mate of depth 8 / VC2 `+M13` through the pass-edge prior and the `Node` bounds, or the 0.99 policy
edge cut). Needs its own todo before this item can be called Done. Possible follow-ups, not done:
AB's rule also has `!isAnalysisMode() && !inPonder && !rmp.enabled()` and
`markPonderingAvailable()`; MCTS has neither; MCTS could stop on a proven root.
