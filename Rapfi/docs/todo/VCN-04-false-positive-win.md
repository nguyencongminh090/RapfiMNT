# VCN-04 — VCN reports a near-certain attacker win while the defender still has a defence

**Status:** done 2026-11-11 (see "Closed 2026-11-11" at the end; the text that follows is the earlier in-progress status): option G (the MCTS pass is a test, not an edge), stages 1
and 2, approved by the user 2026-11-11. Recorded 2026-11-10 by the user; cause measured 2026-11-10
(closed graph x pass edge, §Measurement). See §Test list and §Implementation record (option G).

**Class:** B (search behaviour, VCN mode only), provisional. Re-classify once the cause is known.

## Problem

The user observed a VCN search in which the attacker claims about 99 % winning rate (reported as a
"false positive mate") although the defender still has a playable defence.

Example: **to be supplied by the user** (position, searcher AB / MCTS, level VC2 / VC3 / VC4,
the exact output line — proven mate `+M` or a win-rate / eval value — and the defence the user
believes still holds).

## Static review (2026-11-10, code reading only, nothing run)

### No false proven-mate path found

- AB Step 12 pruning (move count, trivial, policy) runs only once `bestValue > VALUE_MATED_IN_MAX_PLY`
  and never skips `Pos::PASS` (`search/ab/search.cpp` Step 12).
- AB null move is off under VCN; razoring and futility return evals, not attacker mates.
- `vcfdefend` (AB and MCTS `SimpleVCF`): the single forced block is the only legal defence to a
  five threat.
- `MovePicker` `DEFENDFOUR` / `DEFENDB4F3` stages: blocks plus own fours, complete for an open three
  and a four-three.
- `quickWinCheck`: double three excluded at VC4 (`vcfOnly`); at VC2/VC3 it is a real win (a pass
  only gives the attacker a tempo).
- MCTS proof bound: a cut node is not proven lost until widened (SEARCH-09/10,
  `search/mcts/node.cpp` `updateStats`).
- Not read: `classifyMove` with `move == PASS`, AB `vcfsearch` QVCF generation for the defender.

### Mechanisms that give a high non-mate value with a defence still available (suspects)

1. **MCTS edge cut at VCN defender nodes.** VCN keeps `mcts_edge_policy_mass = 0.99`;
   `generateNodeMoves` gives PASS prior 0.25 and scales board moves by 0.75, so a defence with raw
   policy about 1 % or less (far block, counter-four) can be cut. Q averages only kept children.
   SEARCH-10 widens only when every kept reply is *proven* lost (`node.cpp` `updateStats`), so kept
   replies at about 99 % but unproven never bring the cut defence back.
2. **Leaf value after a defender pass at VC2 → VC3.** VC3 → VC4 is solved exactly (MCTS
   `evaluateNode` step 5, AB `vcfsearch` vcfOnly), but VC2/VC3 leaves take the real-game network
   value (MCTS) or static eval (AB), where the attacker has an extra tempo and the level restriction
   is not modelled. The pass, the defender's main refutation, looks bad; in MCTS the 0.25 prior also
   spends many visits on it, pulling the defender node's Q towards the attacker.
3. **AB defender at the horizon / under razoring.** `depth <= 0` without `oppo5`, and Step 7
   razoring, go to `vcfsearch(defender)`: no pass stage, the defender's stand-pat eval is returned.

## Static review 2 (2026-11-10, full reading of the VCN paths, one hypothesis tested)

Read: `search/ab/search.cpp` (`search`, `vcfsearch`, `vcfdefend`, `classifyMove`), `search/vcn.h`,
`game/wincheck.h`, `search/movepick.cpp` (MAIN / QVCF stages), `game/movegen.cpp` (four and
four-three defence), `search/mcts/search.cpp` (`SimpleVCF`, `generateNodeMoves`, `expandNode`,
`evaluateNode`, `widenNode`, `searchNode`), `search/mcts/node.cpp` (`updateStats`, edge cut).

### Why a proven attacker mate (`+M`) is sound

A `+M` for the attacker needs, at every defender node on the way, that every legal reply loses,
and at every attacker node one reply that wins. The argument, by part:

1. **Level rule.** Only a defender pass raises the level (`Vcn::childLevel`); AB carries it per ply,
   MCTS derives it from the defender's pass count. The value of (stones, side to move, level) does
   not depend on the path, and both searchers key their tables by it (`Vcn::hashXor`; the zobrist
   key includes the side to move).
2. **VC4 / VC5 are exact.** A non-root VC4 attacker always goes to the fours-only VCF with no
   stand-pat and the whole-board four generator (start value `mated_in(ply + 2)`); a VC4 defender
   who faces no five wins by passing; a VC5 attacker without a five loses. A missed VCF can only
   turn a win into a loss, never the reverse.
3. **Defender move lists are complete under VCN.** The restricted lists (`DEFENDFIVE`, `DEFENDFOUR`,
   `DEFENDB4F3`) are justified by threats made of fours only (five; open three to straight four;
   four-three), so they hold at every level, and the pass is added in every stage except against a
   five (a pass there loses at once). A move outside the candidate range is worth no more than a
   pass. `findFourDefence` only lists the defences of the last three, but any move that saves the
   defender must defend that three, so it is in the list.
4. **Static wins (`quickWinCheck`).** Flex four and four-three are made of fours: valid at VC2..VC4.
   Double three needs one quiet move: valid at VC2/VC3 (a pass gives the attacker a straight four
   at the next level), excluded at VC4 (`vcfOnly`). Two or more opponent fives is a loss with or
   without a pass.
