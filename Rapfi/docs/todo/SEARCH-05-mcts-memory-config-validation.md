# SEARCH-05 — Validate SEARCH-02 settings beyond freestyle / 2 s / 256 MB

**Status:** open (recorded 2026-11-07, not started).
**Class:** B (measurement; may change defaults).

## Why

SEARCH-02 was measured on one rule (freestyle), one time control (`tc=0/2`), one memory size
(256 MB), 1 thread, 230 games. Equal strength was seen at about 1/4.6 of the memory. Defaults were
left unchanged because of the narrow coverage.

## Scope

- Standard and Renju, longer time controls, more threads, larger memory (unbounded).
- Sweep `mcts_edge_policy_mass` (0.95 / 0.97 / 0.99) and `mcts_expand_min_visits` (2 / 3 / 4)
  separately; the first run only tested the pair `0.97` + `3`.
- Each setting against the defaults, and the best against alpha-beta (the 100-game result was
  43 - 50 - 7 for the pair above).
- If a setting wins or ties everywhere, make it the default and update the bench note.

## Measure

`build/oppo4-ab/run.sh <cand> <base> <rule> <games> tc=...` with SPRT; record peak RSS per setting.
