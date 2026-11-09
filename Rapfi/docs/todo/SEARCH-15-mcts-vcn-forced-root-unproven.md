# SEARCH-15 — MCTS does not prove P4 VC3 / VC2 at a forced root

**Status:** open (recorded 2026-11-09, user). Follow-up of SEARCH-13 (partly done).

**Class:** B (MCTS search behaviour, VCN mode only). Investigation first; no fix is decided.

## Problem

- After SEARCH-13 the singular-root stop is skipped under VCN, so MCTS runs the full budget on
  forced roots. P4 VC3 (AB: `-M8`) and P4 VC2 (AB: `+M13`) are still unproven in the slice-5
  Part C grid (5 s / 30 s, 1 / 8 threads): values are ordinary scores (VC3 -522..-789, VC2
  730..778), with no mate bound, after hundreds of millions to billions of nodes.
- Data: `Rapfi/build/search13/partc/results_base.tsv`, `results_after.tsv` (not in git; see the
  Results section of `SEARCH-13-mcts-vcn-singular-root-stop.md`).

## Suspects (unverified)

- pass-edge prior (VCN-03 slices 2-4);
- `Node` bound propagation (proven win/loss not reaching the root);
- edge cut at 0.99 hiding the needed defence/attack move.

## Scope

1. Reproduce on P4 VC3 with a fixed node budget; dump root edges (visits, value, bound, prior).
2. Find which suspect (or another) stops the proof; write the finding with evidence.
3. Decide a fix as a separate step (decision note), then test list before code.

## Out of scope

- Any change before the cause is shown. AB remains the VCN searcher (VCN-03 conclusion), so this
  is low priority.

## Verification

- Cause identified with data. Any later fix: bench `17621937 / 8c1f72e5`, MCTS golden with VCN off
  identical, Part C zero wrong.
