# oppo4 eval: theory and instrumented measurements (2026-11-06)

Research behind [`../todo/SEARCH-01-oppo4-explicit-threat-node.md`](../todo/SEARCH-01-oppo4-explicit-threat-node.md).

Scope: Standard only, 1 thread, 114k nodes per move, self-play of base with TT kept across a game,
60 games per run. Passive counters left the bench at `4587838` / `4447fd06`; the probe modes do
perturb the search. The instrumentation lived in a gitignored scratch directory and is not kept in
the repo. No knowledge note matches alpha-beta pruning in SKILLS_TREE (one query).

## Claim checked

An issue report said the `oppo4` branch of `search()` never assigns `eval`, and that
`eval = ss->staticEval` fixes large strength losses. The missing assignment is real. The report's
large-scale claims were never reproduced.

## Earlier A/B results against base

Standard, thread 1, 331 openings, forfeit pairs excluded.

| Variant | Condition | Games | W-L-D | Elo [95%] |
|---|---|---|---|---|
| B = `eval = ss->staticEval;` | tc=600/1 | 293 | 78-105-110 | -32 [-55,-8] |
| B | 114k nodes | 386 | 103-127-156 | -19 [-40,+2] |
| C = B + `!oppo4` on futility | tc=600/1 | 400 | 113-141-146 | -24 [-47,-2] |
| D = base + `!oppo4` on razoring | tc=600/1, stopped | 257 | 71-85-101 | -19 [-45,+7] |
| E = `goto moves_loop` for all oppo4 | tc=600/1, stopped | 476 | 139-161-176 | -14 [-34,+6] |

No variant was stronger than base. Only B (time) and C are significantly below it, without
correcting for multiple comparison.

## What base does at non-PV `oppo4 && !oppo5` nodes (n = 60.2M, 14% of non-PV)

| Path | Count | Note |
|---|---|---|
| d < 2.6 drop to vcfsearch | 29.74M (49.4%) | fresh eval plus own VCF, about 2.4 nodes each |
| d < 2.6 moves loop | 0.65M (1.1%) | Standard margin is 0, so only the mate guard blocks the drop |
| d >= 2.6 moves loop | 29.75M (49.4%) | no futility, no likelyFailHigh, no null move |
| d >= 2.6 drop (`alpha > 2765`) | 0.07M (0.12%) | alpha < 6000: 0 cases; alpha < 29500: 44 of 70538 |

The 2765 threshold never fires on a real eval. In effect the sentinel implements: "below razor depth
treat as a VCF leaf, otherwise search all defences without static pruning". Upstream has never
assigned `eval` here since its first public commit (d17d398, 2022-09-30), so all tuning happened on
this behaviour.

## Measurements

Parent-derived eval minus fresh NN eval: mean +16..+19, median 0, sd 138-146. It is noise, not bias.

Verdict "value > alpha" at dropped nodes against a deeper search of the same node:

| Predictor | vs +6 ply (n=74466) | vs +10 ply (n=18699) | Mean nodes |
|---|---|---|---|
| parent eval | 66.9% | 64.3% | 0 |
| fresh eval | 82.3% | 79.6% | 1 eval |
| vcfsearch (base) | 84.7% | 82.4% | 2.4 |
| moves loop | 87.4% | 85.2% | 5.7 |

The loop runs just before the reference and shares TT, so its lead is an upper bound.

Futility false-cut rate (would fire, but the same-depth search fails low), in %:

| Depth bucket | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9+ |
|---|---|---|---|---|---|---|---|---|
| quiet node, real rule (sampled) | 2.5 | 1.4 | 0.7 | 0.8 | 0.4 | 0.4 | 0.1 | 0.4 |
| oppo4, fresh eval | 9.6 | 8.9 | 12.9 | 17.2 | 18.7 | 32.1 | 53.7 | 65.2 |
| oppo4, parent eval | 18.3 | 18.8 | 26.4 | 35.2 | 43.3 | 63.4 | 81.4 | 89.4 |

Overall: fresh 12.1%, parent 25.9%, quiet about 1%.

Wider futility scale with fresh eval (Standard rule scale is 43):

| Scale | 43 | 60 | 80 | 100 | 130 | 160 | 200 | 260 | 340 |
|---|---|---|---|---|---|---|---|---|---|
| False cut % | 11.4 | 9.8 | 8.5 | 7.5 | 6.5 | 5.9 | 5.1 | 4.1 | 3.6 |
| Direct node saving % | 2.65 | 1.61 | 0.95 | 0.57 | 0.30 | 0.16 | 0.07 | 0.02 | 0.01 |

No margin reaches the quiet error rate. Direct saving excludes cascades.

Stale eval side effects in base:

- TT: `search()` stores the parent-derived eval and `vcfsearch` reads it as stand-pat. 961k reads of
  99.4M vcf nodes (0.97%); stand-pat or delta verdict changes in 0.24% of vcf nodes.
- `improvement`: 17.1M of 368.6M quiet nodes (4.6%) compare against a stale ss-2; the sign flips at
  3.02M (0.82%). Flips are symmetric (1.46M towards improving, 1.56M away).

## Conclusions

1. The missing assignment is real, but the resulting behaviour is sound and worth keeping.
2. This explains the earlier A/B results. It is consistent with them, but each CI is wide.
   - B: parent eval futility has 25.9% false cuts against about 1% on quiet nodes.
   - C: D has less pruning yet 4.36M bench nodes, C has 91.8M, so the blow-up comes from the
     likelyFailHigh extension.
   - D, E: pay 17-48% bench time to depth for about 2.8 points of verdict accuracy.
3. The report's false-mate evidence does not hold. By code a dropped node never returns a mated
   score unless proven. Re-run (Freestyle, 1 thread): base and B both report +M13 on the report's
   position p1 at depth 16 and 24, and +M15 on p9 at depth 16. The report's +M25 did not reproduce on
   this build and net.

## Candidates

| Id | Change | Bench (yixin-net) | Verdict |
|---|---|---|---|
| R0 | explicit rule, futility and likelyFailHigh guarded by `!oppo4` | identical: 4587838 / 4447fd06, built-in 17621937 / 8c1f72e5 | chosen, no A/B needed |
| H | R0 + own static eval on searched threat nodes, no parent eval in TT | 4131367 nodes, about 13.1 s (base 12.9-14.5 s) | no predicted Elo sign, does not qualify for an A/B |
| J | H + futility on | 2578015 nodes, 8.8 s | rejected: 12% false cuts, no safe margin |

A/B resolution from earlier data (386 games: +-21 Elo at 95%): +-10 needs about 1700 games (about
8.5 h), +-5 about 6800 (about 34 h).

## Limits

Standard only. The reference is the same engine searched deeper, not ground truth. The quiet baseline
runs only at sampled firing nodes and uses its real margin, including `improving`; the oppo4 shadow
uses improving=false, the conservative side. Bench time is from 1-2 runs. Equivalence for Freestyle
and Renju rests on the algebra in the todo file plus the bench hash.
