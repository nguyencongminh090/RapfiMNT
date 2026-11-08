# SEARCH-06 — MCTS selection rule PUCT-V (per-child variance)

**Status:** closed 2026-11-08, not pursued (user decision; see the end of this file).
**Class:** B.

## Source

Weichart, *Variance-Aware Prior-Based Tree Policies for Monte Carlo Tree Search*, arXiv 2512.21648
(v3, 2026-04-26), eq. 17:

```
PUCT-V(a) = Q(a) + c1 * pi(a) * sigma_a * sqrt(N) / (1 + n_a) + c2 * pi(a) * ln(N) / (1 + n_a)
```

`sigma_a` is the empirical standard deviation of the child's returns (Welford update, O(1) memory).
Read pages 1-12 of 14; appendices D-F not read. The paper evaluates bandits, synthetic trees, MinAtar
(64 simulations, policy head only) and Animal Shogi. No Go/Gomoku, no Elo against a strong engine.

## Problem

`puctSelectionValue` (`search/mcts/search.cpp`) uses `cpuct * P / (1 + n)` with one parent-level
variance scale (`CpuctUtilityStdevScale`). Exploration does not depend on how uncertain each child is.

## Scope

- Opt-in selector behind a config option (default keeps current PUCT).
- Use `childNode->getQVar(...)` (already in `Node`) for `sigma_a`; no new node field expected. Check
  that `getQVar` measures the child's own return variance and not a different quantity.
- Q range: Rapfi utility is in [-1, 1]; the paper assumes rewards in [0, 1]. Decide the scale of
  `sigma_a` and of `c1`, `c2` explicitly.
- Unexplored edge (FPU path in `selectChild`) has no `sigma_a`; define its value.

## Known limit

`pi` multiplies both exploration terms, so a low-prior best move is not helped directly. This item
tunes exploration by uncertainty; it does not by itself fix a best move with low policy.

## Test list

To be written before code (`tests/`, new file added to `TEST_DRIVER_SOURCES`):

- `puctVSelectionValue_zeroVariance_reducesToBiasTermOnly`
- `puctVSelectionValue_uniformPriorAndNoVariance_matchesUctFreeForm`
- `puctVSelectionValue_higherVarianceChild_getsLargerExplorationBonus`
- `puctVSelectionValue_virtualLoss_appliedLikePuct`
- `selectChild_optionOff_returnsSameEdgeAsCurrentPuct` (guards the default path)
- `selectChild_unexploredEdge_usesDefinedFallbackSigma`

## Measure

Decision note first. Bench must stay Nodes `17621937`, Hash `8c1f72e5` (built-in config is
alpha-beta, so bench cannot show an MCTS effect). Then self-play, same harness as SEARCH-05,
equal time, 1 and 4 threads, Standard and Renju, sweep `c1` / `c2`.

## Closure — 2026-11-08 (user)

Not pursued. Reasons from the analysis of 2026-11-08:

- Expected gain is low. The paper itself reports PUCT-V ahead of PUCT only on the stochastic MinAtar
  games and "comparable" on deterministic ones; Gomoku/Renju is deterministic, and the paper has no
  Go/Gomoku result and no Elo against a strong engine.
- Rapfi already uses variance (`CpuctUtilityStdevScale = 0.05`, small; `LCBStdevs = 6.28` for the final
  move); whether the 0.05 was tuned is not known.
- Scale mismatch (rewards [0,1] in the paper, utility [-1,1] here; observed child stdev 0.11-0.28)
  forces `c1`, `c2` to be re-derived, and a low-visit child can get a near-zero `sigma` and be starved.
- The rule does not help a low-prior best move (`pi` multiplies both terms); Step 0 of SEARCH-07
  showed that case is a value problem anyway.
- Validation cost is high: parameters tuned together with PUCT would need re-tuning, SPSA cannot run
  on the user's machine, and 120 games give about +-24 Elo.

Reopen only if MCTS becomes the main searcher and a tuning budget exists. If so: `c2 = 0`, sweep `c1`
around the scale compensation (x3..x9), two self-play rounds, stop if the result is inside the error.

## Also considered and closed: ancestor-based alpha-beta bounds (Pepels & Winands, CoG 2024)

No separate TODO item was made. Reasons:

- In the paper's Gomoku results (Table II), alpha-beta bounds alone are inside the error margin
  (50.88 / 49.69 / 51.16 %, +-2.75); only with implicit minimax backups does it pass (52.69 / 54.91 /
  53.57 %, significant at the last two budgets). The gain comes from implicit minimax, not from the bounds.
- It is a UCT rule (no prior, `ln N`, rewards in [0,1], playout values); there is no obvious place for
  `delta` in PUCT, and the formula (3) leaves `ln(delta * N)` undefined for `delta < 0`.
- Rapfi's backup is a visit-weighted mean (`Node::updateStats`); implicit minimax would change the backup,
  a separate and riskier item. `ValueBound` already carries proven values and `SimpleVCF` covers forced fours at leaves.
- Implementation would thread four path variables through `searchNode` / `selectChild`.
- No convergence guarantee (the paper leaves it to future work).

If anything is revisited from this paper, it is implicit minimax backup as its own item, low expectation.
