# YXANALZ — user-supplied root candidates

**Status:** done (2026-09-09). All four verification tiers passed — see §Verification.

**Class:** C (protocol surface + plumbing). Adds a new branch; the existing root-move
generation path must stay bit-identical. Decision note below stands in for a design doc.

## Goal

New Gomocup/Yixin extension command:

```
YXANALZ x,y x,y x,y ... DONE
```

Analyze exactly the listed moves as the depth-0 root candidates of the side to move, on the
**current board state** (already set by a prior `BOARD` / `YXBOARD`). Unlike `YXNBEST n`
(engine generates its own root list, then reports the top `n`), `YXANALZ` *replaces* the root
list with the user's allow-list. The `x,y` entries are next-move candidates for one side, not a
PV and not alternating-side moves.

Semantically the inverse of `options.blockMoves` (deny-list): `YXANALZ` is an allow-list.

## Decision note (what / expected / how measured)

- **What:** add `SearchOptions::rootMovesWhitelist`; when non-empty, `generateRootMoves()` uses
  exactly that list (no `MovePicker`, no symmetry filter, no block filter). New protocol handler
  `analyzeMoves()` parses the list, validates legality, sets `multiPV`, calls `think()`.
- **Expected:** with the whitelist empty (every existing command), root generation and search are
  unchanged. With `YXANALZ`, each listed legal move is searched as its own PV line and reported
  via the existing `printPvCompletes` path.
- **Measured:** `rapfi bench` node count + hash bit-identical to pre-change baseline (record the
  baseline here on first measurement). Manual: `YXANALZ` on a known position returns per-move
  values matching a `YXNBEST`-with-large-n run restricted to the same moves.
- **Baseline (2026-09-09, gcc, Release, AVX512, `BENCH` command):** Search Bench Nodes
  `16733809`, Hash `4621a98a`. Verified identical with the feature code reverted and after
  re-applying it (feature-inactive path unchanged).

## Settled design points

| Point | Decision |
|---|---|
| Symmetry | **Allowed** — whitelist branch skips `Opening::filterSymmetryMoves`; symmetric entries all kept. |
| Block filter | Skipped in the whitelist branch (user is explicit). |
| Forbidden point (Renju black) | Skip entry + `ERRORL`, not added. **Impl note:** `parseLegalCoord` only checks in-board + emptiness (not forbidden), so `analyzeMoves()` adds an explicit `board->checkForbiddenPoint()` guard for Renju black. |
| `balanceMode` / `swapable` | `analyzeMoves()` forces `balanceMode = BALANCE_NONE`; leaves `swapable` untouched. |
| Output | Reuse `printPvCompletes` (as `YXNBEST`); no new printer in this slice. |
| Early-exit (mate / singular root) | Unchanged; relies on `isAnalysisMode()` (no time limit ⇒ no early stop). |
| Empty whitelist after validation | `ERRORL("No valid analyze move")`, do **not** search, return to protocol loop. |
| MultiPV | `options.multiPV = max<uint16_t>(1, whitelist.size())`. |
| Move not in `candidatesBB` (far from stones) | Accepted; `board->move()` refreshes candidates around it. Eval may be less reliable — acceptable, not guarded. |

## Scope steps

1. `search/searchcommon.h` — add `std::vector<Pos> rootMovesWhitelist;` to `SearchOptions`
   (next to `blockMoves`), with a comment.
2. `search/searchengine.cpp` `generateRootMoves()` — early branch: if
   `th.options().rootMovesWhitelist` is non-empty, `addMoveToRootMoves(m)` for each entry and
   `return` before `MovePicker`, block filtering, and symmetry filtering. (Balance2 expansion in
   `addMoveToRootMoves` is unreachable here because `analyzeMoves()` forces `BALANCE_NONE`.)
