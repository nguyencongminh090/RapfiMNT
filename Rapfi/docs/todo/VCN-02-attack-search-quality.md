# VCN-02 — VCN tree search: attack-oriented search quality and real-world correctness

**Status:** closed 2026-11-03 with known limitations (items 5 and 7 not planned, see §Known limitations). Items 1 and 1b done; item 4 closed (old pruning kept, one iteration
fix) (2026-10-06). Item 6 (threat ordering) implemented and measured (-32% nodes with NNUE, wins 147→149, one known regression #117); item 5's value change is deferred. Acceptance
position `h8j9f6i10` recorded, tracked by the move G5, not by the value (see §Acceptance position).

**Class:** B (search behaviour). Each slice needs a decision note (what / expected / how
measured) before code. Item 1 (QVCF locality at VC4) is a correctness fix and goes first.

**Depends on:** [VCN — VCN search mode](VCN-search-mode.md) (done 2026-10-06, committed as
`b21a2b2`).

## Review of the problem list against the code (2026-10-06)

Checked against `search/ab/search.cpp`, `search/movepick.cpp`, `game/movegen.cpp`, `game/wincheck.h`.

- Items 1–7 hold as written. Item 1 confirmed: `QVCF_MOVES` takes `generateNeighbors` around
  `getLastActualMoveOfSide(self)` with `RANGE_SQUARE2_LINE4`, and `vcfOnly` changes only
  `forceAllowB4InVCF`. Measured below (P1/P8 VC4 `-M4`).
- Item 1's example is right, and the case where it bites is narrow. A VC4 node is reached either
  from a VC3 node through a defender pass, or from a VC4 root through a four or a forced block.
  The root enumerates every move, so a far VCF is only missed **below** the root, after the
  attacker's last stone landed away from the VCF area (a forced block, or a VC3 quiet move the
  defender answered with a pass).
- **Missing, item 1b (correctness, VC4 false win).** `quickWinCheck()` (`game/wincheck.h`) is called
  at the top of `vcfsearch`, so at every non-root VC4 attacker node. Its `F_FLEX3_2X` rule returns
  `mate_in(ply + 5)` for a double open three. A double three is not a VCF: at VC4 the defender
  answers it with a pass and the attacker is at VC5 without a five. Measured below: P7 VC4 `+M7`,
  true result is a loss. The `C_BLOCK4_FLEX3` and `B_FLEX4` rules are VCF-valid (the follow-up is
  a four each time). It is not visible at the root (root skips `quickWinCheck`). It also masked
  the first version of the item 1 test position, which happened to contain a double three.
- Item 2 detail: `basicPatternFilter`'s `VCT` branch accepts `p4 < E_BLOCK4 && p4 >= H_FLEX3`,
  i.e. threes only; a VC3 attacker generator needs `VCF | VCT`. Neither `generate<VCT>` nor
  `generate<VC2>` is instantiated (already noted in the instruction).

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
   - **Done 2026-10-06**, see §Item 1 below.

1b. **VC4 reports a double three as a win (correctness).** See the review section above.
    **Done 2026-10-06**, see §Item 1b below.
2. **Attacker move generation at VC2/VC3 is `ALL`.** *Proposed 2026-10-06: no restriction,
   ordering only; merged into item 6.*
   - A VC3 attacker move does not have to be a three or a four: a quiet move that creates a VCF
     (or, at VC2, one that creates a VCT) is a real attack. A pattern filter (fours + threes,
     `+ twos` at VC2) would drop exactly the hidden attacks VCN exists to find.
   - Width stays `ALL`. Non-attacking moves stay cheap: the defender's pass is searched first and
     refutes them within about one ply. Speed comes from ordering (item 6).
3. **Defender width when the attacker made a plain three or two.** *Proposed 2026-10-06: ordering
   only.*
   - The defender gets PASS + `ALL`. Cutting defender moves would miss defences and report false
     attacker wins.
   - Option kept: order PASS, then the defence squares of the attacker's last threat, then own
     counter-fours, then the rest.
4. **Heuristic pruning still active inside VCN.** *Closed 2026-10-06: the ordinary pruning stays
   (user decision: turning it off costs too much). Only the "singular root" early stop is off in
   VCN. See §Item 4.*