5. **AB pruning cannot make a mate.** Step 12 pruning starts only after one non-losing move
   (`bestValue > VALUE_MATED_IN_MAX_PLY`) and never drops the pass; razoring, futility and the
   depth <= 0 dive of a defender node return an eval or a stand-pat value, never a loss for the
   defender; null move is off. `classifyMove` with a pass is safe (`Pos::distance` returns -1).
   A VC4 root move that is razored one ply below is re-searched as a PV node, where the pass
   refutes it.
6. **MCTS bounds.** A node is proven lost only when every generated edge is visited and proven; an
   unvisited edge or a cut edge array keeps `upper = +inf` (`updateStats`), and a cut node whose
   kept replies are all proven lost is widened (SEARCH-10). Leaf terminals come from the same VCF
   and `quickWinCheck` rules as AB.

Conclusion: no path gives a false `+M`. What the user saw is an **unproven** high value.

### Hypothesis tested and refuted: four-three whose block is the opponent's flex four

`quickWinCheck` takes a shortcut for a four-three: if the opponent has no block-four square
(`C + D + E` counts, `B_FLEX4` not counted) it returns `mate_in(ply + 5)` without checking the
forced block. If the block square is the opponent's flex-four square, the block makes a straight
four and the "winner" loses. Test `quickWinCheck_fourThreeBlockIsOpponentFlexFour_noWin`
(`tests/vcn_tests.cpp`) builds that position: the result is `VALUE_ZERO`, correct. Reason: a
flex-four square always comes with block-four squares on the same line (the ends of the same
three), so the shortcut is not taken and the exact per-candidate check (`defendP4 >= E_BLOCK4`,
which includes `B_FLEX4`) runs. The shortcut would only misfire if every such end square were
itself of class `B_FLEX4` through another line; no such position was built. The test stays as a
guard. `./build/rapfi_tests`: 242/242 pass, asserts live. Engine sources untouched, so bench was
not re-run.

### Where the unproven high value comes from (ranked by reasoning, not measured)

1. **Leaf values do not know the level (VC2, MCTS most).** After a defender pass at VC2 the
   attacker node is at VC3 and is not solved exactly; its leaves take the real-game network value
   of a position where the attacker has one more stone than the defender, which is close to a
   win for the attacker in almost any fighting position. So the pass, the defender's main
   refutation, starts near -1 for the defender and stays there until the VC3 subtree below it is
   proven, which needs a full VCT-size search. The defender's board replies are valued as in the
   real game. If the attacker is ahead in the real game, every child of the defender node is high
   for the attacker and the root shows about 99 % with no proof. At VC3 this heals fast: one more
   pass reaches VC4, which is exact.
2. **Edge cut at defender nodes (MCTS).** With the pass at prior 0.25 and mass 0.99, board moves
   are kept up to 98.7 % of the raw policy (0.74 / 0.75), minimum 8 edges. A defence in the last
   1.3 % of the policy is not in the tree. It comes back only when every kept reply is *proven*
   lost. That happens quickly when the threat is a VCF (leaf VCF proves each reply), but not when
   the threat is a longer VCT: the kept replies sit at about -0.95 unproven and the defence is
   never searched. VCN is meant for positions the network misreads, so a defence the policy
   misses is the expected case, not a rare one.
3. **AB horizon.** A defender node at depth <= 0 (or razored) returns the stand-pat eval; the pass
   is not tried. At VC3 the attacker's last quiet move is therefore scored by eval, although a
   pass followed by the exact VC4 test would refute it whenever the move carries no VCF threat.

### Fix options (not implemented; user decision; class B, each needs its decision note)

- **A. Exact pass test at VC3 defender leaves.** At a VC3 defender leaf with no attacker five
  (MCTS `evaluateNode` step 6; AB `depth <= 0` and razoring for a VC3 defender): play the pass, run
  the fours-only VCF for the attacker at VC4; if it finds no VCF the defender has won (terminal).
  Sound (the pass is legal, VC4 is exact). Cost: one VCF per VC3 defender leaf. Removes source 3
  and the VC3 part of source 1; does not help VC2 leaves.
- **B. No edge cut at VCN defender nodes** (mass 1.0 there), or widen on a value threshold
  instead of only on proof. Removes source 2. Cost: memory and visits at defender nodes.
- **C. Report only.** Keep the search, state in the output/doc that a non-mate VCN value is not a
  win claim (VCN-02 already says: read the value as `+M` or "no win found").
- Knowledge lookup (rule 10): `SKILLS_TREE/tools/find -k knowledge` for MCTS proof search /
  widening / leaf value returned no matching note (2026-11-10).

## Measurement (2026-11-10): the inflation is reproduced and comes from the pass edge at VC2

User correction (2026-11-10): a false mate may not occur; the real symptom is a move worth
30-35 % in an ordinary search that VCN shows at about 90 %.

Setup: `build/yixin-net/pbrain-rapfi` with `config_mcts.toml` (mix9svq network, MCTS), freestyle,
1 thread, `INFO MAX_NODE` 20000 or 30000. Per root move, win rate `w` of `YXNBEST 1` (ordinary)
against `YXVCN n`. Variants change one config key. Harness kept outside the repo (scratchpad).

