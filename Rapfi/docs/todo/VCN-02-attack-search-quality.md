# VCN-02 — VCN tree search: attack-oriented search quality and real-world correctness

**Status:** open (recorded 2026-10-06, not started). Review first, then implement slice by slice.

**Class:** B (search behaviour). Each slice needs a decision note (what / expected / how
measured) before code. Item 1 (QVCF locality at VC4) is a correctness fix and goes first.

**Depends on:** [VCN — VCN search mode](VCN-search-mode.md) (done 2026-10-06, still uncommitted
in the working tree at the time of writing; check `git status` / `git log` first).

## Context: how VCN search behaves today

Analysis by reading `search/ab/search.cpp` and `search/movepick.cpp`. Not yet confirmed on
complex positions.

- There is no separate "VCN thread": with `INFO VCTHREAD 1..3` (persistent) or `YXVCN 2..4`
  (one-shot), **every** Lazy-SMP thread runs the same VCN tree. Attacker = side to move.
- Root is the attacker, enumerates all root moves, never passes. The VC4 → `vcfsearch` jump is
  skipped at the root.
- Level is inherited from the parent and raised by one when the parent (defender) played PASS.
  TT keys are XORed with `(level, attacker)`.
- **VC2/VC3:** ordinary `search()`.
  - Attacker nodes in a quiet position generate `ALL` moves, ordered by NN policy + history.
  - Defender nodes generate **PASS first**, then the usual moves.
  - Against an attacker open three or four-three, the defender's moves are PASS + defence moves +
    own counter-fours.
  - Against an attacker five threat the defender only blocks (no PASS).
- **VC4:** attacker → `vcfsearch`, fours only, no stand-pat (start value `mated_in(ply+2)`).
  Defender → `vcfdefend`. If the attacker's last move created no five threat, the defender
  "wins by passing".
- **VC5:** internal only. An attacker without a five loses.
- **Horizon at VC2/VC3:** ordinary quiescence `vcfsearch` with eval stand-pat. The defender has
  no PASS there.

### Real-world (counter-attack) handling today
| Situation | Today |
|---|---|
| Defender counter-four | Correct: attacker forced to block, level unchanged. At VC4 a non-four block breaks the VCF → attacker loses (strict VCF). |
| Defender counter open three | Correct: attacker may only defend or play fours. At VC4 with no four → attacker loses. |
| Defender has a faster winning line | Partial: full-width inside `search()`. At the horizon only the defender's own VCF is checked, not its VCT/VC2. |
| Defender blocks while making a four | Correct: next attacker node must block. |
| Renju: defender black blocks on a forbidden point; attacker black fours | Correct (existing code). |
| PASS in a real game | Proof device only. The root is the attacker, so the engine never plays PASS; the PV may contain PASS. |

## Problems to fix (ranked)

1. **VC4 only finds fours near the attacker's last move (correctness).**
   - `MovePicker` `QVCF_MOVES` uses `generateNeighbors` around
     `board.getLastActualMoveOfSide(self)` with `RANGE_SQUARE2_LINE4`. In ordinary Rapfi this is
     a quiescence and the locality is fine.
   - In VCN, VC4 is the proof step (`vcfOnly`). A VCF that starts elsewhere is missed, and the
     node returns `mated_in(ply+2)`, i.e. the attack line is wrongly refuted. Example: right after
     the attacker blocked a defender four far from its own stones.
   - First write a failing position test, then for `vcfOnly` generate VCF moves over the whole
     candidate area (e.g. `generate<VCF>` / `VCF | RULE_RENJU` instead of `generateNeighbors`).
2. **Attacker move generation at VC2/VC3 is `ALL`.**
   - Quiet moves are only refuted indirectly by the defender's PASS. Width is the whole board at
     every attacker node.
   - Restrict by level: VC3 (VCT) = fours + threes; VC2 = + twos.
   - Reuse the unused `GenType::VCT` / `GenType::VC2` filters in `game/movegen.cpp`
     (`basicPatternFilter`). Review their boundaries first: non-COMB VC2 accepts
     `L_FLEX2..I_BLOCK3_PLUS`, including block-three shapes. See the VC2 review notes in
     [VCN-search-mode.md](VCN-search-mode.md).
   - Keep the defence stages (`DEFENDFIVE` / `DEFENDFOUR` / `DEFENDB4F3`) unchanged for the attacker.
3. **Defender width when the attacker made a plain three or two.**
   - The defender gets PASS + `ALL`. Dedicated defence generation exists only for five, four and
     four-three (`DEFEND_FIVE` / `DEFEND_FOUR` / `DEFEND_B4F3`).
   - Option: for VCN defender nodes, generate PASS + defence squares of the attacker's last
     threat + own counter-fours (+ counter-threes at VC2?). Needs care so real counter-attacks
     are not cut.
4. **Heuristic pruning still active inside VCN.**
   - Futility (Step 8), razoring (Step 7), LMR, policy pruning and fail-high reduction can cut
     correct attacking moves or correct defender counter-attacks.
   - Move-count pruning is already disabled when the defender is losing (`bestValue ≤ MATED`).
   - Decide per heuristic whether to disable it for VCN attacker and/or defender nodes, then measure.
5. **Horizon bias at VC2/VC3.**
   - At `depth <= 0` the attacker stands pat on eval and the defender has no PASS, so scores there
     mean "good eval", not "attack continues".
   - Option: in VCN, attacker horizon nodes with no forcing continuation should score as a failed
     attack (like `vcfOnly`) instead of eval. Decide after items 1–4, as it changes output values.
6. **Attack move ordering.** The attacker uses ordinary policy/history ordering. Consider ordering
   by threat strength first (pattern4 of the move) inside VCN.
7. **Defender's longer counter-wins at the horizon.** Only the defender's VCF is seen beyond the
   horizon. Probably acceptable; revisit after 1–5.

Out of scope for now: MCTS (ignored by user decision), output format (user is satisfied: no
"win / winning line" output needed), TT per-entry level check (decided not needed).

## Measurement plan (before any change)

- Build a small fixed set of attack positions (freestyle + renju) with known VCF / VCT / VC2
  results, including:
  - a far-away VCF (item 1),
  - a defender counter-four,
  - a defender counter open three,
  - a defender with a faster own VCF.
- Record nodes / time / result per level (`YXVCN 2|3|4`, fixed `MAX_DEPTH`) as the baseline.
- `bench` must stay bit-identical (VCN off path): Nodes `16733809`, Hash `4621a98a`.

## Function → test-case list (draft; finalise before code)

- `vcnSearch_vc4_vcfFarFromLastMove_found` (item 1, must fail today)
- `vcnSearch_vc4_counterFourBlockedWithoutFour_attackerLoses`
- `vcnSearch_vc3_defenderCounterOpenThree_attackerMustDefend`
- `vcnSearch_vc3_defenderFasterVcf_notAttackerWin`
- `movePickerMain_vcnAttackerVc3_onlyFoursAndThrees` (item 2)
- `movePickerMain_vcnAttackerVc2_includesTwos` (item 2)
- `movePickerMain_vcnAttackerFacingOpenThree_onlyDefenceAndFours` (regression)
- `movePickerMain_vcnDefenderQuiet_passThenDefenceSquares` (item 3, if done)
- `basicPatternFilter_vct_boundaries` / `basicPatternFilter_vc2_boundaries` (item 2)

## Verification

- Tests built with asserts live (`rapfi_tests`, `-UNDEBUG`).
- `bench` bit-identical with VCN off.
- Node/time/result comparison against the measurement baseline above, per slice.
