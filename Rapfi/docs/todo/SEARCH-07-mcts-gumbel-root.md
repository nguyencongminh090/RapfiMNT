# SEARCH-07 — MCTS root: Gumbel selection and Sequential Halving

**Status:** closed 2026-11-08, not pursued (user decision after Step 0; see the end of this file).
**Class:** B.

## Source

Danihelka, Guez, Schrittwieser, Silver, *Policy improvement by planning with Gumbel*, ICLR 2022.
Read the main text from a text conversion; appendices not read.

- Root action: Gumbel-Top-k sample of `m` moves, then Sequential Halving; compare
  `g(a) + logits(a) + sigma(q(a))`, `sigma(q) = (c_visit + max_b N(b)) * c_scale * q`
  (paper: `c_visit = 50`, `c_scale = 1.0`).
- The root only needs the final move, so the paper argues for simple regret at the root, not the
  cumulative regret PUCT optimises.
- Paper's own limit: the gain is largest when simulations are few relative to the number of
  actions; with many simulations AlphaZero already works well. Trained and tested on Go, chess, Atari.

## Problem

A best move with low policy can be missing from the tree (`mcts_edge_policy_mass`, SEARCH-02/05) or
get too few visits at the root. SEARCH-04 already records that a cut edge cannot come back.

## Step 0 — diagnostic (no search change)

Using positions where the final best move had low prior, count how often that move is (a) cut by
`mcts_edge_policy_mass = 0.99`, (b) present but under-visited at the end. If (a) and (b) are rare,
close this item. Root and non-root cuts differ: check whether the root is subject to the cut.

## Scope (only if Step 0 shows a real loss)

- Root-only opt-in selector (Gumbel-Top-k + Sequential Halving). Non-root keeps PUCT.
- Interaction with `rootMoves`, multithreaded virtual visits, time management
  (`checkTimeupCondition`) and the pondering/`YXANALZ` allow-list. Sequential Halving fixes the
  budget in advance, which conflicts with time-based stopping; decide how.
- Randomness: Gumbel noise makes root play non-deterministic; for match play use a fixed seed or no
  noise (`g = 0`) and keep only Sequential Halving.

## Test list

To be written before code:

- `gumbelTopK_fixedSeed_returnsMDistinctMovesInPriorBiasedOrder`
- `sequentialHalving_budgetN_visitsSumToBudgetAndHalvesEachPhase`
- `rootSelect_lowPriorWinningMove_isNotDroppedWhenMAtLeastItsRank`
- `rootSelect_optionOff_matchesCurrentSelectChildRoot`
- `rootSelect_moveOutsideRootMoves_neverSelected`
- `rootSelect_singleLegalMove_returnsItWithoutSearchBudgetLoss`

## Measure

Decision note first, bench unchanged (built-in config is alpha-beta). Self-play versus SEARCH-05
default at equal time and at small node budgets (the regime the paper claims), 1 and 4 threads, plus
a set of tactical positions whose best move has low prior.

## Step 0 result — 2026-11-08 (one position only: `h8 j9 f6 i10`, Standard)

Setup: binary HEAD 047692a with `mcts_edge_policy_mass = 0.99` (`build/oppo4-ab/s5_pm99`), NNUE
`mix9svq`, 10 s, `YXBOARD` then `YXNBEST 221`, `message_mode = ucilike` (prior is printed to 2
decimals). Raw output was kept in the session scratchpad, not in the repo. The thread count was set
with `INFO THREAD_NUM 1` but not verified from the output; the reported speed (about 2.7M nodes/s)
suggests it may not have been 1.

- **Root is never cut.** `expandNode<true>` (`search/mcts/search.cpp:417-423`) calls `createEdges`
  without `EdgeLimits`, so `mcts_edge_policy_mass` only affects non-root nodes. Case (a) of Step 0
  cannot happen at the root.
- **Root list is symmetry-reduced.** 117 root moves are listed, all on one side of the main
  diagonal; `E7` has no entry and stands for `G5` (checked only from the listed set, the symmetry
  code was not read).
- **`G5`:** prior 0.01 (between 0.005 and 0.015), ninth by visits: about 949 of 27M (0.0035%) at
  10 s, ev -177, seldepth 15. The chosen move was `I7` (prior 0.17, ev -35). Only 10 root moves have
  prior >= 0.005.
- **Value, not only visits.** `G5`'s ev stayed at -178 / -177 while its visits went 649 -> 949
  (6 s -> 9 s). This does not separate the two causes, because 949 visits is shallow.
- **`YXANALZ` is not usable for this.** A whitelist search stops after about 100 nodes per run
  (`G5`: ev -52, `I7`: ev -36), so it cannot show what a long search of `G5` alone gives.

Reading: for this position the best move is present at the root and is not hidden by the edge cut;
it is under-visited because the network scores it far below `I7`. A Gumbel / Sequential Halving root
would keep `G5` only if it were among the `m` sampled moves, and then compare `q` (-177 vs -35), so
it would likely be halved away in the first phase. This matches the open VCN-02 item 5 (attack value
at the horizon). One position is not a sample: the counting over many positions in Step 0 is not
done. Decision on whether to continue SEARCH-07: open, user.

## Closure — 2026-11-08 (user)

Not pursued. Low-prior best moves like `G5` here come from the NNUE training and the knowledge
distilled from the teacher (KataGomo), so they are rare, and finding more of them is slow with no
independent ground truth (only proven forced wins would give one). A root change cannot fix a
value that is wrong. Reopen only if a source of labelled positions appears (for example forced wins
proven by `YXVCN` showing a low-prior winning move at the root).