| Position (side to move = attacker) | Level | Variant | Largest change of a root move's win rate |
|---|---|---|---|
| `7,7 8,8 8,6 6,8 9,7` | VC2 | default (`mcts_vcn_pass_prior` 0.25) | +23.7 points (3.8 % to 27.5 %); best move 16.5 % to 26.0 % |
| same | VC2 | `mcts_vcn_pass_prior = 0.02` | +2.3 |
| same | VC2 | `mcts_vcn_pass_prior = 0` | 0.0 for every move (identical to the ordinary search) |
| same | VC2 | `mcts_edge_policy_mass = 1.0` | +22.6 (no effect) |
| same | VC3 | default | none: values fall, most moves `-M4` |
| `7,7 6,6 8,8 9,9 8,6` | VC2 | default / prior 0.02 | +11.7 / +0.5 |
| `7,7 8,8 7,9 7,8 6,8 8,6 8,7` | VC2 | default / prior 0.02 | +22.0 / +0.9 |

Reading:

- Source 1 is confirmed and is the whole effect on these positions: the inflation exists only at
  VC2, scales with the pass prior and vanishes at prior 0. The pass child at VC2 is a VC3 attacker
  node valued by the real-game network with one extra attacker stone, and with prior 0.25 the pass
  is one of the first edges visited at every defender node, so every lightly visited defender
  node averages that value in.
- Source 2 (edge cut) is not the cause here: mass 1.0 changes nothing.
- VC3 is not affected: its pass child is the exact VC4 test.
- With prior 0 the VC2 search equals the ordinary search on these positions: the pass is never
  visited. So at VC2 the pass currently adds bias, not information, until a subtree is proven.
- Not measured: a position at 30-35 % going to 90 % (these start at 4-16 %), AB, more nodes.

### User's example (2026-11-10): Renju `h8i7j10h9k7i9i10k10j9k8`, black to move

User report: MCTS VCN returns `i5` at 60 %+ (real win rate below 30 %) on the user's machine; a
much stronger machine returns `N7` at 95 % (real about 5 %). Needs more than 10 minutes. Level not
stated. User decision the same day: do not implement any fix option now.

Run here: 12 minutes, 2 threads and `MAX_MEMORY` 2 GB per run, four runs in parallel, same build
and network as above. Moves entered as `x = letter, y = 15 - number` (the engine's `flipY_X`).

| Run | Best move, win rate | Other top moves |
|---|---|---|
| ordinary (`YXNBEST 1`) | F9 53.8 % | E8 47.9, K7 47.0, J7 46.9 |
| `YXVCN 2`, default | F9 63.6 % | D9 59.1, K7 53.9 |
| `YXVCN 2`, `mcts_vcn_pass_prior = 0.02` | F9 55.7 % | J8 55.5, J7 54.4 |
| `YXVCN 3`, default | all lost: 0.1-0.2 %, most `-M4` / `-M8` | E6 `-743`, H6 `-746` unproven |

- VC2 raises the best move by about 10 points and D9 to 59 %; the low pass prior removes most of
  it. Same direction as the table above, smaller size.
- VC3 shows no inflation.
- **Not reproduced:** `i5` at 60 % and `N7` at 95 %. Neither move is in the ten printed root
  moves of any run. Limits of this run: the 2 GB graph budget filled at about 13M nodes (after
  that the graph is closed and visits add no new nodes), only the top ten moves are printed, and
  the user's level, config and coordinate convention are not confirmed.

### 30-minute runs, 8 GB, 8 threads (2026-11-10, user request): reproduced, cause found

Same opening, one run at a time. Both searches stopped at 1129 s (time management).

| Run | Result | Other top moves |
|---|---|---|
| `YXVCN 2`, default | **C8 98.2 %**, 21.8 of 22.1 billion visits, seldepth 0, PV `C8 Pass`, prior 0.00 | F9 62.5, D8 52.9, K7 49.9 |
| ordinary (`YXNBEST 1`) | F9 58.5 % | J7 47.7, D8 45.0, K7 44.4; C8 not in the top ten |

Timeline of the VC2 run (`multipv 1`):

| Time | Best | Win rate | Nodes | Note |
|---|---|---|---|---|
| 150-156 s | F9 | 53.8 % | 65-67M | graph open, about 0.4M nodes/s; no inflation |
| 159-177 s | F9 | 54.7 to 62.5 % | 83M to 326M | node rate jumps to about 13M/s: the graph budget is full and the graph is closed (SEARCH-03) |
| 180 s to end | C8 | 98.2 % | 393M to 22114M | PV `C8 Pass`, seldepth 0 |

The 12-minute VC2 run above shows the same step when its 2 GB filled: 59.4 % at 233 s (13M nodes),
64.3 % at 293 s.

**Cause: closed graph x the defender's pass edge.**

- A closed graph makes no new node and offers no unexplored edge (`selectChild`), so a playout can
  only follow edges explored before the budget filled.
- At a VCN defender node the pass has prior 0.25, so it is the first or second edge explored. A
  defender node that was expanded shortly before the budget filled has the pass as its only
  explored reply. The pass child is a VC3 attacker leaf with the real-game network value of a
  position where the attacker moves twice: about 98 % for the attacker.
- From then on that defender node's value is the pass child's value and can never be corrected:
  the defender is not allowed to try any other reply. C8 is a far, quiet root move (prior 0.00,
  ordinary value below the top ten); its child is such a node, so C8 reads 98 % and the root PUCT
  sends every remaining visit there.
- The same thing at every half-expanded defender node of the tree explains the smaller step of
  F9 (53.8 to 62.5 %).
- This matches the user's reports: it needs a long run (time to fill the memory), a stronger
  machine fills it sooner and locks harder (`N7` 95 %: another far move), and the move shown is
  one that is poor in an ordinary search.
- Before the budget fills this position shows no inflation at VC2 (53.8 % against 55-57 % for
  the ordinary search at the same time). The fixed-node tables above (20-30K nodes) do show an
  open-graph inflation on other positions, so both effects exist; on the user's example the
  closed graph is the large one.
- A short run with a 400 MB budget (120 s) did not lock on a move: the lock needs a defender node
  caught with only its pass explored under a root move the PUCT still reaches. It is likely on
  long runs, not certain on every run.

Fix options added (not implemented, user decision 2026-11-10: no fix now):

- **F. VCN: end the search when the graph closes** (or keep the closed-graph playouts but never
  through a defender node whose explored replies are only the pass). Removes the lock at its
  source.
- **D** (pass prior by level) also removes it at VC2: with a small prior the pass is no longer
  the first edge explored, so a half-expanded defender node holds a real reply.
- Workaround without code: give the search more memory, or read the VCN result before the node
  rate jumps (the jump marks the full budget); `mcts_vcn_pass_prior = 0.02` in `[search]`.

Fix option added by this measurement:

- **D. Pass prior by level.** Keep `mcts_vcn_pass_prior` where the pass child is exact (child
  level VC4), use a small prior (new key, e.g. 0.02, or 0) where it is network-valued (VC2 to
  VC3). The attacker's `+M` proof is unchanged (every edge, the pass included, must still be
  proven). Cost: the defender finds a pass refutation at VC2 later.
