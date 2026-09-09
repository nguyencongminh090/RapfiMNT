# YXANALZ — execution guidance

Task: [`../todo/YXANALZ-user-root-candidates.md`](../todo/YXANALZ-user-root-candidates.md).

## Approach

1. Add the `SearchOptions` field first, rebuild — no behaviour change, confirms the field is
   wired through the `ctx.options` copy into every thread.
2. Write the `generateRootMoves` test harness + failing tests, then add the whitelist branch.
3. Write the `analyzeMoves` parsing tests, then the handler + dispatch line.
4. Run `bench` before step 2 and after step 4 — must match.

## Pitfalls

- **Stale whitelist.** `options` is a file-static in `gomocup.cpp` reused across commands. The
  whitelist is one-shot: `analyzeMoves()` sets it, and every *other* think entry point
  (`turn`, `begin`, `getPosition`, `nbest`, `balance`, `searchDefend`, `swap2board`) clears it
  at the top of the function. Never clear it in `think()`. Cover with
  `analyzeMoves_leavesNoStaleWhitelistForNextTurn`.
- **Symmetry filter is intentionally skipped here** — do not "fix" it back in. The whole point of
  the command is that the user's exact list is honoured.
- **Worker copy.** Root moves are copied to workers in `startThinking`'s `runOnAllThreads` lambda
  *after* `generateRootMoves(*main())`. The whitelist branch only needs to touch the main
  thread's list; the existing copy handles the rest. Do not add a second copy path.
- **`pickBestThread` is skipped when `multiPV > 1`** (`searchMain` guard). A single-move
  `YXANALZ` sets `multiPV == 1` and *will* go through `pickBestThread` — that is fine (one root
  move, voting is trivial), but be aware the output path differs from the multi-move case.
- **`parseLegalCoord`** already handles in-board / empty-cell / forbidden checks and prints its
  own error. Do not duplicate those checks; just handle `Pos::PASS` and duplicates on top.
- Balance2 path in `addMoveToRootMoves` is dead code for this branch only because
  `analyzeMoves()` forces `BALANCE_NONE`. If a future caller sets the whitelist *with* a balance
  mode, that combination is undefined — assert or reject it rather than letting it silently
  build move pairs from a user list.

## Do not touch

- The `MovePicker` root path, `filterSymmetryMoves`, and `blockMoves` filtering for the
  **empty-whitelist** case — bit-identical `bench` depends on it.
- `pickBestThread`, aspiration window, iterative-deepening loop — unchanged.
- The `rapfi` engine target's `NDEBUG` (only strip it on test targets).