3. `command/gomocup.cpp`:
   - `analyzeMoves()`: clear `options.rootMovesWhitelist`; loop reading tokens until `DONE` /
     EOF; `parseLegalCoord` each; reject `Pos::PASS`, duplicates; push valid ones. If none
     valid → `ERRORL` and return. Force `options.balanceMode = BALANCE_NONE`,
     `options.multiPV = max<uint16_t>(1, whitelist.size())`. Call `think(*board, options.multiPV)`.
   - Dispatch: `else if (cmd == "YXANALZ") CheckBoardOK(analyzeMoves);` — in the board-modifying
     group (pondering stopped first).
   - **Whitelist is one-shot per `YXANALZ` turn.** Clear `options.rootMovesWhitelist` at the
     start of every other think entry point: `turn()`, `begin()`, `getPosition()`, `nbest()`,
     `balance()`, `searchDefend()`, `swap2board()`. Do **not** clear it inside `think()` (that
     would erase the list `analyzeMoves()` just set). A stale whitelist leaking into the next
     `TURN` is the main hazard.

## Function → test-case list (write before code)

`generateRootMoves` (behaviour via a small harness or `SearchThread` fixture):

- `generateRootMoves_whitelistEmpty_fallsBackToMovePicker` — regression: identical root list to
  current behaviour on a fixed position.
- `generateRootMoves_whitelistNonEmpty_rootMovesEqualWhitelistExactly`
- `generateRootMoves_whitelistWithSymmetricEntries_keepsAll` — two mirror-image moves both
  survive (would be collapsed by the normal path).
- `generateRootMoves_whitelistContainsBlockedMove_stillIncludesIt` — entry also in
  `blockMoves` is kept.
- `generateRootMoves_whitelistOrderPreserved_matchesInputOrder` (before value sort).

`analyzeMoves` parsing (protocol-level, stream fixture):

- `analyzeMoves_validCoords_populatesWhitelistAndSetsMultiPv`
- `analyzeMoves_illegalCoord_skipsEntryAndWarns` (occupied cell)
- `analyzeMoves_forbiddenPointRenjuBlack_skipsEntry`
- `analyzeMoves_passToken_rejected`
- `analyzeMoves_duplicateCoord_deduplicates`
- `analyzeMoves_allEntriesInvalid_errorsAndDoesNotSearch`
- `analyzeMoves_forcesBalanceModeNone`
- `analyzeMoves_leavesNoStaleWhitelistForNextTurn` (whichever clearing strategy is chosen)

## Verification

- Tests built with asserts live (`-UNDEBUG` on the test target).
- `rapfi bench` bit-identical vs baseline (feature inactive path). Baseline: Search Bench Nodes
  `16733809`, Hash `4621a98a` (2026-09-09, gcc Release AVX512).
- Targeted position: hand-built board, `YXANALZ` over 3 moves, assert reported values/PVs.

### Verification results (2026-09-09, gcc 14 Release AVX512, NO_COMMAND_MODULES)

1. **Asserts live.** New CMake target `rapfi_tests` (`-DBUILD_TESTS=ON`) compiles the engine core
   with a trailing `-UNDEBUG` that overrides Release's `-DNDEBUG`. Confirmed by
   `./build/rapfi_tests --check-asserts-live`: a deliberate `assert(false)` aborts with SIGABRT
   (exit 134) and the "NDEBUG leaked in" fall-through line never prints. Real fixture asserts also
   fired and were fixed during development.
2. **bench bit-identical.** Baseline captured with the feature code reverted: Nodes `16733809`,
   Hash `4621a98a`. After re-applying all changes and rebuilding `rapfi`: Nodes `16733809`, Hash
   `4621a98a` — identical.
3. **Unit tests.** `./build/rapfi_tests` → `14/14 tests passed` (5 `generateRootMoves_*`, 9
   `analyzeMoves_*` including the targeted 3-move position case).
4. **Targeted position (manual).** `YXBOARD 7,7,1 8,8,2 7,8,1 7,9,2 6,8,1` then
   `YXANALZ 8,7 9,8 6,9`: three PV lines, one per whitelisted move —
   `(1) G10 (=6,9) -169`, `(2) I8 (=8,7) -198`, `(3) J9 (=9,8) -M48`; bestmove `6,9`.
   Sane: the two moves near the fight are playable, `9,8` walks into a mate. A `YXNBEST 60` run
   on the same board shows the same overall character (black ~-1.7).