5. **Horizon value at VC2/VC3 measures the position, not the attack.** *Next.*
   - At `depth <= 0` the attacker stands pat on the ordinary eval, so a position with no attack left
     can still return a positive value (P4 VC2 returns `644` without a win). Under the purpose of VCN
     (§Settled decisions) the value must be strongly negative when no attack exists, whatever the
     positional eval says.
   - Direction (to be written up as a decision note before code): when the attacker at the horizon
     has no attacking continuation (no move making a four or an open three; at VC2 also an open
     two, read from the pattern counts), return a large negative value independent of the eval;
     while an attack exists, keep the eval. The negative constant is a tuning parameter, measured.
6. **Attack move ordering** (absorbs item 2). The attacker uses ordinary policy/history ordering,
   which follows the network and can be blind to exactly the attacks VCN is for. Order by threat
   strength first (pattern4 of the move: fours, threes, multi-threat moves), then policy/history.
   Also the main lever on VCN cost now that pruning stays.
7. **Defender's longer counter-wins at the horizon.** Only the defender's VCF is seen beyond the
   horizon. Revisit after 5 (the horizon value is where a defender counter-attack would show).

### Settled decisions

| Decision | Date | Detail |
|---|---|---|
| **Purpose of VCN** | 2026-10-06 (user) | VCN maximises the attacker's chances: it looks for every way to attack and win. Its value measures the attack chance, not the position: high (and `+M` when proven) while an attack exists, strongly negative when there is none, even if an ordinary search rates the position balanced or better. Its main use is complex positions where an ordinary search or the network misses an attack (e.g. an ordinary 40% position with a hidden win). Such positions cannot be written as hand-made tests; quality is measured on a position set from real games with a fixed time per position. |
| ~~"No miss"~~ | withdrawn 2026-10-06 (user) | Earlier rule: no win within the depth may be missed. Withdrawn as too strict (it made pruning a hard rule instead of a measured trade-off). Pruning and width choices are decided by measurement against the purpose above. |
| Pruning inside VCN | 2026-10-06 (user) | Keep the ordinary pruning. Turning it all off found 12 more wins on 270 self-play positions but cost ~10× time (see §Item 4). |
| Meaning of a low value | 2026-10-06 (user) | "Losing" in VCN is not a label: it is a low value, `-M` when proven or a negative eval when no attack exists. `-M` / negative means "no win found within the depth/time", not a proof the defender wins. `+M` is a proof. |
| Threat bonus at the horizon (item 5) | deferred 2026-10-06 (user) | Scoring "attack still alive" above the positional eval needs a tuned bonus; tuning it properly means SPSA over ~1M self-play games. Not now; SPSA cannot be run on the user's machine (2026-10-07), so item 5 stays closed unless that changes. Consequence: the `h8j9f6i10` criterion eval > 600 stays unmet; VCN picks the right move (G5) but scores it near 0. |

### Known limitations (2026-10-07)

