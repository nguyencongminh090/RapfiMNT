# PERF-01 — Instruction

Read [`../todo/PERF-01-movepick-branch-miss.md`](../todo/PERF-01-movepick-branch-miss.md) first.

## Approach

1. **Research first** (assigned to an Opus subagent, 2026-11-04): write findings to
   `docs/audit/2026-11-04-perf01-move-ordering-research.md`; no source changes in that step.
   Cover how `MovePicker` consumes the sorted list, candidate algorithms, expected cost for
   n = 120..320, and the equivalence argument (strict total order).
2. Pick one approach from the research, record it as a decision note in the todo file, then write
   the failing tests, then implement.
3. Measure as in the todo file; record numbers in §Verification.

## Pitfalls

- Class B: the pick order must stay identical; do not "improve" the ordering here.
- `ScoreComparator` and `PolicyComparator` both break ties by `pos`; keep that, or the bench hash
  becomes toolchain-dependent.
- Fix the symptom at its source: the cost is in the sort call site, not in the comparator.
- Bench baseline is the built-in config; the yixin-net config prints a different Nodes/Hash.
- PGO profiles go stale after a source change (BUILD-01): compare plain vs plain, PGO vs PGO.

## Do not touch

`Accumulator::move` and eval kernels (compute-bound, out of scope), `Board::move` (separate item
if ever opened), hash-table and search-pruning logic.
