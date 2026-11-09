# VCN-03 — execution guidance

Task: [`../todo/VCN-03-mcts-vcn.md`](../todo/VCN-03-mcts-vcn.md). Option B (native MCTS-VCN), done in
slices (§8.1). Slice 0 (guard) and the bound work it depended on (SEARCH-09, SEARCH-10) are done.
Slices 2 (key and root identity) and 3 (pass edge and prior) are done (2026-11-09). **The active slice
is slice 4 (VC4/VC5 terminals).** Slice 5 gets its own section here before it starts.

## Slice 2 — key and root identity

### Goal

MCTS nodes searched in VCN mode never mix with nodes from an ordinary search, from another level,
or from the other attacker, including across searches through graph reuse. With VCN off,
everything is bit-identical. No VCN search *behaviour* is added in this slice: no pass edge, no
VC4/VC5 terminals. Those are slices 3–4.

### Design (from todo §3.1, fixed)

- **Per-node level, derived from the board.**
  `level = min(VC5, rootLevel + (board.passMoveCountOfSide(defender) - rootDefenderPasses))`,
  where `defender = ~options.vcnMode.attacker`, and `rootLevel = options.vcnMode.n`.
  `rootDefenderPasses` is captured once per search, before the workers start (in `searchMain` or
  `setupRootNode`, stored in `MCTSSearcher::scratch`). No `Node` field and no recursion parameter
  (`Node` stays 56 bytes).
- **Node key** = `board.zobristKey() ^ Vcn::hashXor(options, level)`. `hashXor` is `0` when VCN
  is off, so the key equals the zobrist key exactly.
- **One helper** (for example `mctsNodeKey(const Board &, const SearchOptions &, int
  rootDefenderPasses)`, or a searcher method) used at every key site:
  - child allocation in `searchNode` (`search/mcts/search.cpp:649-650`);
  - root allocation in `setupRootNode` (`search.cpp:1318`);
  - the `node.getHash()` assert in `searchNode` (`search.cpp:565`).
  
  Grep again for `zobristKey()` and `allocateOrFindNode(` under `search/mcts/` before editing.
- **Share the VCN helpers.** `namespace Vcn` (`levelIncrement`, `childLevel`, `vc5AttackerLoses`,
  `hashXor`) sits inside the anonymous namespace of `search/ab/search.cpp:244-276`. Move it as is
  to a shared header (for example `search/vcn.h`) and include it from both searchers. This is a
  pure move with no change to the code. AB must stay bit-identical (bench and `tests/vcn_tests.cpp`).
- **Root reuse.** The early return in `setupRootNode` (`search.cpp:1312`) compares only the move
  list. Also compare the root key: store the previous root key in `GraphState` next to
  `previousPosition` and rebuild the root when it differs. Otherwise a VCN search on the same
  position as the last ordinary search would reuse the ordinary root. Decide whether the
  `recycleOldNodes` condition needs the same key test; nodes of the other mode just become
  unreachable and age out. Write the decision down.
- **The guard stays.** `searcherSupportsVcn()` (`command/gomocup.cpp:72`) keeps refusing `YXVCN`
  and warning under MCTS until slice 5. VCN under MCTS is not usable until the pass edge and the
  terminals exist. Tests drive the searcher directly, with `SearchOptions::vcnMode` set, and do
  not go through the protocol.

### Tests (write first, `<function>_<scenario>_<expectedBehavior>`)

The todo's §7 slice-2 list, made concrete:

- `mctsNodeKey_vcnOff_equalsZobrist`
- `mctsNodeKey_vcnOn_differsByLevelAndAttacker`: same board; level VC2 vs VC3, and attacker
  BLACK vs WHITE, give four distinct keys, all different from the zobrist key.
- `mctsNodeKey_defenderPassRaisesLevel_cappedAtVc5`: the key after one or two defender passes
  equals the key computed with the raised level. At VC5 a further pass keeps VC5.
- `setupRootNode_samePositionVcnToggled_rebuildsRoot`: ordinary search, then a VCN search on the
  same moves. The root node differs (key/hash), and the next ordinary search gets the ordinary
  root back.
- `mctsVcn_afterVcnSearch_nextOrdinarySearchReusesNoVcnNode`: after a VCN search, every node
  reached by an ordinary search has a key equal to its board's zobrist key. Walk the tree from the
  root via edges, replaying the moves on a `Board`.
- `vcnHelpers_movedToSharedHeader_abBehaviourUnchanged`: covered by the existing
  `tests/vcn_tests.cpp` staying green. Name the check in the todo; no new test needed.

