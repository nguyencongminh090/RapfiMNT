# SEARCH-15 — execution guidance (investigation)

Task: [`../todo/SEARCH-15-mcts-vcn-forced-root-unproven.md`](../todo/SEARCH-15-mcts-vcn-forced-root-unproven.md).
Class B, investigation only. Written 2026-11-09 from the todo, the Results of SEARCH-13, VCN-03
(§3.2 pass edge, §4.1 bounds, slice 5) and a read of `search/mcts/node.{h,cpp}` and
`search/mcts/search.cpp`.

## Goal

Say **why** MCTS does not prove P4 VC3 (AB: `-M8`) and P4 VC2 (AB: `+M13`) at a forced root, with
data, and name the smallest fix that follows. **This task changes no production code.** Its
deliverable is a findings section in the todo plus, if the cause is clear, a new todo (next free
`SEARCH-nn`) with a decision note and a function → test-case list.

P4 = `positions.py` entry `P4-counterFourBlockWithoutFour` (`build/search13/partc/positions.py`;
the same position is in `tests/mcts_vcn_tests.cpp` around line 1770): black to move, one legal root
move (block), real network (`cfg_vcn`) for the Part C numbers, `HashEvaluator` for unit tests.

## Approach

1. **Reference line from AB.** Run AB on P4 at `YXVCN 3` and `YXVCN 2` (any depth that gives
   `-M8` / `+M13`). Record the PV(s) and, per ply, who moves and the VCN level (a defender pass
   raises the level by one; see `Vcn::childLevel`). This is the proof MCTS has to find. Save under
   `build/search15/`.
2. **Walk the AB line in the MCTS tree.** Run MCTS on P4 with a fixed node budget (try 1M, 10M, 100M;
   1 thread, real network). After the search, follow AB's PV from the root through the node table
   and print for each node on the line: edge count, whether the AB move is a kept edge (or was cut
   at 0.99), its prior, visits, value, `getBound()` (lower/upper, terminal or not), and for the
   defender nodes whether a pass edge exists and its prior/visits. Find the **first node on the
   line where the MCTS tree differs from what a proof needs**:
   - move missing (cut) or visited far too little (prior);
   - pass edge missing or starved (pass prior, VCN-03 §3.2);
   - a leaf that AB scores as terminal but MCTS does not (VC4/VC5 terminal, `SimpleVCF`,
     `search.cpp` ~660-720);
   - all children proven but the parent bound not terminal (bound propagation,
     `Node::updateBound` around `node.cpp:253-295`, `computeSelectionValues` ~939-979, the
     `-upper` / `-lower` sign in `ValueBound::operator|=`);
   - a transposition/graph-reuse effect (same position reached with a different VCN level: the key
     XOR of VCN-03 §3.1 should prevent it).
   Diagnostic code is allowed only in a throw-away form: a test in `tests/` that is not committed, or
   a patch kept under `build/search15/` and not applied to the repo. Remove it before reporting.
3. **Confirm the suspect with one controlled run.** Change one thing via an existing config option
   or a local uncommitted tweak (for example edge cut off, larger pass prior, `mcts_vcn_*` options),
   rerun step 2 at the same budget, and show whether P4 VC3 / VC2 become proven. A tweak that proves
   it is evidence, not a fix: do not commit it.
4. **Also check smaller VCN levels and depth.** If VC2 `+M13` is proven by AB at depth 13, check
   whether MCTS needs more than the nodes it reached (VC2 reached 24M-950M, VC3 93M-2144M): estimate
   from the tree whether the proof line is simply too deep/narrow for visit-driven search. "Needs a
   different algorithm" (for example a proof-number style solver, which VCN-03 already rejected as
   the VCN searcher) is a valid finding.
5. **Write up.** In the todo: findings (first divergence, evidence with numbers, the controlled
   run), a recommended fix with its risk, and whether it is worth doing given that AB stays the VCN
   searcher. Update `Status:`.

## Pitfalls

- Reading the tree after the search: nodes are in the shared node table, edges may be reclaimed or
  replaced (SEARCH-10 re-adds cut moves; widening replaces the edge array). Read single-threaded
  after `Engine` has stopped; do not run the dump during the search.
- Compare equal VCN levels. AB's `-M8` is for the level-3 search from the root; mixing levels gives
  false "differences".
- 8-thread runs are not reproducible; use 1 thread for the investigation.
- Mate values: MCTS reports proven results through bounds; a plain score with non-terminal bound is
  "unproven", never "wrong" (VCN-03 slice 5 classification).
- Do not use `-UNDEBUG` on the `rapfi` target. `rapfi_tests` already forces it.
- MCTS is not covered by BENCH; a green bench says nothing about this task.

## Don't touch

- All production code under `search/`, `eval/`, `game/`, `command/`. No config default changes.
- SEARCH-11, SEARCH-04, PERF-03 and their files.
- `build/vcn03s5/`, `build/search13/`, `build/search14/` (read and copy only).
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`): read only.

## Definition of done

- The todo has a findings section: AB reference line, first divergence in the MCTS tree with
  numbers, and the result of the controlled run (or an honest "not determined" with what was tried).
- Production tree has zero diff (`git diff --stat -- search eval game command` is empty); no
  diagnostic code left in `tests/`.
- If a fix is recommended: a new todo and `TODO.md` entry exist with a decision note and test list
  (write-before-code). If not worth fixing: say so and set SEARCH-15 Done with the reason.
- No bench or golden needed, since no production code changes. Say so in the report.
- Commit only if the user asks: `docs:` for the findings, on the current branch.
