# SEARCH-14 — MCTS prints no result line in BRIEF message mode

**Status:** open (recorded 2026-11-09, user). Found in VCN-03 slice 5 (§9 "Slice 5", finding). Not
VCN-specific.

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