Each test builds its own `Board`, is deterministic, and has asserts live (`rapfi_tests` forces
`-UNDEBUG`). An MCTS search in a test needs an evaluator: follow the existing MCTS tests
(`tests/mcts_*_tests.cpp`) for the setup, and use a small fixed node count.

### Pitfalls

- **The VCF TT key is separate.** `SimpleVCF` uses `board.zobristKey()` for the shared VCF TT
  (`search.cpp:115`). Leave it alone in this slice; its VCN semantics (`vcfOnly`) are slice 4.
  Write that down in the todo, so nobody thinks slice 2 covered it.
- **Passes and zobrist.** Check how `Board::move(Pos::PASS)` changes `zobristKey()` (side to
  move only?). The level formula relies on the defender's pass count, not on the key, so this is
  fine either way, but confirm it and write it down.
- **`rootDefenderPasses` visibility.** Write it before `runOnAllThreads` dispatches the workers
  (the same happens-before as `graph.root`; see the comment at `searchMain`). Do not store it in
  `SearchOptions`, which the protocol owns.
- **Attacker.** `think()` sets `options.vcnMode.attacker = board.sideToMove()`
  (`command/gomocup.cpp:127`). Tests that drive the searcher directly must set it themselves.
- **Bit-identity with VCN off** is the main risk. Check the bench, and also an MCTS fixed-node
  golden output with VCN off, because BENCH is alpha-beta and does not cover MCTS. Reuse
  `build/search10/` (or `build/search09/final_extract.sh`) for the golden.
- `real_test` and `test_mcts.txt` in the repo root: do not edit or commit.

### Don't touch (slice 2)

- `expandNode`, the MovePicker pass stages, priors (slice 3). `evaluateNode` and the terminals,
  `SimpleVCF`, `wincheck.h` (slice 4).
- The SEARCH-09/10 bound and widening code, `Edge`/`Node`/`EdgeArray` sizes, and the MCTS
  defaults (the cut stays on in VCN, decision §8.4).
- AB VCN behaviour. The helper move must be a pure move.
- The protocol guard in `command/gomocup.cpp` (it stays until slice 5).

### Definition of done (slice 2)

- The tests above pass with asserts live, the full `rapfi_tests` suite is green, and the
  fail-first state is recorded.
- Bench `17621937 / 8c1f72e5`. The MCTS golden output with VCN off is identical to the HEAD of
  branch `search-09-10-mcts-bound`.
- `tests/vcn_tests.cpp` is green (AB VCN unchanged after the helper move).
- Todo §9 has a "Slice 2" progress entry with the numbers and decisions (the recycle condition,
  the zobrist/PASS finding, the VCF TT key deferred). The status line says slice 2 is done.
  VCN-03 stays **Active** in `TODO.md`, since slices 3–5 remain. Update the index line's
  progress text.
- Self-play is not needed: with VCN off the change is bit-identical. The golden output is the
  guard.
- Commit on branch `search-09-10-mcts-bound`, in conventional style (`test:`, `search:`,
  `docs:`), only if the user asked.

## Slice 3 — pass edge and prior

### Goal

In VCN mode, a non-root MCTS node where the defender is to move gets a `Pos::PASS` edge under the
same rule as AB, with a real prior. Edges stay sorted by policy, and the pass survives the cut and
the SEARCH-10 widening. With VCN off nothing changes. No terminals: VC4/VC5 rules and `vcfOnly`
come in slice 4. The protocol guard stays until slice 5.

### Design (fixed)

- **When a pass edge exists.** It follows AB's rule (`search/ab/search.cpp:1450`):
  `vcnDefenderPass = vcn enabled && side to move != attacker && !board.p4Count(attacker, A_FIVE)`.
  It is non-root only (the root is the attacker, and the `ROOT` picker never passes). The picker's
  own checks stay as they are (`search/movepick.cpp:578-590`): there is no pass stage against a
  five, and no pass right after a pass.
- **One move-generation helper** shared by `expandNode<false>` (`search/mcts/search.cpp:430-441`)
  and `widenNode` (`search.cpp:512-539`). It builds the `MAIN` picker with
  `generatePassMove = vcnDefenderPass`, collects moves and normalised policies, applies the pass
  prior, and returns them sorted. **The `widenNode` part is mandatory.** Today `widenNode`
  regenerates without a pass. A kept pass edge would then be "missing": the debug assert at
  `search.cpp:543-545` fires, and in release `replaceEdgesWithFull` gives up, so a defender node
  could never be proven lost again. That would undo SEARCH-10 in VCN.
