# SEARCH-14 — MCTS prints no result line in BRIEF message mode

**Status:** done 2026-11-09 (recorded 2026-11-09, user). Found in VCN-03 slice 5 (§9 "Slice 5",
finding). Not VCN-specific. First test failed before the fix, all three pass after, bench
unchanged, masked output diffs show only MCTS `BRIEF` changed, by two lines per search.

**Class:** C (output surface).

## Problem

- In the built-in default message mode `BRIEF`, an MCTS search prints no result line at all.
  Alpha-beta prints `Speed … Eval … | Bestline`.
- Cause, from VCN-03 slice 5:
  - `printSearchEnds` is called only by AB;
  - `printRootMoves` prints only in `NORMAL` / `UCILIKE` (`search/searchoutput.cpp:230-241`).
- Impact is limited:
  - a GUI gets `NORMAL` (`setGUIMode`), and the shipped configs use `ucilike`, so only a bare
    `BRIEF` session (for example piping commands by hand) is affected;
  - both ordinary MCTS and MCTS-VCN are affected.

## Decision note (draft)

- **What:** make MCTS emit one end-of-search line in `BRIEF`, in AB's format (speed, eval, best
  line), either by calling `printSearchEnds` from the MCTS searcher or by a BRIEF branch in
  `printRootMoves`. Pick the one that does not change `NORMAL` / `UCILIKE` output.
- **Expected:** `BRIEF` output under MCTS gains one line. `NORMAL` / `UCILIKE` are byte-identical.
  This is an intended output change with VCN off, so record it.
- **How measured:** captured output diffs per message mode before/after, for AB and MCTS; bench
  (search unaffected).

## Function → test-case list (write before code)

- `mctsSearch_briefMode_printsResultLineLikeAb`
- `mctsSearch_normalMode_outputUnchanged`
- `abSearch_briefMode_outputUnchanged`

## Verification

- Bench `17621937 / 8c1f72e5`. Unit tests with asserts live. Output diffs recorded.

## Results (2026-11-09)

**Design (as in the instruction file).** New `SearchPrinter::printMCTSSearchEnds(th, tc)`
(`search/searchoutput.{h,cpp}`), called once from `MCTSSearcher::search(SearchThread &, ...)` right
after the final `printRootMoves` (main thread, after all workers returned). `printSearchEnds`,
`printRootMoves`, `printDepthCompletes`, the `INFO` output and AB were not touched. It prints only in
`MsgMode::BRIEF`:

```
MESSAGE Speed <speed> | Eval <rootMoves[0].value> | Node <nodes> | Time <time>
MESSAGE Bestline <rootMoves[0].pv>
```

- **No `Depth` / `SelDepth` field.** MCTS has no iteration depth and AB's `Depth d-sd` would need a
  fake `d`; `SelDepth` was left out too to keep the line a strict subset of AB's words (the
  selective depth is still in the NORMAL / UCILIKE root-move lines and in `INFO SELDEPTH`).
- **Bestline is `rootMoves[0].pv` only**, no `previousPv` fallback (in MCTS `previousPv` is the pv
  of the previous root update, not of a previous iteration).
- **Ponder: silent** (early return like `printRootMoves`). This differs from AB, which prints
  `[Pondering] Speed ...`. Also silent if `rootMoves` is empty.

**Tests** (`tests/mcts_vcn_tests.cpp`; new general `ScopedMessageMode(MsgMode)`, with
`ScopedNormalMessages` now a thin subclass of it):

- `mctsSearch_briefMode_printsResultLineLikeAb`: **failed before the fix** (assert
  `countLinesStartingWith(lines, "MESSAGE Speed ") == 1`, abort in `rapfi_tests` with asserts live,
  run saved in `build/search14/failfirst.txt`); passes after.
- `mctsSearch_normalMode_outputUnchanged`, `abSearch_briefMode_outputUnchanged`: pass (the latter
  uses a fixed-depth AB `YXVCN 3` query; it passed before and after).
- Full suite after the fix: `208/208 tests passed` (`build/search14/tests_after.txt`).

**Bench** (gcc, Release, built-in config): Nodes `17621937`, Hash `8c1f72e5`, unchanged.

**Masked output diffs** (`build/search14/cap.py`: 4-move Standard position, `MAX_NODE 20000`, 1
thread, one ordinary search then one `YXVCN 3` query in the same session; `message_mode` and
`default_searcher` set per config dir; `Speed`, `Time`, `tm`, `n/ms`, weight load ms masked; the
baseline was captured twice and was identical). Baseline = the tree with SEARCH-13 uncommitted.

| case | lines before | lines after | diff |
|---|---|---|---|
| AB brief | 13 | 13 | none |
| AB normal | 50 | 50 | none |
| AB ucilike | 45 | 45 | none |
| MCTS brief | 11 | 15 | +4 (two per search) |
| MCTS normal | 35 | 35 | none |
| MCTS ucilike | 31 | 31 | none |

MCTS `BRIEF` diff (both searches, ordinary and `YXVCN 3`):

```
7a8,9
> MESSAGE Speed X | Eval 387 | Node 20K | Time X
> MESSAGE Bestline G9 I10 G10 G11 J8 K8 H11 F9 I7 J6 G7 G6 H7 M6 L7 F7
10a13,14
> MESSAGE Speed X | Eval -M4 | Node 20K | Time X
> MESSAGE Bestline I10 Pass
```

The SEARCH-13 change only affects VCN singular roots; these positions have no forced root, so it
plays no part in the diffs.

**Intended output change with VCN off:** an MCTS search in `BRIEF` now prints these two lines at the
end of every search, ordinary and VCN. Search behaviour is unchanged.

**Consumers of the new lines.** Selfplay forces `BRIEF` (`command/selfplay.cpp:971`) and may run
MCTS, so its logs gain the two lines per search. `scripts/` only holds `pgo_build.sh`; nothing there
parses logs. No `tests/*.sh` parses selfplay logs. `tests/vcn_measure.sh` and `tests/vcn_sweep.sh`
read the last `MESSAGE Speed` line (`Eval`, `Node`, `Time`; `vcn_measure.sh` also `Depth`), which
under MCTS+BRIEF used to be absent and is now found; `Depth` comes out empty for MCTS (no field),
which `vcn_measure.sh` prints as an empty column. No special-casing was added.