- **Item 5 (horizon value) and item 7 (defender's longer counter-wins) are not done and not planned.**
  Item 5 needs an SPSA-tuned attack bonus (~1M self-play games), which the user cannot run locally;
  item 7 depends on item 5.
- **Effect on use:** VCN picks the right move on the acceptance position `h8j9f6i10` (G5) but scores it
  near 0, not above 600. Read the VCN value as `+M` (proven win) or "no win found", not as a
  calibrated attack-chance score. A positive non-mate value at VC2/VC3 is still the ordinary eval.
- **To reopen:** write the item 5 decision note, add a tunable constant, and measure it on the
  position set in `tests/vcn_measure.sh` plus `tests/vcn_selfplay.sh` / `tests/vcn_sweep.sh`
  (a smaller sweep than full SPSA may be enough for a first value).

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

### Position set and baseline (recorded 2026-10-06, before item 1)

Positions and the runner are in [`tests/vcn_measure.sh`](../../tests/vcn_measure.sh)
(`tests/vcn_measure.sh build/pbrain-rapfi 10`: single thread, `MAX_DEPTH 10`, 15×15). Each
position's comment states the expected result. Three consecutive runs give identical results,
depths and node counts.

**Runner bug fixed 2026-10-06 (during item 1b).** The first version piped `END` straight after
`YXVCN`; the engine reads stdin while thinking, so `END` stopped longer searches part-way and their
node counts varied run to run (P5 VC2 gave 427–592 nodes). The runner now sends `END` only after
the move line. The tables below were all re-measured with the fixed runner; the "before item 1"
and "after item 1" states were rebuilt by disabling the later fixes at their `vcfsearch` call
sites. Rows that stop at depth 6 (P1, P4 VC2, P7, P8) have one legal root move (the forced
block), so iterative deepening ends early.

| Position | Expected | VC4 | VC3 | VC2 |
|---|---|---|---|---|
| P1 far VCF after forced block (freestyle) | win at all levels | **`-M4`** (28) ✗ | `+M7` (143) | `+M7` (126) |
| P2 open three | win | `+M3` (10) | `+M3` (10) | `+M3` (10) |
| P3 quiet | VC4/VC3 loss | `-M2` (699) | `-M4` (868) | `-M10` (86K) |
| P4 counter-four, block without four | VC4 loss | `-M2` (21) | `-M8` (4130) | `439` (259) |
| P5 defender counter open three | no attacker win | `-M2` (39) | `-M4` (71) | `-108` (614) |
| P6 defender faster VCF | no attacker win | `-M2` (56) | `-M4` (94) | `-M8` (303) |
| P7 double three after forced block (item 1b) | VC4 loss, VC3 win | **`+M7`** (20) ✗ | `+M7` (15) | `+M7` (15) |
| P8 = P1 in renju, white attacks | win at all levels | **`-M4`** (28) ✗ | `+M7` (219) | `+M7` (185) |

Cells are `eval (nodes)`; ✗ marks a wrong result. Every search took ≤ 51 ms (P3 VC2), most ≤ 2 ms.

## Item 1 — VC4 fours over the whole board

**Decision note.**
- **What:** new `ExtraArgs<QVCF>::fullBoardVCF` (default `false`), stored in `MovePicker`. In
  `QVCF_MOVES`, when set, generate fours with `generateVCFMoves(rule, …)`
  (`generate<VCF>` / `generate<VCF | RULE_RENJU>`) over the whole candidate area instead of
  `generateNeighbors` around the side's last stone. `vcfsearch` sets it only for `vcfOnly`
  (VCN on, attacker to move, level VC4). Forbidden points are still dropped by `pickNextMove`.
- **Expected:** VCN off: no change (`fullBoardVCF` is `false` everywhere except under `vcfOnly`),
  so `bench` stays bit-identical. VC4: P1 and P8 become wins. VC3/VC2 change only where their
  subtrees reach VC4 attacker nodes after a pass. VC4 node counts can grow (more fours per
  node), measured with the table above.
- **Measured:** `bench`, `rapfi_tests`, the position table re-run.

**Tests (written before the fix; both search/picker tests failed before it):**
- `movePickerQvcf_fullBoardOff_onlyFoursNearLastMove`: the ordinary QVCF picker still excludes a
  four far from the side's last stone (locality preserved for ordinary search).
- `movePickerQvcf_fullBoardOn_includesFoursFarFromLastMove`: with `fullBoardVCF` every four on the
  board is generated, and nothing that is not a four.
- `vcnSearch_vc4_vcfFarFromLastMove_found`: `YXVCN 4` on P1 returns a mate for the attacker.
  The fixture asserts no `B_FLEX4` / `C_BLOCK4_FLEX3` / `F_FLEX3_2X` for black after the forced pair,
  so `quickWinCheck` cannot produce the win on its own.

**Results (2026-10-06, gcc 15.2.0, Release, AVX512+VNNI).**
1. Tests: `rapfi_tests` 50/50 pass; `--check-asserts-live` aborts (exit 134), so asserts are live.
2. `bench` bit-identical: Nodes `16733809`, Hash `4621a98a`.
3. Position table, rows that changed (all others identical in result and nodes):

| Position | Level | Before | After |
|---|---|---|---|
| P1 far VCF (freestyle) | VC4 | `-M4` (28) ✗ | `+M7` (220) |
| P8 far VCF (renju) | VC4 | `-M4` (28) ✗ | `+M7` (304) |
| P3 quiet | VC2 | `-M10` (86K) | `-M10` (89K) |
| P4 counter-four | VC3 | `-M8` (4130) | `-M8` (4628) |

The P3/P4 rows search more nodes because their subtrees reach VC4 attacker nodes after a defender
pass, which now see every four. P7 (item 1b) is unchanged at `+M7`, still wrong at VC4.

## Item 1b — VC4 must not count a double three as a win

**Decision note.**
- **What:** `quickWinCheck<R>(board, ply, beta, vcfOnly = false)` in `game/wincheck.h`. With
  `vcfOnly` the `F_FLEX3_2X` rule is skipped. The `A_FIVE`, `B_FLEX4` and `C_BLOCK4_FLEX3` rules
  stay: each wins through a four at every attacker step, so they are VCF wins. `vcfsearch` passes
  its `vcfOnly` (VCN on, attacker to move, VC4). Chosen over a check inside `vcfsearch` because
  the double-three and four-three rules return the same `mate_in(ply + 5)` and cannot be told
  apart after the call.
- **Expected:** every other caller (AB `search()`, MCTS, the runtime-rule wrapper) keeps the
  default, so `bench` stays bit-identical. P7 VC4 becomes a loss; P7 VC3/VC2 stay `+M7` (there a
  double three is a real win: pass, then open four).
- **Measured:** `bench`, `rapfi_tests`, position table re-run against the item 1 results.

**Tests (written before the fix):**
- `quickWinCheck_doubleThree_default_reportsWin`: pins the existing behaviour for non-VCN callers.
- `quickWinCheck_doubleThree_vcfOnly_noWin`: must fail before the fix.
- `quickWinCheck_fourThree_vcfOnly_stillWin`: the `C_BLOCK4_FLEX3` rule survives.
- `quickWinCheck_flexFour_vcfOnly_stillWin`: the `B_FLEX4` rule survives.
- `vcnSearch_vc4_doubleThreeAfterForcedBlock_attackerLoses`: P7 as a unit test, must fail before
  the fix. Fixture asserts that black has a double three and no four after the forced pair.
- `vcnSearch_vc3_doubleThreeAfterForcedBlock_attackerWins`: VC3 still wins (regression).

**Results (2026-10-06, gcc 15.2.0, Release, AVX512+VNNI).**
1. Before the fix `quickWinCheck_doubleThree_vcfOnly_noWin` and
   `vcnSearch_vc4_doubleThreeAfterForcedBlock_attackerLoses` failed; the other four passed.
2. Tests: `rapfi_tests` 56/56 pass; `--check-asserts-live` aborts (exit 134).
3. `bench` bit-identical: Nodes `16733809`, Hash `4621a98a`.
4. Position table against the item 1 state, rows that changed (all others identical):

| Position | Level | Before | After |
|---|---|---|---|
| P7 double three after forced block | VC4 | `+M7` (20) ✗ | `-M4` (28) |
| P1 far VCF (freestyle) | VC4 | `+M7` (220) | `+M7` (304) |

P1 VC4 searches more nodes because inner VC4 nodes no longer stop at a double three. No row is
wrong any more.

## Item 4 — pruning inside VCN (closed: ordinary pruning kept)

**Outcome (2026-10-06).** Ordinary pruning stays in VCN (user decision, cost). One change kept:
no "singular root" early stop while VCN is on.

**What was tried.** A candidate turned off, inside VCN, every value shortcut (razoring, futility,
singular multi-cut) and depth cut (IIR/IID, singular negative extension, fail-high reduction, LMR)
on both sides, plus Step 12 move skipping at attacker nodes. Measured on 270 freestyle self-play
positions (engine self-play at depth 4, positions every 4 plies from ply 12), `YXVCN 3`,
`MAX_DEPTH 8`, one thread, 60 s cap:

| | Ordinary pruning | Everything off |
|---|---|---|
| Positions with `+M` | 110 | 122 (12 more; ordinary search confirmed the two checked) |
| Total search time | 49 s | 471 s (~10×) |
| Median / p90 per position | 0 ms / 5 ms | 0 ms / 774 ms |
| p99 / max | 162 ms / 47 s | 59 s / 59 s (4 positions hit the 60 s cap) |

No position had `+M` with ordinary pruning but not without it. The candidate was reverted.

**Kept: no singular-root early stop in VCN** (`ABSearcher::search`, iterative deepening).
- With a time limit, a root with a single non-losing move (e.g. a forced block) stopped iterating a
  few depths after that was detected, before `MAX_DEPTH`. A forced root move says nothing about
  whether the attack still wins.
- Evidence: sweep position #253: eval `270` at depth 6 with the stop, `+M9` without it; an
  ordinary search after the forced pair (4,8)/(5,7) also proves the win.
- Cost: only roots with a single non-losing move iterate further. Position table: no result
  changed; nodes rose on forced-root / mate rows (P1 VC4 304→408, P4 VC2 259→1713 with eval
  439→644, P8 VC3 219→477); every row still ≤ 2 ms.
- Test: `vcnSearch_vc3_forcedRootMoveWithTimeLimit_winFound` (#253, 60 s turn time so the stop
  rules are live). Fails with the stop restored, passes without it.
- `rapfi_tests` 57/57, `bench` bit-identical (`16733809` / `4621a98a`).

## Item 6 — attack move ordering for the VCN attacker

**Decision note.**
- **What:** `ExtraArgs<MAIN>::vcnAttackOrder` (default `false`), set by `search()` at VCN attacker
  nodes that are not facing a five. In `MovePicker`'s `MAIN_MOVES` scoring (new score flag
  `VCN_ATTACK`) each move gets a bonus by the threat strength of its own pattern: fours
  (`E_BLOCK4..B_FLEX4`) > threes (`H_FLEX3..F_FLEX3_2X`) > twos (`L_FLEX2..I_BLOCK3_PLUS`) > none.
  Moves are only reordered, none added or removed; scores of non-threat moves are unchanged, so
  the policy-pruning thresholds that read `curMoveScore()` see the same values for them and
  only higher values for threat moves. Reason: the ordinary order follows the network, which can
  be blind to exactly the attacks VCN is for.