- **Pass prior.**
  - Add a config value `mcts_vcn_pass_prior`: `SearchCfg.mctsVcnPassPrior` in
    `search/searchconfig.h`, parsed next to `mcts_edge_policy_mass` in `config.cpp:377`, clamped to
    `[0, 1)`, default **0.25**. The default is not measured; slice 5 measures it and may tune it.
    Write that down in the todo.
  - When the pass is generated: `P(pass) = prior`, and every board move's normalised policy is
    multiplied by `1 - prior`, so the sum stays about 1.
- **Order.** `createEdgesFrom`, `selectEdgeCount` and `selectChild` (the first unexplored edge has
  the highest remaining policy, `search.cpp:351`/`378`) all need the list in descending policy
  order. The picker returns PASS first with policy 0, so re-place it by its prior. A stable
  insertion of the single pass entry into the already-sorted board moves is enough. Check first
  that the picker's board moves really come out sorted by normalised policy in the `DEFENDFOUR` /
  `DEFENDB4F3` stages; if not, sort the whole list stably.
- **Defender passes and the key.** The pass child's key already carries the raised level (slice 2:
  `passMoveCountOfSide(defender)`). `board.move(rule, Pos::PASS)` and the evaluator hooks already
  support a pass, because AB uses them.
- **`vcnAttackOrder`** (an AB-only ordering) is **not** used. MCTS orders by policy.

### Tests (write first)

`expandNode`/`widenNode` live in an anonymous namespace. Expose the shared move-generation helper
through a small internal header, or test it through the searcher the way `tests/mcts_vcn_tests.cpp`
(slice 2) does. Pick one and write it down. Use the todo §7 names plus these:

- `expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior`
- `expandNode_vcnDefender_edgesSortedByPolicyDescendingAndSumToOne`
- `expandNode_vcnDefenderFacingFive_noPassEdge`
- `expandNode_vcnAttacker_noPassEdge`
- `expandNode_vcnOff_edgesUnchanged` (same moves, policies and order as before the change)
- `expandNode_vcnDefenderPassPriorZero_passLastOrAbsentAndBoardPoliciesUnscaled` (pins the edge
  case; choose and document whether prior 0 means "no pass edge" or "pass last")
- `widenNode_vcnDefenderCutNodeWithPassEdge_widensAndKeepsPass`: build a cut defender node whose
  kept edges, including the pass, are all proven lost. The widen must succeed, and the node then
  proves lost.
- `config_mctsVcnPassPrior_parsedAndClamped`
- `searchNode_vcnDefenderPass_childKeyHasRaisedLevel`: a short fixed-node VCN search. The PASS
  child's hash equals `mctsNodeKey` at the raised level.

### Pitfalls

- **Policy 0 for PASS from the picker** (`movepick.cpp:586`). Never let a zero-policy edge sit
  ahead of positive ones. That breaks the "first unexplored = highest remaining prior" invariant.
- **The cut runs after the prior.** Apply prior and order first, then `selectEdgeCount` /
  `createEdgesFrom`. With prior 0.25 the pass is always kept; do not special-case the cut.
- **`expandNode` is the hot path.** With VCN off the helper must do exactly today's work. The
  bit-identity guard is the MCTS golden output with VCN off (cut on and off), and the bench.
- **Renju:** check that a pass does not trip the forbidden-move or `validateOpponentCMove`
  logic in the picker for the defender, as AB already runs it. Cover Renju in at least one test if
  cheap.
- **SEARCH-10 retire/widen code** is not to be changed beyond calling the shared helper.
- `real_test`, `test_mcts.txt`: do not edit or commit.

### Don't touch (slice 3)

- `evaluateNode`, terminals, `SimpleVCF`, `wincheck.h`, the VCF TT key (slice 4).
- The protocol guard in `command/gomocup.cpp` and PV/printer handling of PASS (slice 5).
- AB search and `MovePicker`'s pass rules. MCTS adapts to the picker, not the other way round.
- `Edge`/`Node`/`EdgeArray` sizes, the SEARCH-09/10 bound logic, and the defaults of the other
  `mcts_*` options.

### Definition of done (slice 3)

- The tests above pass with asserts live, the full suite is green (including
  `tests/vcn_tests.cpp`), and the fail-first state is recorded.
- Bench `17621937 / 8c1f72e5`. The MCTS golden output with VCN off (cut on and off) is identical
  to branch HEAD `4149332`.
