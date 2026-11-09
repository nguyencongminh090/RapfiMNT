# SEARCH-12 — MCTS root with a five or flex four on the board aborts in assert builds

**Status:** done 2026-11-09 (recorded 2026-11-09, user; found during VCN-03 slice 4). Fixed in the ROOT picker, tests failed before and pass after, bench and MCTS golden unchanged.

**Class:** C (plumbing bug: a missing flag; release behaviour is unaffected).

## Problem

- The `ROOT` `MovePicker` constructor (`search/movepick.cpp`, about lines 227-241) handles forced
  roots in four branches: own five, opponent five, own flex four (`generate<WINNING>` /
  `generate<DEFEND_FIVE>` + `markFirstMoveOneHot`), and opponent flex four (`scoreAllMoves<POLICY>`).
  In the three one-hot branches it never sets `hasPolicy = true`.
- The `MAIN` picker's `DEFENDFIVE_MOVES` stage does set it, after `markFirstMoveOneHot`
  (`movepick.cpp`, `case DEFENDFIVE_MOVES`).
- `Node::createEdges` (`search/mcts/node.cpp:167`) asserts `movePicker.hasPolicyScore()`, and the
  MCTS root expansion uses the ROOT picker. So any MCTS search whose root has a five or a flex four
  on the board aborts in a build with asserts live, VCN on or off.
- In release builds the assert is compiled out. The one-hot policy is written anyway, so the edges
  are still right. Only the flag is wrong.
- **Evidence:** reproduced with an opponent-five root (VCN-03 slice 4 smoke run). The own-five and
  own-flex-four branches are inferred from code.

## Fix direction

Set `hasPolicy = true` next to each `markFirstMoveOneHot` in the ROOT constructor, as the MAIN path
does. This is in the picker (the layer that produces the wrong flag), not in `createEdges`. Check
that no alpha-beta code branches on `hasPolicyScore()` for a ROOT picker in a way that would change
AB's behaviour. If one does, record it, since the bench must stay bit-identical.

## Function → test-case list (write before code)

- `movePickerRoot_opponentFive_normalizedPolicy_hasPolicyScore`
- `movePickerRoot_ownFive_normalizedPolicy_hasPolicyScore`
- `movePickerRoot_ownFlexFour_normalizedPolicy_hasPolicyScore`
- `movePickerRoot_opponentFlexFour_normalizedPolicy_hasPolicyScore` (already set? pin it)
- `mctsSearch_rootOpponentFive_assertBuild_completesWithBlockMove`

## Verification

- Bench `17621937 / 8c1f72e5`. Unit tests with asserts live.
- MCTS golden output unchanged on non-forced roots.

## Results (2026-11-09)

Measurement files: `build/search12/` (not in the repo). gcc Release build in `build/` (asserts live only
in `rapfi_tests`), baseline from HEAD 676ac9b.

**Fix.** `search/movepick.cpp`, ROOT constructor: `hasPolicy = true` after `markFirstMoveOneHot` in the
own-five, opponent-five and own-flex-four branches, inside the existing `if (useNormalizedPolicy)`.
Nothing else in production code.

**Tests** (all five in `tests/mcts_vcn_tests.cpp`, so no `TEST_DRIVER_SOURCES` change). Positions:
closed four on column 7 (white at (7,4), five point (7,9)) with white or black to move; open three
(7,5)-(7,7) with white or black to move. Each test builds its own `Board` and asserts the position
preconditions (side to move, `p4Count`).
- `movePickerRoot_opponentFive_normalizedPolicy_hasPolicyScore`: the single block (7,9), one-hot, both flags.
- `movePickerRoot_ownFive_normalizedPolicy_hasPolicyScore`: first move (7,9), one-hot, both flags.
- `movePickerRoot_ownFlexFour_normalizedPolicy_hasPolicyScore`: one-hot, both flags.
- `movePickerRoot_opponentFlexFour_normalizedPolicy_hasPolicyScore`: pins current behaviour through
  `scoreAllMoves<POLICY>` (board bound to the stand-in evaluator, so it runs as an engine test, not forked).
- `mctsSearch_rootOpponentFive_assertBuild_completesWithBlockMove`: 2000-node MCTS search, answer (7,9).

**Failed before the fix** (`build/search12/tests_red.txt`, `RAPFI_TEST_ISOLATE=1`): the first three
reported `[FAIL]` on `Assertion r.hasPolicyScore` (each position precondition held); the
opponent-flex-four test passed as expected; the search test aborted the whole driver in
`Node::createEdges` (`node.cpp:167`, `Assertion movePicker.hasPolicyScore() failed`). Red run for the
search test: it is the last test of the driver, so the abort itself is the record; no pure companion
test was added.
**After the fix:** 203/203 pass, both plain and `RAPFI_TEST_ISOLATE=1` (`tests_fix.txt`, `tests_fix_isolate.txt`).

**Bench:** Nodes `17621937`, Hash `8c1f72e5` (gcc Release; unchanged from HEAD).

**MCTS golden:** 4 Standard positions, 1 thread, 300K nodes, edge cut off (the SEARCH-10 recipe and
position set), final root block compared base vs fix: byte-identical for all four. Periodic detail
lines are time-driven and differ run to run even on the same binary (position 0 differs on line 4 in
a raw diff for that reason; the base binary reproduces its own final block).

**AB check (`hasPolicyScore()` readers).** `search/ab/search.cpp:1079` and `:1638` read it on the MAIN
picker (constructed at `:704` and `:1451`; `:2221` is the QVCF picker). AB ROOT pickers
(`searchengine.cpp:67,98`, `command/gomocup.cpp:877`) pass the default `useNormalizedPolicy = false`, so
the new write is unreachable there. The only ROOT picker with `useNormalizedPolicy = true` is MCTS
`expandNode<true>` (`mcts/search.cpp:596`). The other readers (`mcts/node.cpp:167`,
`mcts/search.cpp:559,574`) are asserts. No AB behaviour changes; bench confirms.

**YXVCN smoke, assert build.** Separate build `build/search12/assert` (`-DFORCE_ASSERTION=ON`, Release,
never used for the bench), MCTS config `build/search12/cfg_off`, one thread, 3000 nodes, opponent-five
root (the position above). Input in `build/search12/smoke_in_yxvcn.txt` (`YXBOARD`, the 7 moves, `DONE`,
`YXVCN 3`), run from `cfg_off`:
`(cat ../smoke_in_yxvcn.txt; sleep 8; echo END) | ../assert/pbrain-rapfi`
Output: `multipv 1 ev -M4 ... pv H10 Pass` then `7,9` (the block, no abort). The ordinary turn
(`smoke_in_turn.txt`, `BOARD ... DONE`) also answers `7,9`.

**Follow-up (not done here).** The smoke test `mctsVcn_smokeVsAb_noMateAbRefutes` and the VCN-03
golden set still avoid forced roots because of this bug; they could now be widened to forced roots.
**Out of scope, not reproduced.** A ROOT picker without an evaluator would hit the same assert on
ordinary roots (`scoreAllMoves<POLICY>` sets `hasPolicy` only with an evaluator). The new tests did not
reveal it (the opponent-flex-four test binds an evaluator), so no new TODO item was opened.
