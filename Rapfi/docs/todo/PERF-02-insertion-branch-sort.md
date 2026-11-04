# PERF-02 — Branch misses in the short-list (insertion) branch of `fastPartialSort`

**Status:** done (2026-11-04). Research:
[`../audit/2026-11-04-perf02-insertion-branch-research.md`](../audit/2026-11-04-perf02-insertion-branch-research.md);
user chose option 1, candidate C4. Bench bit-identical; see Verification.

**Class:** B. Whether the bench moves depends on the approach chosen (see below). If it moves,
the change needs a recorded reason, the new bench value and a strength check before done.

## Goal

Cut the branch mispredictions left in `fastPartialSort` after PERF-01, now that the middle branch
uses radix sort. Keep the search tree identical if possible. If that is not possible, change it
only with a measured, recorded justification.

## Evidence (measured 2026-11-04, after PERF-01, see `docs/todo/PERF-01-movepick-branch-miss.md`)

- `perf record -e branch-misses:u -c 20000`, clang `x64-clang-AVX512VNNI`, built-in BENCH:
  - `fastPartialSort<ScoreComparator>` self time is **14.7% of all branch misses**, the second
    largest symbol after `Board::move<Rule 1>` (16.6%).
  - `radixSortByScore` is 2.85%.
  - The self time is mostly the inlined insertion branch (n <= 121). The split was not confirmed
    with `perf annotate`.
- List statistics from the PERF-01 research (instrumented BENCH, built-in config):
  - About 5.3M lists, avg n 14.8. That is about 5x as many lists as the middle branch, but short.
  - 23% of lists yield 1 move; about 80% are drained, empty lists included.
  - yixin-net: 1.05M lists, avg n 14.6.

## Why this is behaviour

The branch `n <= InsertionSortLimit` (121) in `search/movesort.h` sorts only moves with
`score >= limit` (`limit = 0` at every call site). Moves with a negative score stay in a
deterministic but **unsorted** order that the swap scheme produces. The search consumes that order,
because lists are drained. Two families of fix:

1. **Bit-identical.** Reproduce the exact output order, including the negative tail, with less
   branchy code. For example: partition the non-negatives with the same displacement rule, then
   sort only that prefix with a branch-light method (sorting network or small radix). The bench
   must stay `17621937` / `8c1f72e5`. First prove what order the current loop leaves the
   negatives in.
2. **Behaviour-changing.** Fully sort (or radix-sort) the whole short list. Simpler and probably
   faster, but it changes the tree. That needs:
   - the reason and the new bench value recorded here;
   - a self-play / strength regression (SPRT-style) showing no Elo loss;
   - an update of the bench baseline in `CLAUDE.md` and `docs/process/change-process.md`.

Prefer 1 if a prototype shows a gain above run-to-run spread. Fall back to 2 only with the user's
decision.

## Decision note (what / expected / how measured)

Decided 2026-11-04 (user choice "A" after the research report).

- **What:** option 1, candidate C4. On builds with `USE_AVX512` and only for
  `ScoredMove::ScoreComparator`, the insertion branch (n <= 121) calls a new
  `NO_INLINE insertionBranchByScore(begin, n, limit)` in `search/movepick.cpp`:
  1. branchless Lomuto partition by `score >= limit`, `begin[0]` always kept in the prefix, the
     below-limit moves left in the same FIFO-queue order as the loop;
  2. stable sort of the prefix by an AVX-512 rank sort on the key
     `((32767 - score) << 48) | ((pos ^ 0x8000) << 32) | arrival_index`.

  Every other ISA and `PolicyComparator` keep today's loop unchanged.
- **Why bit-identical holds:** the research proved the loop is exactly "partition with
  `begin[0]` pinned, then stable sort of the prefix" and checked the model with 0 mismatches on
  6.35M dumped BENCH lists and 1.4M random lists. Stability is required: real
  `DEFENDFOUR`/`DEFENDB4F3` lists contain duplicate `pos` with different union bits.