- **Expected:** VCN off: no change (`bench` bit-identical). VCN on: same or more wins at the same
  depth, fewer nodes where the attack is found sooner; no change in the set of searched moves
  except through the existing pruning (a threat move is pruned less often).
- **Tuning note:** the bonus sizes are constants (policy scores are `int16`, observed raw range
  about -111..387 on the acceptance position). Chosen to put the tiers clearly above the policy
  range, not tuned; the tier order is the design, the sizes are not.
- **Measured:** fixed-depth VC3 sweep over the 270 self-play positions (`tests/vcn_selfplay.sh`,
  `tests/vcn_sweep.sh`): wins, total nodes and time against the current order; the acceptance
  position; position table.

**Tests (listed before code):**
- `movePickerMain_vcnAttackOrderOn_fourThenThreeThenRest`: in a position with a four, a three
  and a stronger-looking defence move, the four comes before the three, which comes before the
  rest.
- `movePickerMain_vcnAttackOrderOff_orderUnchanged`: with the flag off the order equals the
  current one (no leakage into ordinary search).
- `movePickerMain_vcnAttackOrderOn_sameMoveSet`: the set of generated moves is identical with
  the flag on and off.

**Results (2026-10-06).**
1. Tests: `rapfi_tests` 60/60 pass, `--check-asserts-live` aborts (exit 134). The ON test passes with
   the change and fails with the ordering switched off. `bench` bit-identical (`16733809` /
   `4621a98a`).