- Todo §9 has a "Slice 3" entry recording: the prior default and that it is unmeasured, the
  ordering check result, how prior 0 behaves, and the test-exposure choice. The status line is
  updated. VCN-03 stays Active.
- No self-play: VCN-off is bit-identical and VCN-on is not reachable from the protocol yet.
- Do not commit unless the user asks.

## Slice 4 — VC4 / VC5 terminals

### Goal

`evaluateNode` (`search/mcts/search.cpp:548`) applies the VCN leaf rules of todo §3.3, with the same
values as AB. A VC4 attacker node is always solved at evaluation: either it has a VCF that is made
only of fours, or it loses. A VC4 defender node wins at once when the attacker made no five threat.
A VC5 attacker without a five loses. With VCN off, nothing changes (bit-identical). The protocol
guard and PV printing stay for slice 5.

### Deviation from §8.1, decided here: share the VC4 rules, not the AB function

§8.1 said the VC4 solver would be "shared with AB (factored out of `search/ab/search.cpp`)". AB's
`vcfsearch`/`vcfdefend` (`search/ab/search.cpp:2064`, `:2347`) is tied to AB's `SearchStack`,
`NodeType`, PV, eval stand-pat, policy tracing and timing. Factoring it out would put the AB bench
at risk for no gain in correctness.

Instead, MCTS reuses **the same primitives with the same flags**, and `search/ab/search.cpp` is not
edited:

- `quickWinCheck<R>(board, ply, beta, vcfOnly)` (`game/wincheck.h:38`). Give the runtime wrapper
  `quickWinCheck(Rule, ...)` (`wincheck.h:118`) a defaulted `vcfOnly = false` parameter. This is
  additive; existing calls are unchanged.
- The QVCF picker with `forceAllowB4InVCF = fullBoardVCF = true`, exactly as AB passes `vcfOnly`
  twice at `search/ab/search.cpp:2224-2228`.
- The VC4 values AB uses:
  - attacker with no VCF: `mated_in(ply + 2)` (AB: "refuted by a defender pass");
  - defender with no five threat from the attacker: `mate_in(ply + 1)` (`search/ab/search.cpp:2372-2377`);
  - VC5 attacker without a five: `mated_in(ply)`, checked **before** the draw check (§8.3).

Write this deviation in todo §8 or §9, so the user can overrule it.

### Design

- **Level.** `mctsNodeVcnLevel(board, options, searcher.scratch.rootDefenderPasses)` (slice 2), only
  when VCN is enabled. `ply` / mate values use `board.ply()`, as MCTS does today.
- **`evaluateNode`, non-root, VCN on.** Order, with `attacker = options.vcnMode.attacker`:
  1. VC5, attacker to move, `Vcn::vc5AttackerLoses(board, attacker)`: terminal
     `mated_in(board.ply())`.
  2. Board full / maxMoves: draw (unchanged).
  3. Immediate win: `quickWinCheck(rule, board, ply, beta, vcfOnly)`, with
     `vcfOnly = attacker to move && level == VC4`. Then the maxMoves clamp, as today.
  4. VC4, defender to move, `!board.p4Count(attacker, A_FIVE)`: terminal `mate_in(board.ply() + 1)`
     (the defender wins by passing).
  5. VC4, attacker to move: run the VC4 VCF (below). On a win, terminal with that mate value.
     Otherwise terminal `mated_in(board.ply() + 2)`. **Never** fall through to the NN value.
  6. Otherwise (VC2/VC3, either side; VC4 defender facing a five; VC5 defender): the existing
     `SimpleVCF::vcf`, then the NN value.
- **VC4 VCF in `SimpleVCF`.** Add a `vcfOnly` mode to `SimpleVCF::vcf`/`vcfsearch`/`vcfdefend`:
  - **Attacker nodes:** `quickWinCheck(..., vcfOnly)`; QVCF picker with
    `forceAllowB4InVCF = fullBoardVCF = true`; no stand-pat (the start value is
    `mated_in(ply + 2)`, as in AB; a missing VCF returns it).
  - **Defender nodes:** unchanged. They block the five; a forbidden block in Renju still loses.
  - **The mode is a parameter, not a global.** The default path (`vcfOnly = false`) must run exactly
    today's code.
- **VCF TT key.** In VCN mode, `SimpleVCF` stores and probes under
  `board.zobristKey() ^ Vcn::hashXor(options, level)`, where `level` is the level at the
  `SimpleVCF` entry (AB: "the level stays constant along a vcf chain"). VCF-only results must never
  meet plain VCF results in the shared `TT`. With VCN off, the key is exactly `zobristKey()`.
