# SEARCH-09 — MCTS mate bound unsound under the policy-mass edge cut

**Status:** open (2026-11-08). Found while researching [VCN-03](VCN-03-mcts-vcn.md) §4.1; not yet
reproduced on a position. First step is a failing test.

**Class:** B (search behaviour). Decision note and numbers required before done (CLAUDE.md rule 9).

**Blocks:** VCN-03 option B (native MCTS-VCN): a VCN proof is exactly "every defender reply loses",
the claim this bug makes unsound.

## Problem

- `Node::createEdgesFrom` (`search/mcts/node.cpp:177`) keeps only the leading moves until their
  cumulative policy reaches `mcts_edge_policy_mass` (default `0.99` since SEARCH-05; never fewer
  than `mcts_min_edges = 8`, `search/searchconfig.h:57`; root is never cut).
- `Node::updateStats` (`node.cpp:236`) builds `ValueBound` only from the edges that exist. A cut
  move is invisible to it.
- So a node whose **kept** children are all proven wins for the opponent gets
  `bound.upper` = mated, even when a cut move saves it (false proven loss).
- One level up, that child's false proven loss becomes a false proven win of the parent
  (`maxBound.lower >= VALUE_MATE_IN_MAX_PLY`). `selectBestmoveOfChildNode` then calls
  `overrideBestByMateBounds` (`search/mcts/search.cpp:889`), which multiplies every non-proven
  child's selection value by `1e-9`. **The engine plays the falsely proven move** and prints `+M`.
- Only the proven-loss direction (`upper`) is affected. A single proven-winning child is a valid
  proof whether or not siblings were cut.
- Only nodes with more than 8 legal replies can be cut, so nodes facing a four (few replies) are
  safe. Candidates: a defence against an open three with scattered counter-fours, or a quiet node,
  where the only saving move has policy below the 1 % tail.

## Decision note (draft)

- **What:** a node whose edge list was cut may not become proven lost from its kept children
  alone. Options: (a) record "edges were cut" in a spare bit and, for such nodes, clamp
  `bound.upper` to a non-mate value when all kept children are proven wins for the opponent;
  (b) on that event, add the cut moves back (needs the safe edge-array replacement of SEARCH-04).
  (a) is the small fix; (b) recovers the proof.
- **Expected:** default-off path (`mcts_edge_policy_mass >= 1`) bit-identical; with the cut, no
  false `+M` / `-M` on the test position; strength equal or better (fewer false mate overrides).
- **How measured:** the failing position test; MCTS fixed-node golden output with the cut off
  (must be identical); self-play vs base with the default `0.99`, same settings as SEARCH-05;
  count of mate overrides on a position set before/after.
- `Node` stays 56 bytes (SEARCH-08): the "cut" flag must use a spare bit, not a new field.

## Function → test-case list (write before code)

- `updateStats_allKeptChildrenProvenWinForOpponentEdgesCut_noProvenLoss`
- `updateStats_allChildrenProvenWinForOpponentNoCut_provenLoss` (current behaviour kept)
- `updateStats_oneChildProvenLossForOpponentEdgesCut_provenWinKept`
- `createEdgesFrom_cutApplied_setsCutFlag`, `createEdgesFrom_noCut_cutFlagClear`
- position test: only saving reply has policy < 1 % in a node with > 8 replies; MCTS with
  `0.99` must not print a mate score nor override the best move.

## Verification

- bench bit-identical (`17621937` / `8c1f72e5`; built-in config is alpha-beta).
- unit tests with asserts live.