2. Sweep: 270 freestyle self-play positions, `YXVCN 3`, `MAX_DEPTH 8`, one thread
   (`tests/vcn_selfplay.sh`, `tests/vcn_sweep.sh`). "Old" = same build with the ordering off.
   Unit tests cannot show the network effect (no network in the test binary; with the classical
   scorer the ordinary order already follows the tiers except for a few defence squares).

| | Old order | New order |
|---|---|---|
| NNUE: positions with `+M` | 147 | 149 |
| NNUE: total nodes / search time | 40.0M / 23.6 s | **27.3M / 16.9 s** (-32% / -28%) |
| Classical: positions with `+M` | 110 | 108 |
| Classical: total nodes / search time | 93.8M / 62.4 s | 91.6M / 62.1 s (-2%) |

   (Classical: 40 positions are answered with no search info, identical in both modes.)
3. Result class changed on 12 NNUE and 5 classical positions, both directions. NNUE: new order
   gained 6 wins (#45, #118, #132, #192, #194, #215), lost 4 (#46, #119, #135, #207); classical:
   lost 2 (#19, #117), gained none. Re-checking the lost ones with the new order at deeper depth:
   NNUE #46 (depth 12), #119 / #135 / #207 (depth 10) and classical #19 (depth 10) all find the win,
   so those are depth effects. **Classical #117 does not**: old `+M11` at depth 8, new order `466`
   even at depth 12. This is a real miss caused by the new order (cause not diagnosed).
4. Acceptance position `h8j9f6i10` (NNUE, Standard, 8 threads, 60 s, `YXVCN 2`), one run each:
   new order depth 28, eval `278`, G5 ✓; old order depth 31, eval `13`, G5 ✓. Both pick G5; the
   `278` is within the run-to-run spread seen before (`-5`..`515`) and is not > 600.

**Conclusion.** With the network, threat ordering cuts VCN search cost by about a third and does
not reduce wins on this set (149 vs 147); it does not lift the acceptance value above 600. With
the classical scorer it changes almost nothing. Evidence is one 270-position fixed-depth set;
the win counts differ by 2 either way, so it shows a speed gain, not a win-rate gain. Kept
(user may veto), with #117 recorded as a known regression, diagnosed below.

### Diagnosis of the known regression #117 (2026-10-06)

Classical evaluator, freestyle, `YXVCN 3`, `MAX_DEPTH 8`, one thread. Old order: `+M11` in 2225
nodes (root move (8,5), line `I6 F8 K6 G6 J7 D8 E8 Pass`). New order: `647` (root move (8,4)),
and still no mate at depth 12.

- **What the order does to the winning line.** (Temporary test, since removed.) The root winner (8,5)
  is a tier-1 move: old rank 3, new rank 7. The attacker's winning threes inside the line move from
  rank 3 to rank 5. Nothing is removed, only moved later.
- **Which heuristic then loses it.** Temporary switches turned heuristics off inside VCN one at a time,
  new order:

| Off inside VCN | Result on #117 |
|---|---|
| none | `647`, plays (8,4) |
| LMR / razoring / futility / IIR (each alone) | no mate (`408` / `367` / `248` / `440`) |
| move-count skipping at attacker nodes (alone) | no mate, plays (8,5) (`546`) |
| move-count + razoring + futility | `+M11`, 94K nodes |
| LMR + all of Step 12 + razoring + futility + IIR | `601`, no mate |
| exempt threes/fours from Step 12 skipping | no mate (`546`) |

  No single heuristic is the cause. The mate needs move-count skipping, razoring and futility all
  to be off, at 94K nodes instead of 2.2K. This is the ordinary sensitivity of a depth-limited,
  pruned search to move order: a reorder changes which lines are proven within depth 8. It is not
  an implementation bug.
- **Is it systematic?** Not on the NNUE set: 6 positions gained, 4 lost (5 of the 6 losses found
  again at depth 10-12). On the classical set the new order lost #19 (found at depth 10) and #117.
- **Tier subsets** (same 270 positions, depth 8):

| Ordering | NNUE wins / nodes | Classical wins / nodes | #117 |
|---|---|---|---|
| none (old) | 147 / 40.0M | 110 / 93.8M | `+M11` |
| fours only | 147 / 39.1M | 109 / 97.7M | `+M15` |
| fours + threes | 149 / 27.0M | 108 / 99.6M | miss |
| fours + threes + twos (kept) | 149 / 27.3M | 108 / 91.6M | miss |

  The speed gain with the network comes from ordering the threes; fours alone gives none. So the
  gain and the #117 miss come from the same change. Fixing #117 by dropping to fours-only costs the
  whole gain, and exempting threats from Step 12 skipping does not fix it.
- **Decision:** keep fours + threes + twos (user may veto). #117 stays a known miss under the
  classical evaluator; the NNUE set (the real configuration) shows no such loss at fixed depth.
  No test is added for #117 (it is a miss we accept, not behaviour to guard).

## Acceptance position `h8j9f6i10` (user, 2026-10-06)

Black h8, white j9, black f6, white i10; black (attacker) to move. A medium position where an
ordinary search struggles to find the right move; VCN should do better. Criterion: **VCN eval
> 600**.

Current build (classical evaluator: this tree has no config and `Networks/` is empty), 10 s,
one thread:

| Rule | Ordinary search | `YXVCN 3` | `YXVCN 2` |
|---|---|---|---|
| Freestyle | `14` (depth 28) | `-M8` (4 ms) | `-27` (depth 26) |
| Standard | `-5` | `-M8` | `134` |
| Renju | `-7` | `-M8` | `-23` |

**Conditions (user, 2026-10-06):** the user's Yixin setup: standard rule (`INFO RULE 1`),
NNUE `mix9svq` + `model260809.bin` (config and weights copied from the YixinUI folder into
`build/yixin-net/`, git-ignored; run the engine from that folder so `bench` and `rapfi_tests` keep
the classical evaluator), 8 threads, `TIMEOUT_TURN 60000`, `TIMEOUT_MATCH 0`, VCN level VC2
(Yixin "checkmate in global search: vc2" = `INFO VCTHREAD 2` / `YXVCN 2`).
**Correct move: G5 (`6,4`)** (user).

With NNUE (one run each, 8 threads, so not reproducible run to run):

| Time | Search | Depth | Eval | Move |
|---|---|---|---|---|
| 10 s | ordinary | 28 | `32` | J8 ✗ |
| 10 s | `YXVCN 2` | 29 | `10` | J8 ✗ |
| 10 s | `YXVCN 3` | 30 | `-M8` | — |
| 60 s | ordinary | 30 | `684` | G5 ✓ |
| 60 s | `YXVCN 2` | 32 | **`515`** ✗ (< 600) | G5 ✓ |

The YixinUI `engine` binary (not a VCN build: it ignores `INFO VCTHREAD`) gives `17` / J8 at 10 s.

**Re-run, 3 × each at 60 s (same conditions):**

| Run | Ordinary: depth / eval / move | `YXVCN 2`: depth / eval / move |
|---|---|---|
| 1 | 35 / `21` / J8 ✗ | 35 / `9` / G5 ✓ |
| 2 | 35 / `11` / J8 ✗ | 34 / `-5` / G5 ✓ |
| 3 | 35 / `39` / G5 ✓ | 33 / `6` / G5 ✓ |

Over all four 60 s runs: VCN played G5 4/4, the ordinary search 2/4. The first run's `684` /
`515` did not repeat (the re-runs reached depth 33–35 against 30–32 and settled near 0), so it was
an outlier. With 8 threads the last printed depth line may come from a different thread than the
played move, so its eval is indicative only. No run meets eval > 600.

**Gap:** VCN plays the right move at 60 s more reliably than the ordinary search, but its value
(near 0 in the re-runs) is far below the criterion. That is expected from the current design: the VCN defender has
one more option (pass), so with the same evaluation VCN can only score lower. Reaching > 600
needs item 5 to score "attack still alive" above the positional eval, not only "no attack"
below it. That bonus is deferred (needs SPSA tuning, user 2026-10-06), so for now this position
is tracked by the move (G5), not by the value.

## Function → test-case list

Done: see the test lists under §Item 1, §Item 1b and §Item 4. Pending, to be finalised per slice:
- Item 5: `vcnSearch_vc2_noAttackLeft_strongNegativeEvenWithPositiveEval`,
  `vcnSearch_vc2_attackStillAlive_keepsEval`.
- Item 6: `movePickerMain_vcnAttacker_threatMovesFirst`.
- Item 3 (if done): `movePickerMain_vcnDefenderQuiet_passThenDefenceSquares`.
- Acceptance: `h8j9f6i10` once its conditions are known.
- Regression for real-world handling: `vcnSearch_vc4_counterFourBlockedWithoutFour_attackerLoses`
  (P4), `vcnSearch_vc3_defenderCounterOpenThree_*` (P5), `vcnSearch_vc3_defenderFasterVcf_*` (P6).
- Dropped (width restriction rejected): `movePickerMain_vcnAttackerVc3_onlyFoursAndThrees`,
  `movePickerMain_vcnAttackerVc2_includesTwos`, `basicPatternFilter_vct/vc2_boundaries`.

## Verification

- Tests built with asserts live (`rapfi_tests`, `-UNDEBUG`).
- `bench` bit-identical with VCN off.
- Node/time/result comparison against the measurement baseline above, per slice.
