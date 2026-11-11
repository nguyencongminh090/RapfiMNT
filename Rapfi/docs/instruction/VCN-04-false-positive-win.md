# VCN-04 — execution guidance (option G: the MCTS pass is a test, not an edge)

Task: [`../todo/VCN-04-false-positive-win.md`](../todo/VCN-04-false-positive-win.md), section
"Proposed comprehensive design". Class B (search behaviour, MCTS under VCN only). Approved by the
user 2026-11-11: "Do both stages". Written 2026-11-11 from the todo and a read of
`search/mcts/search.cpp`, `search/mcts/searcher.h`, `search/mcts/node.cpp`, `search/vcn.h`,
`game/wincheck.h`, `search/movepick.cpp`.

## Goal

Under the MCTS searcher in VCN mode, no graph node is reached by a pass. The defender's pass is
replaced by a test run when a defender leaf is first evaluated: "after a null move, does the
attacker win at level + 1?". Result: no leaf is valued with an extra attacker stone, and a closed
graph (SEARCH-03) has no pass edge to lock on (the measured cause of C8 98.2 %, see the todo).

## Model split

| Step | Model | Why |
|---|---|---|
| Test list, tests, stage 1 and stage 2 code | session model (default) | Search semantics and proof soundness |
| Verification runs (unit tests, bench, fixed-node table, Part C, long run) | same agent | Scripted, harness exists |

## Current state (read before starting)

- `search/mcts/search.cpp`
  - `generateNodeMoves` (about line 539): adds `Pos::PASS` for the VCN defender and calls
    `applyVcnPassPrior`. Used by `expandNode<false>` and `widenNode`.
  - `evaluateNode` (about line 639): VCN leaf rules 1-6. Rule 4/5 are the exact VC4 rules; rule 6
    runs `SimpleVCF::vcf` under VCN rules, then the network.
  - `SimpleVCF` (line 37-325): `vcf(rule, board, ply, VcnContext)`; at level VC4 with the attacker
    to move it is the exact fours-only VCF (no stand-pat, whole-board generator) and returns
    `mated_in(...)` when there is no VCF.
- `search/mcts/searcher.h`: `mctsNodeVcnLevel` / `mctsNodeKey` derive the level from the
  defender's pass count since the root. With no pass edge the level of every graph node is the
  root level. Keep these functions as they are (they stay correct); do not add a Node field.
- `search/searchconfig.h`, `config.cpp`: `mctsVcnPassPrior` / `mcts_vcn_pass_prior`.
- Tests: `tests/mcts_vcn_tests.cpp` (53 tests; the "slice 3: pass edge and prior" group and the
  pass-in-PV test depend on the pass edge), `tests/vcn_tests.cpp` (AB and MovePicker; unchanged).
- AB (`search/ab/search.cpp`) keeps its pass move. Not part of this task.

## Design (hard requirements)

1. **No pass edge.** `generateNodeMoves` never yields `Pos::PASS` (pass `generatePassMove = false`
   to the picker). Remove `applyVcnPassPrior`, `SearchCfg.mctsVcnPassPrior` and its config key
   (an unknown key in an old config file must not be an error: check how `config.cpp` treats
   unknown keys and say what happens in the report).