- **Expected:** bench unchanged (`17621937` / `8c1f72e5`; yixin-net `4587838` / `4447fd06`);
  `cycles:u` -2..-3.5% on built-in, about -0..-1% on yixin-net; `branch-misses:u` -6..-14%.
- **How measured:** `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`, 5
  interleaved runs, plain vs plain, gcc `build/` and `x64-clang-AVX512VNNI`, built-in and
  yixin-net; then PGO vs PGO (`scripts/pgo_build.sh`). `cycles:u` is the primary measure.
- **Not done:** an AVX2/NEON variant (no gain on non-AVX-512 builds); option 2 (full sort of the
  short list, tree-changing: bench would become `11127201` / `997a1b04`), which would be a
  separate search-behaviour item with SPRT.

## Function -> test-case list (written before code; revised after research)

The original case `..._nonNegativePrefixSortedByScoreThenPos` was wrong as written: the prefix
always contains `begin[0]`, even when its score is negative. It is replaced below.

| Unit | Case name | Expected |
|---|---|---|
| `insertionBranchByScore` (AVX-512 builds) | `insertionBranchByScore_randomLists_matchFrozenReference` | bytes identical to the frozen loop for n in {0, 1, 2, 3, 8, 9, 15, 16, 17, 64, 120, 121}, schemes narrow/ties, full int16, extremes only, all negative, none negative |
| same | `insertionBranchByScore_nonZeroLimit_matchesFrozenReference` | same, limit in {-32768, -5, -1, 1, 3, 100, 32767} |
| same | `insertionBranchByScore_duplicatePos_stableByArrival` | duplicate pos with equal score and different union bits keep arrival order (equals frozen) |
| same | `insertionBranchByScore_negativeFirstMove_keptLastInPrefix` | hand example: negative `begin[0]` ends as the last prefix element |
| same | `insertionBranchByScore_negativeTail_isFifoQueueOrder` | hand example: tail equals the queue order worked out by hand |
| same | `insertionBranchByScore_unionBits_preservedPerMove` | multiset of (pos, union bits) unchanged, special float patterns included |
| same | `insertionBranchByScore_emptyOrSingle_isNoop` | n = 0 and n = 1 leave memory untouched |
| `fastPartialSort` | `fastPartialSort_insertionBranchDuplicatePos_matchesFrozenReference` | through the dispatcher, duplicate-pos lists equal the frozen copy (every ISA) |
| `fastPartialSort` | existing `..._scoreComparatorAcrossSizeThresholds_...` / `..._policyComparator..._...` | unchanged, still pass |
| `MovePicker` | existing `movePickerMain_listSizesAcrossSortBranches_sequenceUnchangedVsBaseline` | goldens unchanged |
| bench | gate | `17621937` / `8c1f72e5` (gcc and clang), yixin-net `4587838` / `4447fd06` |

The `insertionBranchByScore_*` cases exist only under `USE_AVX512`; the test binary inherits the
engine's definitions, so the AVX-512 builds (`build/`, `x64-clang-AVX512VNNI`) run them.

## Verification

- 2026-11-04, red step (gcc `build/`, Release, asserts live): the 7 `insertionBranchByScore_*`
  cases failed to compile (`'insertionBranchByScore' is not a member of 'Search'`), as intended.
