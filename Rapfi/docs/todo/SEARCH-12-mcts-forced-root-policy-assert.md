# SEARCH-12 — MCTS root with a five or flex four on the board aborts in assert builds

**Status:** open (recorded 2026-11-09, user). Found during VCN-03 slice 4.

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