- **Root.** `evaluateNode<true>` skips terminal checks today; keep it so. At a VC4 root the
  attacker's children (defender, VC4) resolve by rule 4, or by the five-block line.

### Tests (write first; todo §7 slice-4 names plus these)

- `evaluateNode_vc4AttackerDoubleThreeOnly_attackerLoses` (VCN-02 1b)
- `evaluateNode_vc4AttackerFarVcf_attackerWins` (VCN-02 1: the full-board picker finds a far VCF.
  Reuse the `farVcfAfterForcedPairBoard` fixture in `tests/vcn_tests.cpp`)
- `evaluateNode_vc4AttackerNoVcf_attackerLosesNotNnValue`
- `evaluateNode_vc4DefenderNoFiveThreat_defenderWins`
- `evaluateNode_vc4DefenderFacingFive_notTerminalByPassRule`
- `evaluateNode_vc5AttackerNoFive_attackerLoses`
- `evaluateNode_vcnBoardFull_returnsDrawLikeAb`
- `evaluateNode_vc5AttackerNoFiveOnFullBoard_attackerLosesNotDraw`
- `evaluateNode_vc3_unchangedFromOrdinaryRules` (VC2/VC3 behave as VCN-off except for the key)
- `evaluateNode_vcnOff_unchanged` (VCN off: same terminal and value as before)
- `simpleVcf_vcfOnlyMode_ttKeySeparatedFromPlain`: a plain VCF result in the TT for a position
  does not satisfy a VC4 probe, and the reverse.
- `quickWinCheckRuntime_vcfOnlyFlag_matchesTemplate` (the wrapper passes the flag through)
- **Cross-check against AB:** on each `tests/vcn_tests.cpp` VC4/VC5 leaf position, the terminal
  value MCTS `evaluateNode` gives has the **same sign and mate class** (win / loss / draw / none) as
  AB's `vcfsearch` result at that node. Plies may differ by the stack convention; state which.

`evaluateNode` is internal. Expose it through `search/mcts/searchtesting.h` as slice 3 did.

### Pitfalls

- **Bit-identity with VCN off** covers `evaluateNode`, `SimpleVCF`, the `quickWinCheck` wrapper and
  the VCF TT key. Guards: bench, and the MCTS golden with VCN off (cut on and off).
- **The order of rules matters:** VC5 loss before the draw; `vcfOnly` win check before the VC4 VCF.
  AB's order is the reference (`search/ab/search.cpp:1251-1306`, `:2104-2122`).
- **No stand-pat in VC4 mode.** A `VALUE_ZERO` start in `vcfsearch` makes a missing VCF look like
  "no result", and the node would fall through to the NN value. That is wrong: in VC4 it is a loss.
- **The level inside `SimpleVCF` is constant** (no passes inside a VCF chain). Compute it once at
  entry.
- **Mate plies and maxMoves:** keep the existing `mate_step(value, 0) > options.maxMoves` clamp
  where `evaluateNode` has it. Use `nonPassMoveCount` consistently, as AB does.
- **The TSan reports in the lockless TT are pre-existing** (VCN-03 §8.4). Do not try to fix them
  here.
- `real_test`, `test_mcts.txt`: do not edit or commit.

### Don't touch (slice 4)

- `search/ab/search.cpp` (no factoring; AB bit-identical), and the `MovePicker` rules.
- The slice 2/3 key and pass-edge code, except to read the level. The SEARCH-09/10/11 code and
  docs. The `Edge`/`Node`/`EdgeArray` sizes. The `mcts_*` defaults.
- The protocol guard in `command/gomocup.cpp` and PV printing (slice 5).

### Definition of done (slice 4)

- The tests above pass with asserts live, the full suite is green (including `tests/vcn_tests.cpp`),
  and the fail-first state is recorded.
- Bench `17621937 / 8c1f72e5`. The MCTS golden with VCN off (cut on and off) is identical to branch
  HEAD.
- A small VCN-on smoke run through the searcher, not the protocol: VC4 and VC3 searches on 2–3
  `vcn_tests.cpp` positions, 1 thread, fixed nodes. Report the root result next to AB's answer for
  the same position and level. This is information for slice 5, not a pass/fail gate, but a wrong
  `+M` (a mate AB refutes) is a failure.
- Todo §9 has a "Slice 4" entry: the deviation from §8.1, the rule order, the TT-key change, the
  numbers, and the AB cross-check table. The status line is updated. VCN-03 stays Active.
- Do not commit unless the user asks.