- Green step: `insertionBranchByScore` in `search/movepick.cpp` (`NO_INLINE`, `USE_AVX512` only,
  `static_assert(sizeof(ScoredMove) == 8)`, `assert(n <= MAX_MOVES / 4)`, early return for
  n < 2), called from the insertion branch of `fastPartialSort` under
  `if constexpr (ScoreComparator)`. Results:
  - gcc `build/` (AVX-512): `rapfi_tests` 79/79 pass; `./rapfi_tests --check-asserts-live` aborts
    on its sentinel, so asserts are live.
  - clang AVX-512 (scratch test build with the preset's options): 79/79 pass.
  - gcc AVX2-only (scratch): 72/72 pass, `insertionBranchByScore_*` skipped by design, and bench
    `17621937` / `8c1f72e5` on the unchanged fallback loop.
- Mutation check: making the tie-break index descending (unstable order for equal keys) failed
  `fastPartialSort_insertionBranchDuplicatePos_matchesFrozenReference`. The duplicate-pos guard
  has teeth. Reverted.
- Bench gate (Nodes / Hash, identical to HEAD `a5eb8bc`):

  | build | built-in | yixin-net |
  |---|---|---|
  | gcc 15.2 Release AVX512+VNNI (`build/`) | 17621937 / 8c1f72e5 | 4587838 / 4447fd06 |
  | clang `x64-clang-AVX512VNNI` | 17621937 / 8c1f72e5 | 4587838 / 4447fd06 |
  | clang `x64-clang-AVX512VNNI` PGO | 17621937 / 8c1f72e5 | 4587838 / 4447fd06 |

- Performance: single thread, `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`,
  interleaved runs; the baseline is HEAD `a5eb8bc` with the same flags, built in a scratch
  worktree. i7-1165G7 at RAPL PL1 24 W (throttled), so absolute Nodes/s is not comparable with
  the PERF-01 tables. Median [min-max]:

  | build / config | runs | cycles:u base -> new | branch-misses:u | instructions:u |
  |---|---|---|---|---|
  | gcc / built-in | 5 | 70.66G [70.01-71.86] -> 68.89G [68.29-69.30] (**-2.5%**, disjoint) | 0.457G -> 0.395G (-13.6%) | -0.9% |
  | clang / built-in | 5 | 65.73G [65.07-66.33] -> 62.75G [62.69-63.38] (**-4.5%**, disjoint) | 0.452G -> 0.396G (-12.4%) | -4.2% |
  | gcc / yixin-net | 10 | 52.11G [51.68-53.82] -> 51.96G [50.95-53.83] (-0.3%, overlap) | 0.151G -> 0.136G (-9.9%) | -0.1% |
  | clang / yixin-net | 10 | 51.37G [50.53-55.49] -> 50.99G [50.11-51.38] (-0.7%, overlap) | 0.146G -> 0.138G (-5.5%) | -1.1% |
  | clang PGO / built-in | 5 | 64.01G [63.39-64.14] -> 61.08G [60.83-62.55] (**-4.6%**, disjoint) | 0.430G -> 0.392G (-8.8%) | -4.6% |
  | clang PGO / yixin-net | 6 | 51.22G [50.84-51.51] -> 50.68G [50.56-51.10] (-1.1%, overlap) | 0.140G -> 0.131G (-6.4%) | -1.2% |

  - On built-in the gain sits at the top of the expected -2..-3.5% range (clang above it) and is
    above run-to-run spread on every toolchain, PGO included. On yixin-net it is -0.3..-1.1%,
    inside spread even with 10 runs: there the change is neutral to slightly positive.
  - Nodes/s (median) built-in: gcc +1.0% (overlap), clang +8.7%, clang PGO +3.8%. Nodes/s is
    noisy under the 24 W limit; `cycles:u` is the primary measure.
  - A first clang run measured a stale binary (`ninja pbrain-rapfi rapfi_tests` aborted on the
    unknown test target in the preset build dir, so nothing was rebuilt: instructions were
    identical to base). It was discarded and re-run after checking that the binary contains the
    new symbol. That discarded run doubles as an A/A control: old code from the repo's own
    preset build against the scratch baseline gave identical instructions (184.30G both) and
    cycle medians within 0.1% (65.82G vs 65.91G), so the two builds use equivalent flags.
- Not done: AVX2/NEON variants and any MSVC build (not compiled; those builds keep the loop, no gain); option 2 (tree-changing),
  which would be a separate item.
