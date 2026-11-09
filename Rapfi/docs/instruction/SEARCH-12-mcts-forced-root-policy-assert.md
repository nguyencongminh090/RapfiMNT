# SEARCH-12 — execution guidance

Task: [`../todo/SEARCH-12-mcts-forced-root-policy-assert.md`](../todo/SEARCH-12-mcts-forced-root-policy-assert.md).
Class C (plumbing flag). Written 2026-11-09 from the todo and a read of `movepick.cpp`.

## What to change

In the ROOT `MovePicker` constructor (`search/movepick.cpp:207`), the three one-hot branches
(own five `:227`, opponent five `:232`, own flex four `:237`) call `markFirstMoveOneHot` under
`if (useNormalizedPolicy)` and never set `hasPolicy`. Set `hasPolicy = true` right after each
`markFirstMoveOneHot`, inside the same `if`. Nothing else in production code.

## Approach

1. **Baselines from HEAD first**, outputs under `build/search12/`, not in the repo.
   - `echo BENCH | ./pbrain-rapfi` must report Nodes `17621937`, Hash `8c1f72e5`.
   - MCTS golden output on non-forced roots (same recipe and position set as SEARCH-09/10: single
     thread, fixed node count, Standard; save root move, visits, Q, value). Reuse the SEARCH-09
     scripts if they are still in `build/search09/` or `build/search10/`.
2. **Write the todo's five tests first and watch them fail.** Put all five in
   `tests/mcts_vcn_tests.cpp`. No test file builds a ROOT picker yet. This file already has the
   stand-in evaluator (`HashEvaluator`, `threadWithEvaluator(rule)`, about lines 198 and 418), the
   board helpers, and `RUN_PURE_TEST` / `RAPFI_TEST_ISOLATE`. No new file and no
   `TEST_DRIVER_SOURCES` change are needed. Rules:
   - Each test builds its own `Board` with the exact position (a real five, a real flex four for
     the side to move or the opponent). Build `ExtraArgs<ROOT>{true, 1.0f}` (field order:
     `useNormalizedPolicy`, `normalizedPolicyTemp`).
   - Assert `hasPolicyScore()`, `hasNormalizedPolicy()`, and the one-hot content: the first move
     has `curMoveNormalizePolicy() == 1.0f`, every other has `0.0f`. Read `curMoveNormalizePolicy()`
     after each `mp()` call; it is set only while iterating (`movepick.cpp:416`). The own-five and
     own-flex-four cases are inferred from code; the tests are what prove them.
   - The opponent-flex-four test goes through `scoreAllMoves<POLICY>`, which sets `hasPolicy` only
     when an evaluator is loaded (`movepick.cpp:471`). Bind the board to `threadWithEvaluator(rule)`
     so the evaluator path runs, and assert `hasPolicyScore() == true`. This branch is not changed
     by the fix: the test pins current behaviour and passes before and after.
   - The three `movePickerRoot_*` one-hot tests only check return values, so before the fix they
     report `[FAIL]` without aborting. They do not need isolation.
   - `mctsSearch_rootOpponentFive_assertBuild_completesWithBlockMove`: a short fixed-node MCTS
     search from an opponent-five root returns the only block. `rapfi_tests` forces `-UNDEBUG`, so
     this is what aborts today (through `expandNode<true>`, `search/mcts/search.cpp:596`, then
     `Node::createEdges`). A full search is engine-driven and goes through `RUN_TEST`, not
     `RUN_PURE_TEST`, so `RAPFI_TEST_ISOLATE=1` does not fork it: before the fix it aborts the whole
     driver. For the red run, either run it alone (comment out or `RAPFI_TEST_SKIP` the rest), or
     add a pure companion test that drives `Node::createEdges` with the ROOT picker on that board
     under `RUN_PURE_TEST`, which reports `[FAIL]` in isolate mode. Say which you used in the todo.
   - Record in the todo that the new tests failed (or aborted) before the fix.
