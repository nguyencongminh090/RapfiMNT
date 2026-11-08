# Notes: using the Rapfi NNUE for play-style biometrics and anti-cheat

Status: brainstorm notes (2026-11-07). Nothing here is implemented or measured.
Statements marked **(unverified)** were not checked against code or data.

## 1. What the NNUE is (as used here)

- `mix9svq` = Rapfi's "mix" family NNUE (`Rapfi/eval/mix9svqnnue.cpp`). Weights are the
  `mix9svq*.bin.lz4` files in `Rapfi/build/yixin-net/`: freestyle (`bsmix`, several
  board sizes), standard 15x15, renju 15x15 (black and white nets).
- It is a real neural network, "NNUE" = *efficiently updatable*: sparse input, a large
  first layer updated incrementally per move (accumulator), small quantised int8/int16
  layers, SIMD on CPU.
- **Input features** are *line shapes*, not the classical Pattern2/Pattern4 codes
  (`mixcommon.h`): for every cell, 4 directions, an 11-cell line, base-3 code
  (empty / self / opponent), plus border-distance codes. `ShapeNum = 442503`.
  A codebook (`codebook[2][65536][64]` + `mapping_index`) maps each shape to a vector.
- **Outputs**: `evaluateValue` -> win/loss/draw (softmax, side to move);
  `evaluatePolicy` -> a score per empty cell (`PolicyBuffer`, optional `applySoftmax`).
- Board sizes: square only, code asserts `boardSize <= 22`; a weight file lists its
  supported sizes in a header mask. Standard/renju nets are 15x15 only.
- Evaluator state is incremental: use `syncWithBoard(board)` for arbitrary positions.
  `computeEvaluatorValue` applies the config draw-ratio adjustment; calling the
  evaluator directly gives the raw network output.

## 2. Intuition (NNUE) vs search

- NNUE = static, one forward pass: good for shape/positional sense, weak on long forcing
  lines (VCF/VCT).
- In `search()` the engine itself falls back to `vcfsearch`/`vcfdefend` at depth 0 and
  uses `quickWinCheck`; `evaluate()` only calls the NNUE inside the alpha-beta margin.
- Consequence: NNUE alone is fine for **screening and style features**; **cheating
  verdicts** should be checked against shallow search output.

## 3. Idea A: style biometrics with few-shot learning

Pipeline: `PGN -> per-move NNUE features (cached) -> game encoder -> metric learning`.

- NNUE is a **frozen feature extractor**, not fine-tuned (quantised, not differentiable
  in our pipeline). It was trained for engine value/policy, not for style, so style
  appears only as deviation from the engine.
- Per-move features: rank and probability of the played move in policy, policy entropy,
  value loss (win-rate before/after), pattern type created (attack/defence/build),
  game phase (stone count), position on board. Optional: the 64-d hidden vector
  (needs code changes to export; **unverified** effort).
- Model: prototypical network or triplet loss so new players can be enrolled from a few
  games (1/3/5-shot). A fixed softmax only works for a fixed player set.
- Evaluation: split train/val/test **by player**; report identification accuracy and
  verification EER on unseen players.
- Risks: style vs skill confusion; style drift over time; position-context noise
  (feed difficulty/phase as context); biometric privacy and consent.
- Without player labels: self-supervised (segments of the same game/session), but a
  labelled subset is still needed to validate.

## 4. Idea B: move difficulty for the statistical anti-cheat

Goal: matching an *easy* move is not evidence. Weight evidence by difficulty.

Difficulty proxies from the NNUE, per position before the move:

- top-1 policy probability;
- top-1 minus top-2 gap (probability or post-move win rate);
- policy entropy;
- number of plausible moves (cells covering ~90% of policy mass);
- value spread between best and other candidate moves (needs playing each candidate and
  evaluating: tens of evaluations, still cheap).

Also use board threat counts (`p4Count`, e.g. opponent five / flex four, as in
`search.cpp`) to flag **forced moves** and exclude them (weight 0).

Statistical use:

1. Bin positions by difficulty (and by game phase, and by player rating band).
2. From known-clean games estimate the human match probability `p_bin`.
3. For each move, evidence = how surprising the match is given `p_bin` (log-likelihood
   ratio or z-score); sum over the game.
4. Output a signal to combine with other signals (move time, session behaviour).

Cautions:

- "Easy for the engine" is not "easy for a human": tactical positions can look easy to
  the static NNUE and be hard for people; calibrate on human data and add shallow search.
- Opening book moves match a lot without being suspicious; separate by phase.
- Strong honest players have high match rates in hard positions; measure false-positive
  rate on trusted strong players before choosing thresholds.
- This is a signal for human review, not a standalone verdict.

## 5. Data and pipeline plan

Data: raw Gomoku PGN, headers contain player names (usable as identity labels).
Rapfi has **no PGN reader** (checked `Rapfi/` for "pgn": no hits), so a parser is needed.

1. Parse PGN (agree on coordinate convention and axis flip; note `coord_conversion_mode`
   in `config.toml`), clean bad games, normalise player aliases, split by player.
2. Extract per-move features with the engine and **cache** them (Parquet/CSV). Pin engine
   version, weights and config for reproducibility.
   - Route A: drive Rapfi over the protocol from Python (no engine changes, slower;
     whether the protocol exposes raw policy per cell is **unverified**).
   - Route B: a C++ command inside Rapfi calling `evaluateValue`/`evaluatePolicy`
     (fastest, needed for large data).
3. Baseline: statistical summaries + kNN/SVM to check there is signal.
4. Then prototypical/triplet model; report 1/3/5-shot on unseen players.
5. For anti-cheat: difficulty-binned match statistics vs clean-player baselines.

## 6. Open items

- Obtain a sample of a few PGN games (coordinate convention, rule, board size).
- Number of players, games per player, total positions (decides route A vs B).
- Which rule and weight file (freestyle/standard/renju), board size 15 or other.
- Decide which search depth/time to use as the "reference move" for anti-cheat.
- Privacy/consent for storing style embeddings (legal requirements **unverified**).
