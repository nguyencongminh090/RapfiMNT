# SEARCH-04 — MCTS progressive widening

**Status:** closed 2026-11-09 (user), not done. Recorded 2026-11-07.

**Reason:** SEARCH-10 already re-adds cut moves when every kept reply is lost, with a retire list freed at the search boundary, so epoch-based reclamation and hazard pointers are not needed. No measurement shows a strength loss from one-shot cutting beyond that case. General widening would also weaken the SEARCH-03 memory bound. Reopen only if a measurement on the real network shows many missed good moves (compare SEARCH-11: 0.012 %). No code change.
**Class:** B.

## Problem

SEARCH-02 cuts edges once, at creation (`mcts_edge_policy_mass`). A move cut at that point can never
come back, which is a risk for tactical moves with low prior. Adding edges later means replacing a
published `EdgeArray`, while other threads may be reading the old one.

## Scope

- Widen a node's edge set as its visit count grows (for example add the next policy-ranked moves at
  visit thresholds).
- A safe reclamation scheme for the replaced array: epoch-based reclamation or hazard pointers.
  Rejected for now: chaining a tail array (breaks the "visited edges form a prefix" assumption in
  `selectChild` and `Node::updateStats`).

## Test list

To be written before code: single-thread widening order, concurrent widening plus reads (stress, TSan
if available), memory accounting after replacement.

## Measure

Self-play against SEARCH-02 settings at equal time; peak RSS. Expected gain is strength at a given
memory, since SEARCH-02 alone is only equal to the base.