3. **Implement** the three one-line changes.
4. **Check the AB side, as the todo requires.** `hasPolicyScore()` is read at
   `search/ab/search.cpp:1079` (policy reduction) and `:1638` (policy pruning), both on the MAIN
   picker, and at `search/mcts/node.cpp:167` and `search/mcts/search.cpp:559,574` (asserts only).
   The MCTS ROOT picker is built in `expandNode<true>` (`search/mcts/search.cpp:596`). AB ROOT pickers
   (`searchengine.cpp:67,98`, `command/gomocup.cpp:877`) pass the default `useNormalizedPolicy =
   false`, so the new `hasPolicy = true` is unreachable there. Grep once more before editing in
   case that changed. If any AB path does read it for a ROOT picker, record it in the todo.
5. **Verify** (Definition of done), then rebuild the release with
   `scripts/pgo_build.sh x64-clang-AVX512VNNI` only if the bench check needs a PGO binary
   (a plain `ninja` build is only a compile check).
6. **Smoke the protocol path.** Since VCN-03 slice 5 this bug is reachable from an ordinary
   `YXVCN` query in an assert build (`docs/todo/VCN-03-mcts-vcn.md:665`). Build an engine with
   `-DFORCE_ASSERTION=ON` in a separate build directory (debug run only, never used for the bench),
   send an MCTS `YXVCN` query on an opponent-five root, and confirm it answers the block instead
   of aborting. Record the command and output in the todo. Never put `-UNDEBUG` on `rapfi`.

## Pitfalls

- Fix it in the picker, not in `createEdges` (CLAUDE.md rule 4). Do not remove or weaken the
  assert, and do not add a `hasPolicy` write outside the `if (useNormalizedPolicy)` guard.
- Only the `hasPolicy` flag is wrong. The one-hot policy values are already right; do not change
  the move generation, ordering or policy values. Release behaviour must stay identical.
- MCTS is not covered by BENCH. A green bench only shows AB and shared code were not disturbed.
  The unit tests and the golden diff are the real guards.
- `rapfi_tests` forces `-UNDEBUG`. Never do that to the `rapfi` target.
- The other ROOT branches also run `scoreAllMoves<POLICY>`, which sets `hasPolicy` only when an
  evaluator exists. A ROOT picker without an evaluator would hit the same assert for ordinary
  roots. That is out of scope; if the tests reveal it, record it as a new `TODO.md` item and do
  not fix it here.

## Don't touch

- `Node::createEdges` and its asserts, the MAIN and QVCF picker constructors (`DEFENDFIVE_MOVES`
  already sets the flag), `markFirstMoveOneHot`, `scoreAllMoves`.
- SEARCH-11 (policy partial sort), SEARCH-13, SEARCH-14, and the alpha-beta search.
- The existing tests in `tests/mcts_vcn_tests.cpp` that avoid forced roots because of this bug:
  the smoke run's `assert(... && "smoke: forced root")` (about lines 1379-1392) and the golden set
  whose roots have "no five or flex four (SEARCH-12)" (about line 1433). Leave them as they are. In
  the todo, note that after the fix they could be widened to forced roots, as a possible follow-up.
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`): read only, never edit or
  commit.

## Definition of done

- All five tests pass in `rapfi_tests` with asserts live, the full suite is green, and the new
  tests failed before the fix (record that in the todo).
- Bench `17621937 / 8c1f72e5`. MCTS golden output on non-forced roots identical.
- The `YXVCN` smoke (step 6) answers on an opponent-five root in an assert build.
- Todo: status updated, results written, the AB `hasPolicyScore()` check recorded. `TODO.md` line
  and this file's `instruction.md` line moved to `## Done` with the date and results.
- Commits in conventional style: `test:` (failing tests), `search:` (fix), `docs:` (results).
  Branch first if on `main` and the user asked for commits; otherwise leave the changes
  uncommitted and report.
