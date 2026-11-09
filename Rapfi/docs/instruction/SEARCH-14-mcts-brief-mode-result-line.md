# SEARCH-14 — execution guidance

Task: [`../todo/SEARCH-14-mcts-brief-mode-result-line.md`](../todo/SEARCH-14-mcts-brief-mode-result-line.md).
Class C (output surface). Written 2026-11-09 from the todo and a read of
`search/searchoutput.cpp`, `search/mcts/search.cpp` and `search/ab/search.cpp`.

## What to change — pick this design

Neither option in the todo works as written:

- **Calling `printSearchEnds` from MCTS** changes `NORMAL`: that function prints its
  `Speed … | Depth … | Eval … | Node … | Time …` line in both `NORMAL` and `BRIEF`
  (`searchoutput.cpp:230`). It also needs a `rootDepth` that MCTS does not have, and its Bestline
  fallback to `previousPv` is wrong for MCTS (there `previousPv` is the pv of the previous root
  update, `mcts/search.cpp:1625`, not of a previous iteration).
- **A BRIEF branch in `printRootMoves`** prints every `timeToPrintMCTSRootmoves` (1000 ms) or
  `nodesToPrintMCTSRootmoves` during the search (`mcts/search.cpp:1462-1486`), not once at the end.

So: add a new `SearchPrinter` method, e.g. `printMCTSSearchEnds(SearchThread &th, const TimeControl
&tc)`, that prints only when `messageMode == MsgMode::BRIEF`, and call it once from
`MCTSSearcher::search(SearchThread &th, …)` right after the final `printRootMoves`
(`mcts/search.cpp:1419`, main thread, after all workers returned). Output, two `MESSAGE` lines in
AB's words:

```
MESSAGE Speed <speedText> | Eval <rootMoves[0].value> | Node <nodesText> | Time <timeText>
MESSAGE Bestline <MovesText{rootMoves[0].pv}>
```

No `Depth` field (MCTS has no iteration depth; do not print a fake one). If you want the selective
depth, use `SelDepth <rootMoves[0].selDepth>` and say so in the todo. Use `rootMoves[0].pv` only, no
`previousPv` fallback. In ponder, return early like `printRootMoves` does (all MCTS output is
silent in ponder); record that this differs from AB's `[Pondering]` prefix.

## Approach

1. **Baselines from HEAD first**, outputs under `build/search14/`, not in the repo.
   - Bench: Nodes `17621937`, Hash `8c1f72e5`.
   - Captured protocol output for {AB, MCTS} × {`BRIEF`, `NORMAL`, `UCILIKE`} on one ordinary
     position (and one `YXVCN 3` query under MCTS), fixed node budget, 1 thread. Set the mode with
     `message_mode` in a copied config dir. Mask time-dependent fields (`Speed`, `Time`, `tm`,
     `n/ms`) with `sed` before diffing; everything else must be stable at a fixed node count.
2. **Write the todo's three tests first and watch the first fail.** Put them in
   `tests/mcts_vcn_tests.cpp` (it has `MctsEngineFixture`, `runMctsSearch`, `ScopedCoutCapture`
   from `tests/testutil.h`, and `ScopedNormalMessages` about line 1471; add a general
   `ScopedMessageMode(MsgMode)` next to it rather than a second copy).
   - `mctsSearch_briefMode_printsResultLineLikeAb`: `BRIEF`, fixed-node MCTS search, capture `cout`
     (note `runMctsSearch` already captures and discards; add a variant that returns the string).
     Assert exactly one line starting `MESSAGE Speed ` containing `| Eval ` and `| Node `, and one
     `MESSAGE Bestline ` line whose moves start with `rootMoves[0].pv[0]`. Before the fix there is
     no such line.
   - `mctsSearch_normalMode_outputUnchanged`: `NORMAL`; assert no `MESSAGE Speed … | Eval` line and
     no `MESSAGE Bestline` line, and the existing `(1) …` and `Speed … | Visit …` lines are there.
     Do not compare bytes in a unit test (time fields); the byte check is the masked diff of step 5.
   - `abSearch_briefMode_outputUnchanged`: AB, `BRIEF`, fixed depth (see `runVcn` / the AB tests in
     `tests/vcn_tests.cpp` for a direct AB search); assert one `Speed … | Depth …` line and one
     `Bestline` line. Passes before and after.
   - Restore the message mode and the searcher in every test (fixtures), or later tests break.
   - Record in the todo that the first test failed before the fix.
3. **Implement**: declaration in `searchoutput.h`, body in `searchoutput.cpp` next to
   `printSearchEnds`, one call in the MCTS searcher.
4. **Verify** (Definition of done).
5. **Output diffs**: rerun step 1's captures, masked. Expected: MCTS `BRIEF` gains exactly the two
   lines per search (ordinary and `YXVCN`); MCTS `NORMAL` / `UCILIKE` and all AB modes identical.
   Put the before/after line counts and the diff of the MCTS `BRIEF` case in the todo. This is an
   intended output change with VCN off; say so in the todo.

## Pitfalls

- `NORMAL` and `UCILIKE` must stay byte-identical (after masking). A GUI gets `NORMAL`
  (`setGUIMode`, `command/gomocup.cpp:165`) and the shipped configs use `ucilike`; a change there
  can break parsers.
- Do not touch the `INFO …` (`showInfo`) output of `printRootMoves`; GUIs read it.
- Selfplay forces `BRIEF` (`command/selfplay.cpp:971`) and may run MCTS: the new lines appear in
  selfplay logs. Check that nothing parses those logs line by line (grep `scripts/`, `tests/*.sh`);
  if something does, record it rather than special-casing selfplay.
- Print from the main thread only, once per search, after the workers have returned. Do not print
  from the periodic block in `MCTSSearcher::search(SearchThread &)`.
- Search behaviour is unchanged; bench and the MCTS golden only guard against accidents.

## Don't touch

- `printSearchEnds`, `printRootMoves`, `printDepthCompletes`, AB's output, the `INFO` output.
- SEARCH-11, SEARCH-13.
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`): read only, never edit or
  commit.

## Definition of done

- The three tests pass in `rapfi_tests` with asserts live, the full suite is green, and the first
  test failed before the fix (recorded in the todo).
- Bench `17621937 / 8c1f72e5`.
- Masked output diffs recorded: only MCTS `BRIEF` changed, by the two lines.
- Todo: status updated, results and the ponder/no-Depth choices written. `TODO.md` line and this
  file's `instruction.md` line moved to `## Done` with the date and results. The comment on
  `ScopedNormalMessages` ("MCTS prints its root-move lines in NORMAL / UCILIKE only") stays true;
  update it only if it becomes misleading.
- Commits in conventional style: `test:` (failing tests), `search:` (fix), `docs:` (results).
  Branch first if on `main` and the user asked for commits; otherwise leave the changes
  uncommitted and report.