2. **Pass test** in `evaluateNode`, for a VCN defender leaf (defender to move) with no attacker
   five, after the existing rules 1-3 (VC5, draw, `quickWinCheck`) and after the defender's own
   VCF check (a defender win stays a defender win):
   - Root level **VC4**: unchanged (rule 4 already ends the node).
   - Root level **VC3**: play a null move (`Pos::PASS`) on the board, run the exact VC4 VCF for
     the attacker (`SimpleVCF::vcf` with level VC4), undo. No VCF: the node is terminal, the
     defender wins, with the same value the old pass edge produced (the negated value of the old
     pass child; check against `evaluateNode_vcnLeaves_matchAbSignAndMateClass` and AB's value
     for the same position). VCF found: go on to the network value as before.
   - Root level **VC2**: the question is "attacker wins at VC3 after the null move", answered
     with three values: `Threat` (proven), `NoThreat` (proven), `Unknown`.
     - Stage 1, static necessary condition: after the null move, if the attacker has no cell
       whose `pattern4` is at least `K_BLOCK3` (no move that makes a three or a four), the answer
       is `NoThreat`. Reason: a VC3 win starts with a move after which the attacker has a four
       available; that four either exists now (`>= E_BLOCK4`) or is created by a move that makes
       a three on that line. Use `board.p4Count`. Otherwise go to stage 2.
     - Stage 2, bounded VCT solver (new, inside `search/mcts/search.cpp` next to `SimpleVCF`, or
       its own file): attacker to move at VC3, node budget from a new config key
       (`mcts_vcn_pass_test_nodes`, 0 = stage 1 only). Attacker node: win by `quickWinCheck` /
       VCF; otherwise for each legal move m: m is admissible only if it is a four or the exact
       VC4 VCF exists after m and a null move (pre-filter: after m the attacker has a cell
       `>= E_BLOCK4`); every legal defender reply (the MAIN picker without pass, plus the
       defender's own immediate wins and VCF) must lose. `NoThreat` only when the attacker's
       candidate set is exhausted without the budget running out (candidates must be all legal
       moves filtered by the exact test, not a pattern shortlist: a quiet move can enable a VCF).
       Budget out anywhere: `Unknown`.
     - `NoThreat`: terminal, the defender wins. `Threat` and `Unknown`: ordinary node, network
       value (lenient variant, user-approved default in the todo).
     - Cache the three-valued answer so a transposed leaf does not re-run it (the shared TT under
       the VC3 key is one option; a wrong cache hit must not turn `Unknown` into a proof).
3. **Soundness to keep.** A `+M` for the attacker must be a forced win with real defender
   replies only. The test may only end a node in the defender's favour. Never mark a node as an
   attacker win from the test alone.
4. **Value conventions.** Mate values in MCTS use `board.ply()` (absolute), as the existing rules
   do. Do not return a mate longer than `options.maxMoves` (same guard as rule 3).
5. Board state must be identical before and after the test (null move undone on every path,
   including early returns). The board in `evaluateNode` is the thread's own board.

## Test list (write into the todo before code; failing first where behaviour changes)

Minimum, named `<function>_<scenario>_<expectedBehavior>`, `HashEvaluator` as the existing tests:

- `generateNodeMoves` / `expandNode`: `expandNode_vcnDefender_noPassEdge` (VC2, VC3, VC4;
  freestyle and Renju black), `expandNode_vcnDefenderFacingOpenThree_noPassEdgeAndFullDefendList`,
  `widenNode_vcnDefenderCutNode_noPassEdgeAfterWiden`, `expandNode_vcnOff_edgesUnchanged` (keep).
- Pass test, VC3: `evaluateNode_vc3DefenderAttackerHasNoVcfAfterNull_defenderWinsTerminal`,
  `evaluateNode_vc3DefenderAttackerHasVcfAfterNull_notTerminal`,
  `evaluateNode_vc3DefenderFacingFive_noPassTest`, `evaluateNode_passTest_boardUnchanged`.
- Pass test, VC2 stage 1: `evaluateNode_vc2DefenderAttackerHasNoThreeOrFourMove_defenderWinsTerminal`,
  `evaluateNode_vc2DefenderAttackerHasThreeMove_notEndedByStaticRule`.
- Stage 2 solver (direct calls through `search/mcts/searchtesting.h`):
  `vctSolve_openThreeAttacker_threat`, `vctSolve_quietPosition_noThreat`,
  `vctSolve_budgetZero_unknownOrStage1Only`, `vctSolve_budgetExhausted_unknown`,
  `vctSolve_defenderCounterFourRefutes_noThreatOrUnknownNeverThreat`,
  `vctSolve_quietMoveEnablesVcf_isCandidate` (the exhaustive-candidate rule).
- End to end (through `YXVCN`): the existing `mctsVcn_*` mate tests keep their results
  (`mctsVcn_vc2Vc3Vc4_farVcfAttacker_mates`, the double three VC3 win / VC4 loss,
  `mctsVcn_vc3_attackerWithoutThreat_defenderPassRefutes` renamed for the test),
  `mctsVcn_graphNeverContainsPassEdge` (walk the graph after a VC2 and a VC3 search).
- Config: `config_mctsVcnPassTestNodes_parsedAndClamped`; the old `mcts_vcn_pass_prior` tests are
  removed with the option.
- Replace, do not delete silently: list in the todo every existing test that is removed or
  rewritten and why.

## Pitfalls

- `quickWinCheck` after the null move must use the attacker's level + 1 flag (`vcfOnly` at VC4).
- A defender leaf where the attacker has an open three or a four-three passes the test trivially
  (flex four / four-three is a VCF): make sure the test is cheap there (`quickWinCheck` first).
- `expandNode` `noValidMove` can now happen at a VCN defender node (for example a double three
  and no own four): it is a real loss; keep the existing terminal value logic.
- Multi-thread: `evaluateNode` runs on several threads with their own boards; the solver must
  not use static or shared mutable state except the TT.
- The MCTS root is not evaluated by these rules (`Root` skips them); root moves that fail the
  test are ended one ply below, at the defender leaf.
- Asserts are live in `rapfi_tests` only. SEARCH-12: assert builds abort at a root with a five or
  flex four; keep end-to-end tests on roots without forced moves.
- NPS: stage 2 runs a search per VC2 defender leaf. Measure it; pick the default budget from the
  numbers and record them. If no budget is worth its cost, default 0 and say so.

## Boundaries — do not touch

- `search/ab/`, `search/movepick.*`, `game/`, `eval/`: no behaviour change. Bench must stay
  bit-identical.
- `build/yixin-net/` (the user's engine, configs and weights): read and copy only.
- `tests/vcn_tests.cpp` test `quickWinCheck_fourThreeBlockIsOpponentFlexFour_noWin` stays.
- No commit, no push. Leave the work in the working tree.
- Do not start more than one engine with a large memory budget at a time (14 GB machine).

## Verification before marking this task done

All of these, run by the implementing agent, with outputs saved under `build/vcn04/`:

1. `rapfi_tests` built with asserts live: all pass; `--check-asserts-live` aborts. New tests were
   seen failing before the code change where behaviour changes (record which).
2. Bench (`echo BENCH | ./pbrain-rapfi`, gcc Release AVX512+VNNI, the `build/` tree): Nodes
   `17621937`, Hash `8c1f72e5`.
3. Release PGO build: `scripts/pgo_build.sh x64-clang-AVX512VNNI`; use
   `build/x64-clang-AVX512VNNI-pgo-use/pbrain-rapfi` with the configs and weights of
   `build/yixin-net/` (copy or symlink into `build/vcn04/net/`; harness scripts `run.sh`, `cmp.sh`,
   `long.sh`, `long30.sh` are in `build/vcn04/`, they expect to run inside the net directory).
4. Fixed-node table of the todo ("Measurement (2026-11-10)"): the three freestyle positions at
   VC2 and the first at VC3, 20000 / 30000 nodes, 1 thread, ordinary against VCN. Expected: the
   change of win rate of every root move is small (report the numbers; before: +11.7 to +23.7).
5. VCN Part C grid (`docs/instruction/VCN-03-mcts-vcn.md` §Part C, harness under
   `build/vcn03s5/` and `build/search13/`): zero wrong answers; report proven / correct counts
   against the VCN-03 slice 5 numbers (MCTS 23/27 proven).
6. The user's Renju opening (`long30.sh`: 30 min, 8 GB, 8 threads), `YXVCN 2` once and `YXVCN 3`
   once, one after the other. Expected: no root move far above its ordinary value without a
   proof (before: C8 98.2 %, ordinary F9 58.5 %, logs in `build/vcn04/logs/`); no `Pass` in any PV.
7. NPS of MCTS with VCN on at VC2 and VC3 against the old binary (`build/yixin-net/pbrain-rapfi`)
   on the same positions, and the stage 2 budget chosen from it.

Then update the todo (`Status:`, test list, results with numbers, removed tests, deviations) and
the `TODO.md` / `instruction.md` lines (move to `## Done` only if every item above passed;
otherwise leave Active with what is left).