- **E. Value the VC2 pass child without the extra tempo** (start from the negated value of the
  defender node, as AB's null move does with `-(ss - 1)->staticEval`). Only corrects the first
  ply of the pass subtree; deeper leaves still carry the extra stone.

## Proposed comprehensive design (2026-11-11, user request; approved the same day: "Do both stages")

Execution guidance: [`../instruction/VCN-04-false-positive-win.md`](../instruction/VCN-04-false-positive-win.md).
Correction to stage 1 below: the static condition is "a move that makes a three (open or blocked)
or a four", not only an open three; see the instruction file for the reason.

User direction: propose one solution that removes the cause, e.g. make the pass not a valid move.

**G. MCTS: the pass is a test, not an edge.** Scope: `search/mcts` only; AB keeps its pass move
(it searches the pass by recursion and has no network leaf with an extra tempo). Class B.

Equivalence used: "the defender refutes move m by passing" is the same statement as "m carries no
threat at the next level". So the pass can move from the defender's edge list to a validity test
of the attacker's move, answered by a solver instead of by the network.

- **No pass edge** in the MCTS graph at any level (`generateNodeMoves` never yields `Pos::PASS`;
  `mcts_vcn_pass_prior` goes away). Every node of the graph is then a position with alternating
  moves, so no leaf is valued with an extra attacker stone, and the closed graph has no pass to
  lock on. The level is constant in the graph (the root level).
- **Pass test at a defender leaf** (`evaluateNode`, after the existing terminal rules, when the
  attacker has no five): play a null move, ask "does the attacker win at level + 1?".
  - Root level VC3: the question is the fours-only VCF (exact, already written:
    `SimpleVCF::vcf` at VC4). No VCF: the node is terminal, the attacker's move loses. VCF found:
    ordinary node, the defender must answer with a real move.
  - Root level VC4: unchanged (already terminal rules, no pass edge is ever needed).
  - Root level VC2: the question is a VC3 (VCT) win, which is a search. Three answers: proven
    threat (ordinary node), proven no threat (terminal loss for the attacker), unknown within the
    budget (ordinary node, valued by the network on a legal position).
- **Soundness.** With no pass edge every `+M` is a forced win of the real game. The test only
  removes attacker moves, so it cannot create a win. At VC3 the level meaning is exact. At VC2
  an "unknown" answer lets a non-threat move stay, so a VC2 `+M` is always a real win but may
  contain a move that a full VC2 definition would reject; a strict variant treats "unknown" as
  no threat and can miss wins instead.
- **Stages.** 1: remove the pass edge, exact test for VC3, static necessary condition for VC2
  (after the null move the attacker has a move that makes a four or an open three; otherwise no
  threat), unknown otherwise. 2: bounded VCT solver for the VC2 test (the AB VCN search at VC3
  with a node budget, result cached in the TT under the VC3 key), if stage 1 is not selective
  enough.
- **Cost.** One VCF per new defender leaf at VC3 (the pass edge already paid one per defender
  node that visited the pass). Stage 2 adds a bounded search per new defender leaf at VC2.
- **Verification.** User's Renju opening, 30 min / 8 GB at VC2 and VC3: no root move above its
  ordinary value without a proof. Fixed-node table above: change of win rate about 0. VCN Part C
  grid: zero wrong answers. Existing pass-edge tests (VCN-03 slice 3) are replaced by tests of
  the pass test; AB tests and bench `17621937 / 8c1f72e5` unchanged (AB untouched).
- Options A, D, E, F above become unnecessary if G is taken (A is the VC3 part of G).

## Plan (original, needs an example; kept for reference)

1. Reproduce: `YXVCN n` with AB and with MCTS (`tests/vcn_measure.sh`).
2. Discriminate: MCTS again with `mcts_edge_policy_mass 1.0`. If the value drops, suspect 1.
   If both searchers stay high at VC2, suspects 2 / 3.
3. Write the failing test first (`tests/vcn` tests), then decide the fix with the user.
4. Before proposing a fix design, look up a knowledge note (CLAUDE.md rule 10).

## Test list

Written 2026-11-11, before any code of option G (rule 2). Tests live in `tests/mcts_vcn_tests.cpp`;
internals are reached through `search/mcts/searchtesting.h`. Stand-in evaluator `HashEvaluator`.
"FF" marks a test that must fail before the code change (behaviour changes); the others pin
behaviour that must not change.

### `generateNodeMoves` (shared by `expandNode<false>` and `widenNode`)

| Test | Scenario | Expected |
|---|---|---|
| `expandNode_vcnDefender_noPassEdge` (FF) | Freestyle, defender to move, no attacker five; root level VC2, VC3, VC4; both stand-in policies (sorted and unsorted tail) | No `Pos::PASS`; moves, order and policies equal to the VCN-off generation |
| `expandNode_vcnDefenderRenjuBlack_noPassEdgeAndNoForbiddenMove` (FF) | Renju, black defends, a forbidden 3x3 point exists | No pass, no forbidden move, equal to the VCN-off generation |
| `expandNode_vcnDefenderFacingOpenThree_noPassEdgeAndFullDefendList` (FF) | Attacker has an open three (picker stage DEFENDFOUR) | No pass; list equal to the VCN-off list |
| `expandNode_vcnDefenderFacingFive_noPassEdge` (kept) | Attacker has a four | No pass; list equal to the ordinary one |
| `expandNode_vcnAttacker_noPassEdge` (kept) | Attacker to move | No pass; ordinary list |
| `expandNode_vcnOff_edgesUnchanged` (kept) | VCN off | Unchanged |
| `widenNode_vcnDefenderCutNode_noPassEdgeAfterWiden` (FF) | Cut defender node, every kept reply proven lost, widen | Old and widened arrays hold no pass; the node proves lost once the added replies lose |

### `evaluateNode`: pass test at a VCN defender leaf

| Test | Scenario | Expected |
|---|---|---|
| `evaluateNode_vc3DefenderAttackerHasNoVcfAfterNull_defenderWinsTerminal` (FF) | VC3, attacker made a quiet move, defender to move | Terminal, value `mate_in(ply + 3)` = negated VC4 VCF value after the null move (the value of the old pass child, negated) |
| `evaluateNode_vc3DefenderAttackerHasVcfAfterNull_notTerminal` | VC3, attacker has an open three, defender to move | Not terminal, utility = network value |
| `evaluateNode_vc3DefenderFacingFive_noPassTest` | VC2 and VC3 (one test, both levels), attacker has a four, defender to move | Not ended by the pass test (same result as VCN off) |
| `evaluateNode_vc2DefenderHasOwnFlexFour_defenderWinStaysFromQuickWin` | VC2 defender (to move) with an open three of its own, budget 0 | Terminal defender win with the value of rule 3, `mate_in(ply + 3)` (not the pass-test value) |
| `evaluateNode_vc3AttackerToMove_noPassTest` | VC3 attacker leaf, quiet | Not terminal (the test is for defender leaves only) |
| `evaluateNode_vc3PassTestMateBeyondMaxMoves_draw` (FF) | VC3 defender leaf, `maxMoves` one move away | Terminal draw value, not a mate |
| `evaluateNode_vc3DefenderPassTest_agreesWithAbRootLoss` (FF) | Quiet root at VC3: AB's root value is a loss; every MCTS defender leaf below a sample of root moves | Terminal defender win (same class as AB) |
| `evaluateNode_vc2DefenderAttackerHasNoThreeOrFourMove_defenderWinsTerminal` (FF) | VC2, attacker has no cell `>= K_BLOCK3`, budget 0 | Terminal, `mate_in(ply + 5)` |
| `evaluateNode_vc2DefenderAttackerHasThreeMove_notEndedByStaticRule` | VC2, attacker has two in a row, budget 0 | Not terminal, network value |
| `evaluateNode_vc2DefenderSolverProvesNoThreat_defenderWinsTerminal` (FF) | VC2, attacker has two in a row, budget large | Terminal, `mate_in(ply + 5)` |
| `evaluateNode_vc2DefenderSolverThreatOrUnknown_notTerminal` | VC2, attacker has an open three (threat) / tiny budget (unknown) | Not terminal, network value |
| `evaluateNode_passTest_boardUnchanged` | VC3 terminal, VC3 non-terminal, VC2 static, VC2 solver (proof and budget out) | zobrist key, ply, side to move, pass counts, `p4Count` identical before and after |
| `evaluateNode_vc4DefenderNoFiveThreat_defenderWins`, `evaluateNode_vc3_unchangedFromOrdinaryRules` (attacker leaves and five-facing defender), the VC4 / VC5 tests, `evaluateNode_vcnOff_unchanged`, `evaluateNode_vcnLeaves_matchAbSignAndMateClass` (kept) | as before | unchanged |

### Static rule and solver (direct calls)

| Test | Scenario | Expected |
|---|---|---|
| `vcnPassTestStatic_noThreeOrFourCell_noThreat` (FF) | Isolated attacker stones | `NoThreat` |
| `vcnPassTestStatic_threeMakingCell_unknown` | Two in a row | `Unknown` |
| `vcnPassTestStatic_renjuBlackAttackerForbidCell_unknown` | Renju, black attacker, a `FORBID` cell | `Unknown` (a pattern-forbidden cell can be a legal three) |
| `vctSolve_openThreeAttacker_threat` (FF) | Attacker to move with an open three | `Threat` |
| `vctSolve_doubleThreeAttacker_threat` (FF) | Double three, defender has no four | `Threat` |
| `vctSolve_quietPosition_noThreat` (FF) | One attacker stone | `NoThreat` |
| `vctSolve_twoInRow_noThreatWithinBudget` (FF) | Two in a row, large budget | `NoThreat` |
| `vctSolve_budgetZero_unknown` | Budget 0 | `Unknown`, zero work units |
| `vctSolve_budgetExhausted_unknown` | Two in a row, budget 3 | `Unknown`, nodes used <= budget + 1 |
| `vctSolve_defenderCounterFourRefutes_noThreatOrUnknownNeverThreat` | Double three whose refutation is a defender four that blocks one three with tempo; control position without the defender's three is `Threat` | Not `Threat` |
| `vctSolve_quietMoveEnablesVcf_isCandidate` (FF) | A move of class below `K_BLOCK3` that makes the VC4 VCF exist | The move is in the admissible list; result is not `NoThreat` |
| `vctAdmissibleMoves_fourAndVcfThreatOnly` (FF) | Two in a row plus a far stone | Three-making moves on the pair are admissible; a far quiet move is not |
| `vctSolve_attackerMustBlockFive_onlyBlockConsidered` (FF) | Defender has a four, attacker to move | Admissible list is a subset of {block}; result not `Threat` unless the block wins |
| `vctSolve_provenResultCached_secondCallUsesNoNodes` (FF) | Same position twice | Same result, second call uses 0 nodes |
| `vctSolve_unknownNotCachedAsProof_largerBudgetStillProves` (FF) | Budget 3 then large | `Unknown`, then `NoThreat` |
| `vctSolve_foreignTtEntryUnderCacheKey_ignored` (FF) | A VCF-shaped TT entry planted under the cache key | The answer is computed, not read |
| `vctSolve_boardUnchanged` | Proof and budget-out runs | Board identical |
| `vctSolve_renjuBlackAttacker_forbiddenMoveNotCandidate` (FF) | Renju, black attacker, a real forbidden point that would make a three | Not in the admissible list |

### Config

| Test | Scenario | Expected |
|---|---|---|
| `config_mctsVcnPassTestNodes_parsedAndClamped` (FF) | Default, 0, positive, negative, above the cap | Default as documented; negative becomes 0; values above the cap are clamped |
| `config_oldMctsVcnPassPriorKey_ignoredWithoutError` | `[search] mcts_vcn_pass_prior = 0.25` | Loads, no error (unknown keys are ignored) |

### End to end (`YXVCN` through the protocol, MCTS, fixed nodes)

| Test | Scenario | Expected |
|---|---|---|
| `mctsVcn_graphNeverContainsPassEdge` (FF) | VC2 and VC3 searches, walk the graph | No edge holds `Pos::PASS`; every node key is the root-level key of its board |
| `mctsVcn_vc3_attackerWithoutThreat_losesByPassTestNoPassInPv` (FF) | Quiet root, VC3 | Root value is a loss; no `Pos::PASS` in any root move's PV |
| `mctsVcn_vc2_quietRoot_noPassInPvAndNoProvenWin` (FF) | Quiet root, VC2 | No pass in any PV; no proven attacker win |
| `mctsVcn_vc2Vc3Vc4_farVcfAttacker_mates`, `mctsVcn_doubleThreeAfterForcedPair_vc3WinsVc4Loses`, `mctsVcn_vc4_attackerWithoutVcf_loses`, `yxvcn_*`, `mctsVcn_smokeVsAb_noMateAbRefutes` (kept) | as before | unchanged results |

Budget unit in the solver tests: one work unit is one stone placed by the solver or one node of a
VCF search it starts (see §Implementation record (option G), deviations).

Corrections to this list after the code was written (2026-11-11): the row
`evaluateNode_vc3DefenderHasOwnVcf_defenderWinStaysFromVcf` became
`evaluateNode_vc2DefenderHasOwnFlexFour_defenderWinStaysFromQuickWin` (the defender's own win is
decided by rule 3, `quickWinCheck`, before any VCF); `evaluateNode_vc2DefenderFacingFive_noPassTest`
is folded into `evaluateNode_vc3DefenderFacingFive_noPassTest`, which loops over VC2 and VC3; the
row for a Renju black attacker with a `FORBID` cell at `evaluateNode` level had no fixture and is
covered by `vcnPassTestStatic_renjuBlackAttackerForbidCell_unknown`.

Existing tests removed or rewritten: listed with reasons in §Implementation record (option G).

## Verification criteria

- The example no longer reports a near-certain attacker win (or the report is explained as
  intended "attack chance").
- Bench `17621937 / 8c1f72e5` unchanged unless the change is meant to move it.
- VCN Part C grid: zero wrong answers kept.

## Paused 2026-11-11 (option G, work stopped before verification was complete)

Status stays Active. Code and tests are in the working tree, uncommitted. Not every verification
item of `docs/instruction/VCN-04-false-positive-win.md` has been run; nothing is to be moved to
Done yet. Outputs are under `build/vcn04/`.

### Verification items

| # | Item | State | Output | Numbers |
|---|---|---|---|---|
| 1 | Unit tests, asserts live | done | `tests_after.txt`, `asserts_live.txt`, `tests_failing_first.txt` | 271/271 pass; `--check-asserts-live` aborts (exit 134); failing first against the old engine: 24 aborted + 2 failed, 245/247 others passed |
| 2 | Bench gcc | done | `bench_gcc.txt` | Nodes 17621937, Hash 8c1f72e5 |
| 3 | PGO build | done | `pgo_build.log`, `bench_pgo.txt`, snapshot `pbrain-vcn04-pgo` | exit 0; bench 17621937 / 8c1f72e5 |
| 4 | Fixed-node table | done | `fixed_node_table.txt` | Largest rise of a root move against ordinary, 20K / 30K nodes: P1 VC2 +1.3 / +1.4; P2 VC2 +0.3 / +0.1; P3 VC2 +0.0 / +0.3; P1 VC3 all moves fall to 0.00 (-M). Before: +11.7 to +23.7 |
| 5 | Part C grid | **partial, not classified** | `partc/results_vcn04.tsv` | 108 rows of 112 + DONE line; the run was killed (no DONE line). Rows not yet compared with the VCN-03 slice 5 answers: zero-wrong-answers is NOT established. Rerun the whole grid |
| 6 | Two 30-minute runs (user's Renju opening) | **not run** | - | - |
| 7 | NPS against the old binary | done with an earlier PGO binary (before VCF nodes were counted in the budget); budget-0 numbers are not affected by that change, a refresh with the final binary was planned | `nps_fixed200k.txt`, `stats_vc2_fixed20k.txt`, `partc/budget_probe.tsv` | VC2 n/ms, 200K nodes, 1 thread, old / new budget 0 / 16 / 64 / 256 / ordinary: P1 168 / 131 / 62 / 23 / 9 / 127; P2 295 / 161 / 98 / 34 / 15 / 143; P3 187 / 129 / 48 / 17 / 7 / 143. VC3: new binary proves the root -M4 in 17 / 36 / 4 ms (old: 757 / 655 / 1398 n/ms, unproven or slower) |

### Stage 2 budget chosen so far

Default `mcts_vcn_pass_test_nodes = 0` (stage 1 only). Reason (measured): at 20K nodes the solver
proves NoThreat for 2-4 % of its runs at budget 16 and 6-12 % at 4096, Threat about 25 %, the rest
Unknown; NPS falls to 0.3-0.6x at budget 16 and below 0.1x at 256; in the 5 s budget probe no
budget from 16 to 1024 proved a cell that budget 0 did not. Budget 0 itself: the static rule ends
6 / 6619, 111 / 3441 and 20 / 6911 defender leaves on the three positions.

### Tests removed or rewritten

Removed (the feature they tested is gone):
`expandNode_vcnDefender_edgesSortedByPolicyDescendingAndSumToOne` (re-sort was part of
`applyVcnPassPrior`), `expandNode_vcnDefenderPassPriorZero_passLastOrAbsentAndBoardPoliciesUnscaled`,
`config_mctsVcnPassPrior_parsedAndClamped` (option removed),
`extractPV_mctsVcnPassInLine_printsPassLikeAb` (no pass in an MCTS PV),
`searchNode_vcnDefenderPass_childKeyHasRaisedLevel` (replaced by `mctsVcn_graphNeverContainsPassEdge`).

Rewritten: `expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior` to `expandNode_vcnDefender_noPassEdge`;
`expandNode_vcnDefenderFacingOpenThree_passEdgeAndSortedDefendList` to `..._noPassEdgeAndFullDefendList`;
`expandNode_vcnDefenderRenjuBlack_passEdgeAndNoForbiddenMove` to `..._noPassEdgeAndNoForbiddenMove`;
`widenNode_vcnDefenderCutNodeWithPassEdge_widensAndKeepsPass` to `widenNode_vcnDefenderCutNode_noPassEdgeAfterWiden`;
`mctsVcn_vc3_attackerWithoutThreat_defenderPassRefutes` to `mctsVcn_vc3_attackerWithoutThreat_losesByPassTestNoPassInPv`.

### Deviations from the instruction file and open issues

- Static rule excludes `J_FLEX2_2X` (above `K_BLOCK3` in the enum but makes no three) and
  answers Unknown for a Renju black attacker with any pattern-`FORBID` cell.
- Solver defender replies come from the `game/movegen` generators, not the MAIN picker (the
  picker would read evaluator policy on a board moved without evaluator updates).
- Budget unit: stones placed by the solver + VCF nodes; can overshoot by one VCF search. The
  solver is iterative deepening on quiet attacker moves (max 8), three-valued; only proven
  answers are cached (TT, own key salt and tag).
- Attacker candidates are scanned over the whole board.
- VC2 NoThreat terminal value: `mate_in(ply + 5)`.
- `evaluateNode_vc3DefenderFacingFive_noPassTest` cannot tell "test skipped" from "test passed".
- Old config key `mcts_vcn_pass_prior`: unknown keys are not read, the file still loads
  (test `config_oldMctsVcnPassPriorKey_ignoredWithoutError`).
- Diagnostic block `VCN_PASS_TEST_STATS` left in `search/mcts/search.cpp`, off by default.
- VC2 NPS of the new binary equals the ordinary search and is 0.55-0.78x of the old binary (the
  cheap pass subtrees are gone). Solver not unit-tested with several threads.
- Budget probe (5 s, 1 thread, earlier binary): P4 VC3 and P6 VC2 now proven `-M8`; no wrong
  answer seen there, but this is not the full grid.

### Resume

Run one at a time, nothing else heavy alongside (14 GB machine).

1. Part C grid (about 33 min), then classify:
   `cd build/vcn04 && ./partc/matrix.sh pbrain-vcn04-pgo cfg_default vcn04 > partc/matrix.log 2>&1`
   (rewrites `partc/results_vcn04.tsv`, ends with a `DONE` line), then
   `cd partc && python3 classify.py results_vcn04.tsv ../../vcn03s5/partc/results.tsv`.
   Reference: VCN-03 slice 5 MCTS 23/27 proven, 0 wrong. Any wrong answer must be investigated.
2. Long runs (30 min each, 8 GB, 8 threads), check free memory first:
   `cd build/vcn04/net && ../long30.sh vc2_30m_new base.toml "YXVCN 2"`, then
   `../long30.sh vc3_30m_new base.toml "YXVCN 3"`. Logs: `build/vcn04/vc2_30m_new.log`,
   `vc3_30m_new.log`. Compare with `logs/ord_30m.log` (F9 58.5 %) and `logs/vc2_30m.log`
   (C8 98.2 %); no `Pass` in any PV.
3. Optional NPS refresh with the final binary: `build/vcn04/nps.sh dir cfg rule nodes cmd moves`.
4. If source changed since the snapshot (md5 of `pbrain-vcn04-pgo`: f0379cdbd9c5f019e0721fea22a4212f),
   rebuild with `scripts/pgo_build.sh x64-clang-AVX512VNNI` and redo items 1-4 first.
5. Then write the results here, and only if all seven items passed move the `TODO.md` /
   `instruction.md` lines to Done.

## Resumed 2026-11-11: verification item 5 (Part C grid)

The killed grid run is used as it is (user decision: 108 of 112 rows are enough). Binary
`build/vcn04/pbrain-vcn04-pgo` (md5 f0379cdb...), built after the last source change. Classified
with `build/vcn04/partc/classify.py results_vcn04.tsv ../../vcn03s5/partc/results.tsv`.

| Budget | Threads | Cells | Proven | Correct | Wrong | VCN-03 slice 5 (proven) |
|---|---|---|---|---|---|---|
| 5 s | 1 | 27 | 24 | 24 | 0 | 23 |
| 5 s | 8 | 27 | 24 | 24 | 0 | 23 |
| 30 s | 1 | 27 | 24 | 24 | 0 | 23 |
| 30 s | 8 | 24 | 21 | 21 | 0 | 23 of 27 |

- Missing rows: 30 s / 8 threads, P9 at VC4, VC3, VC2 and the acceptance position. P9 gives the
  same correct answers in the three complete groups (`-M2`, `+M9`, `+M9`).
- Newly proven cell: P4 VC3 `-M8` (before: unproven, eval -67 to -164). Still unproven: P3, P4,
  P5 at VC2, as before.
- Acceptance position `h8j9f6i10` (VC2, correct move G5 = `6,4`): still not found. New binary
  returns `8,6` with eval -40 to 42; VCN-03 returned G7 with eval 442 to 486. Not a new error
  (MCTS missed G5 in VCN-03 too), not fixed by this task.

Item 5: passed (zero wrong answers on 105 classified cells).

## Resumed 2026-11-11: verification item 6 (user's Renju opening, 30 min, 8 GB, 8 threads)

Binary `build/vcn04/pbrain-vcn04-pgo`, `mcts_vcn_pass_test_nodes = 0` (default), harness
`build/vcn04/long30.sh`, one run each, one after the other. Logs:
`build/vcn04/logs/vc2_30m_new.log`, `build/vcn04/logs/vc3_30m_new.log`. Both runs stopped by the
engine at 1129 s, as the old runs did.

| Run | Best move | Win rate | Visits of best / all | Returned |
|---|---|---|---|---|
| Ordinary, old binary (`ord_30m.log`) | F9 | 58.55 % | 9460M / 9473M | F9 |
| `YXVCN 2`, old binary (`vc2_30m.log`) | C8, PV `C8 Pass` | 98.18 % | 21805M / 22114M | C8 |
| `YXVCN 2`, new binary | F9 | 56.64 % | 8611M / 8616M | F9 (`6,5`) |
| `YXVCN 3`, new binary | H6 `-M12` | 0.01 % | 1666M / 23617M | H6 (`9,7`) |

- VC2 timeline: F9 53.78 % at 151 s (open graph, 284 n/ms), graph closed between 181 s and 241 s,
  F9 56.45 % at 301 s, 56.64 % from 901 s to the end. No root move rose after the graph closed.
  Other root moves at the end: D8 50.33 %, J7 49.50 %, E8 47.90 %. C8 is not in the ten printed
  moves.
- VC3: all ten printed root moves are proven lost for the attacker (`-M4` to `-M12`). H6 was the
  last, unproven (-1120) at 901 s and `-M12` at 1051 s.
- `Pass` in the logs: 0 occurrences in both.

Item 6: passed. Open items: item 7 budget > 0 columns were measured with an earlier binary (the
default budget is 0, so they do not describe the default behaviour).

## Closed 2026-11-11

Status: done (user decision 2026-11-11: item 7 accepted as measured; close after a rerun of the
unit tests and the bench). Supersedes "Status stays Active" of the Paused section. Not committed.

| # | Item | Result |
|---|---|---|
| 1 | Unit tests, asserts live | rerun at close: 271/271 pass; `--check-asserts-live` aborts (exit 134) |
| 2 | Bench, gcc Release AVX512+VNNI (`build/`) | rerun at close: Nodes 17621937, Hash 8c1f72e5 |
| 3 | PGO build | done (`build/vcn04/pbrain-vcn04-pgo`, bench identical) |
| 4 | Fixed-node table | largest rise of a root move at VC2: +1.4 points (before: +11.7 to +23.7) |
| 5 | Part C grid | 0 wrong on 105 classified cells; 24/27 proven per group (before: 23/27) |
| 6 | 30 min / 8 GB / 8 threads | VC2 F9 56.64 % (old: C8 98.18 %); VC3 all printed root moves proven lost; no `Pass` |
| 7 | NPS against the old binary | accepted as measured: budget 0 columns valid; budget > 0 columns from an earlier binary |

Left as it is, not part of this task: the acceptance position `h8j9f6i10` still does not return
G5 under MCTS VC2; VC2 speed is 0.55 to 0.78x of the old binary; the stage 2 solver is not
unit-tested with several threads and its default budget is 0.
