# VCN — execution guidance

Task: [`../todo/VCN-search-mode.md`](../todo/VCN-search-mode.md).

## Approach

1. Record the bench baseline first (`echo BENCH | ./pbrain-rapfi`); it must be bit-identical at
   the end. Re-run it after every step that touches `search.cpp` or `movepick.cpp`.
2. Add the data (`searchcommon.h`, `searchstack.h`) with no behaviour change; build; bench.
3. `PASS`-safety guards next (history, movepick `ttmValid`, `classifyMove`, `statScore`) — these are
   behaviour-neutral while no pass can reach them; bench again.
4. Then the actual VCN logic, then the `YXVCN` command, then tests.

## Pitfalls

- **The reference commit does not compile as published.** `vcnLevel` (in `search()`),
  `vcnIsAttacker` (in `vcfdefend`) and `vcnAllowB4` (in `vcfsearch`) are undefined there. Do not
  copy it verbatim; derive `vcnLevel` from `ss->vcnLevel` after it is set for the node.
- **Derive the level before anything uses it** — it is needed for the TT key (Step 4), for the
  early `depth <= 0` dispatch and for the VC5 check, all of which sit above the point where the
  reference computes it.
- **`Pos::PASS` is `-1`.** `board.pattern4(Pos::PASS, …)`, `mainHistory[..][PASS]`,
  `counterMoveHistory[..][PASS.moveIndex()]` and `statScore(.., PASS)` read out of bounds. Every
  site that handles a move taken from `MovePicker` in the main loop must tolerate `PASS`. `Pos`
  converts to `int`, so `while (Pos move = mp())` keeps going on `PASS` (only `Pos::NONE == 0`
  ends the loop).
- **Name clash:** `GenType::VC2` (global, movegen) is unrelated to the VCN levels; `VCNLevel` is a scoped enum so there is no collision. Always spell levels `VCNLevel::VC2` etc.
- **Database:** disable both probe and write while VCN is on; a VCN-keyed node must never read or
  write a record that stands for the ordinary game.
- **Do not make `SearchStack::vcnLevel` non-zero by default.** `VCNLevel::None == 0` keeps the stack
  zero-initialised and the default path identical.
- **Don't touch:** `game/pattern.*`, bitkeys, zobrist, `movegen.cpp`'s `GenType::VC2` filter
  (separate follow-up, see the VC2 review notes).
- One-shot like `YXANALZ`: `options.vcnMode` is a file-static in `gomocup.cpp`; `YXVCN` sets it and
  always clears it after `think()` returns, including on early-exit paths.
