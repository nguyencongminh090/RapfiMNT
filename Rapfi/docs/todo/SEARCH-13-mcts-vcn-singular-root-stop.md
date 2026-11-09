# SEARCH-13 — MCTS singular-root stop blocks VCN proofs at a forced root

**Status:** open (recorded 2026-11-09, user). Found in VCN-03 slice 5 (§9 "Slice 5", finding).

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
