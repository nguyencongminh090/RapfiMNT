# PERF-02 — Instruction

Read [`../todo/PERF-02-insertion-branch-sort.md`](../todo/PERF-02-insertion-branch-sort.md) first.
For background, read PERF-01 ([`../todo/PERF-01-movepick-branch-miss.md`](../todo/PERF-01-movepick-branch-miss.md),
[`../audit/2026-11-04-perf01-move-ordering-research.md`](../audit/2026-11-04-perf01-move-ordering-research.md)).

## Approach

1. **Research first.** Write findings to `docs/audit/<date>-perf02-insertion-branch-research.md`.
   Make no source changes in that step. Prototype in a scratch copy.
   - Run `perf annotate` on `fastPartialSort<ScoreComparator>`.
   - Characterise the negative-tail order.
   - Micro-benchmark the candidates on real dumped lists, n <= 121.
2. Pick option 1 (bit-identical) or option 2 (behaviour-changing). Option 2 needs the user's
   explicit go-ahead.
3. Write the decision note, then the failing tests, then the implementation. Measure as in the
   todo file.

## Pitfalls

- The negative-score tail order is behaviour. "It is unspecified" in the code comment does not
  mean the search does not depend on it: lists are drained.
- `PolicyComparator` also goes through this branch. Its float key does not pack into an integer
  equivalently, so keep it on the generic path unless proven otherwise.
- Lists are short (avg about 15). A method with a fixed setup cost, like a 256-bucket histogram,
  can lose here. Measure, don't assume.
- PGO profiles go stale after a source change: compare plain vs plain and PGO vs PGO.

## Do not touch

- The middle branch (radix sort, PERF-01) and the n > 323 `partial_sort` branch.
- `Board::move`, eval kernels and pruning logic.
