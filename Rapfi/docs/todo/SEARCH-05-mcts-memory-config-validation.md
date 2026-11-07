# SEARCH-05 — Validate SEARCH-02 settings beyond freestyle / 2 s / 256 MB

**Status:** in progress (started 2026-11-07).
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

## Decision note (what / expected / how measured) — 2026-11-07

- **What:** variants are config-only copies of one binary (HEAD 047692a) in `build/oppo4-ab/s5/<name>/`:
  `d0` (defaults), `pm95` / `pm97` / `pm99` (`mcts_edge_policy_mass`, expand 1), `ex2` / `ex3` / `ex4`
  (`mcts_expand_min_visits`, mass 1.0), `pair` (0.97 + 3), `ab` (alpha-beta default).
- **Expected:** milder settings (`pm99`, `ex2`) tie the defaults; the aggressive ones (`pm95`, `ex4`)
  may lose a little; memory falls monotonically with the setting.
- **How measured:** (1) peak RSS, one fixed 2M-node search, Standard rule, per variant; (2) staged
  self-play with `run.sh`, SPRT [-10, 10] plus a game cap, `thread=1`, 256 MB, `tc=0/2`: stage 1
  Standard, single-parameter sweeps vs `d0`; stage 2 survivors on Renju, more threads / larger
  memory / longer time, and the best vs `ab`. A setting becomes the default only if it ties or wins
  everywhere it was tested; error bars are reported with every score.
- **Knowledge note:** none matches MCTS memory tuning (see SEARCH-02); design follows measurement.

## Results — 2026-11-07 / 08

Binary HEAD 047692a for every variant (config-only copies in `build/oppo4-ab/s5_<name>/`). Score is
for the candidate vs `d0`; Elo and ±1 SE are computed from W-L-D with the trinomial variance. No run
used an SPRT early stop in stage 1; 120 games gives about ±24 Elo (1 SE).

### Peak RSS (one fixed 2M-node search, Standard)

| Variant | d0 | pm95 | pm97 | pm99 | ex2 | ex3 | ex4 | pair |
|---|---|---|---|---|---|---|---|---|
| RSS (MB) | 431 | 147 | 164 | 195 | 287 | 221 | 170 | 102 |

### Stage 1 — Standard, 1 thread, 256 MB, tc=0/2, 120 games vs `d0`

| Variant | W-L-D | Score | Elo (±1 SE) | SPRT LLR |
|---|---|---|---|---|
| pm99 | 37-34-49 | 0.512 | +9 ± 24 | +0.292 |
| pm97 | 30-37-53 | 0.471 | -20 ± 24 | -0.726 |
| pm95 | 30-49-41 | 0.421 | -55 ± 26 | -1.727 |
| ex2 | 27-41-52 | 0.442 | -41 ± 24 | -1.457 |
| ex3 | 26-39-55 | 0.446 | -38 ± 23 | -1.412 |
| ex4 | 41-45-34 | 0.483 | -12 ± 27 | -0.322 |

Only `pm99` ties `d0`; stronger cuts lose monotonically. `mcts_expand_min_visits` lost for 2 and 3,
which contradicts the expectation that `ex2` ties. `pm99` alone went to stage 2 (user decision).

### Stage 2 — `pm99` vs `d0`

| Leg | Setup | Games | W-L-D | Score | Result |
|---|---|---|---|---|---|
| R | Renju, 1 thread, 256 MB, tc=0/2 | 120 | 57-55-8 | 0.508 | tie (LLR +0.123) |
| T | Standard, 4 threads, 256 MB, tc=0/2 | 47 | 36-7-4 | 0.809 | H1 accepted (LLR +3.124) |
| M | Standard, 1 thread, 1 GB, tc=0/2 | 80 | 23-23-34 | 0.500 | tie |
| L | Standard, 1 thread, 256 MB, tc=0/8 (longer time) | 20 | 19-1-0 | 0.950 | H1 accepted (LLR +5.452) |

A first M run was interrupted at 26 games (6-11-9) by a shutdown and discarded; the full rerun above
is the result (`logs/s5s2_M_partial1.*` kept for the record).

### `pm99` vs alpha-beta (`ab`), tc=0/2, 1 thread, 256 MB, 100 games each

| Rule | W-L-D | Score | Elo (±1 SE) |
|---|---|---|---|
| Standard | 25-35-40 | 0.450 | -35 ± 27 |
| Renju | 47-51-2 | 0.480 | -14 ± 34 |

Neither stopped by SPRT. Consistent with the SEARCH-02 result (43-50-7 for the pair, freestyle).

### Reading

- `pm99` ties `d0` where memory is not binding (1 thread at 2 s, Renju, 1 GB) and wins clearly where
  `d0` is likely to hit the 256 MB cap (4 threads, longer time). The memory-cap explanation is a
  hypothesis: RSS or node count versus time of `d0` was not measured directly.
- No setting tested lost to `d0` while `pm99` was in play, so `pm99` meets the "ties or wins
  everywhere tested" rule. Not covered: freestyle in this task, memory above 1 GB, long time with
  many threads, and tie legs R / M have about ±30 Elo error.
- MCTS with `pm99` stays slightly below alpha-beta within error.
- **Defaults are unchanged.** Whether to make `mcts_edge_policy_mass = 0.99` the default (needs a
  bench re-record and a bench note update) is a pending user decision.
