# PERF-01 — Branch-miss reduction in move-list ordering

**Status:** done (2026-11-04). Research (Opus):
`docs/audit/2026-11-04-perf01-move-ordering-research.md`. Implemented as decided below: LSD radix
sort in the middle branch of `fastPartialSort`. Bench bit-identical, Nodes/s +17% built-in
(gcc and clang), +6% yixin-net; see Verification.

**Class:** B (touches search move ordering; behaviour must stay identical unless a decision note
says otherwise). Needs the decision note below, bench regression and live-assert tests before done.

## Goal

Cut branch mispredictions (and the cycles they cost) in `fastPartialSort` / move picking without
changing the search tree, i.e. `echo BENCH | ./pbrain-rapfi` stays Nodes `17621937`, Hash
`8c1f72e5` (built-in config).

## Evidence (measured 2026-11-04, see `docs/notes/notes.md` addenda)

Single-thread `BENCH` with the yixin-net config (`default_candidate_range = full_board`),
clang AVX512 VNNI build, `perf stat` / `perf record`:

- IPC 2.14; LLC miss 2.7%, DRAM stalls under ~1% of cycles (cache miss is **not** the issue).
- Branch miss 3.2% of branches (2.2 MPKI), est. 8-9% of cycles.
- `std::__introsort_loop` + `std::__final_insertion_sort` over `ScoredMove` (`ScoreComparator`)
  = **41.5% of all branch-misses** but only **7.1% of cycles**.
- Others: `Board::move` x3 rules 17.7% of branch-misses (14.9% cycles); `evaluatePolicy` 5.9%;
  `Accumulator::move` 4.4% of misses but 38.4% of cycles (compute-bound, not a target).
- The research (audit report above) re-profiled the **built-in** bench config: there the sort is
  about 25% of cycles and about 52% of branch misses, so the original "3-4% NPS" ceiling only
  holds for yixin-net.

## Decision note (what / expected / how measured)

Decided 2026-11-04 from the research report; the user chose the report's recommendation.

- **What.** In `fastPartialSort`, branch `InsertionSortLimit < n <= SortLimit` (121 < n <= 323)
  only, and only for `ScoredMove::ScoreComparator`, replace `std::sort` with
  `radixSortByScore(begin, n)`: a stable LSD radix sort (up to 4 passes of 8 bits, a pass is
  skipped when all keys share that digit) on a packed `uint64` key
  `[63:48] = 32767 - score`, `[47:32] = pos ^ 0x8000`, `[31:0] = union bits` (carried, not
  compared), decoded back in place.
  - Unchanged: the insertion branch (n <= 121, leaves negative-score moves in a deterministic
    unsorted order), the `partial_sort` branch (n > 323, heap-ordered tail), every
    `PolicyComparator` call (float key: `+0.0`/`-0.0` and NaN make a bit-packed key
    non-equivalent), and both thresholds. Moving a threshold or routing n > 323 through radix
    changes the tree and would be a separate decision.
  - Placement: `fastPartialSort` moves from the anonymous namespace of `search/movepick.cpp` to a
    new header `search/movesort.h` (namespace `Search`) so the unit tests can call it;
    `radixSortByScore` is declared there and defined `NO_INLINE` in `movepick.cpp` (keeps its
    ~10 KB of stack arrays out of the recursive `search` frame), with `static_assert` /
    `offsetof` guards on the `ScoredMove` layout (8 bytes, union at offset 4).
  - Rejected (research Q2): per-pick lazy selection (50-58% of middle-branch lists are drained,
    so it is quadratic where it matters), `partial_sort` prefix + lazy, branchless quicksort
    (slower than radix), counting sort on `int16` (65,536 buckets). Lazy first pick on top of
    radix is a possible later slice, only if measured end to end.
- **Why it is equivalent.** `ScoreComparator` is a strict total order on lists of distinct
  `pos` (tie-break by `pos`, `game/movegen.h`), so the sorted sequence is unique; the key map is
  strictly monotone in (score desc, pos asc) over all of `int16`; decoding restores every byte.
  Hence any correct sort, stable or not, yields the same bytes.
- **Expected.** Bench Nodes/Hash identical (`17621937` / `8c1f72e5`; yixin-net `4587838` /
  `4447fd06`). Prototype numbers from the research (5 interleaved runs, medians): Nodes/s
  gcc built-in +22%, clang built-in +14%, clang yixin-net +5%; `cycles:u` -14..-16% built-in,
  -5.6% yixin-net; branch misses -43..-59%. The real implementation should land near these.
- **How measured.**
  1. `BENCH` Nodes/Hash on gcc 15.2 Release AVX512+VNNI (`build/`, rebuilt first: the binary
     there was stale at research time) and on `x64-clang-AVX512VNNI`; yixin-net config too.
  2. `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`, 5 interleaved runs,
     plain vs plain on `x64-clang-AVX512VNNI`, built-in and yixin-net; report median Nodes/s
     with min-max range and `cycles:u` (node count is fixed, so cycles are the less noisy
     measure). Then PGO vs PGO (BUILD-01; profiles go stale after this change).
  3. `perf record -e branch-misses:u` / `-e cycles:u`: `std::__unguarded_partition_pivot` /
     `__final_insertion_sort` over `ScoreComparator` should disappear.
- **Knowledge lookup (hard rule 10).** The research ran two `SKILLS_TREE/tools/find -k knowledge`
  queries; no matching note. The decision rests on the measurements in the research report.

## Function -> test-case list (written before code)

File `tests/movesort_tests.cpp` (in `TEST_DRIVER_SOURCES`, run via `rapfi_tests`, asserts live).
The reference for `fastPartialSort` is a **frozen copy of today's `fastPartialSort`** inside the
test file, per branch, not plain `std::sort`: the insertion and `partial_sort` branches have
implementation-defined orders that are behaviour (research Q1). This replaces the earlier draft
rows `..._matchStdSort` across all sizes and `allEqualScores_orderedByPos` for all branches,
which would fail against the current code with negative scores.

| Unit | Case name | Expected | Red before impl? |
|---|---|---|---|
| `radixSortByScore` | `radixSortByScore_randomLists_matchStdSortWithScoreComparator` | n in {2, 3, 122, 200, 323, 485}, seeded `PRNG`, distinct `pos`, full-range and narrow-range (many ties) scores: bytes identical to `std::sort` + `ScoreComparator` | yes |
| same | `radixSortByScore_int16Extremes_orderedByScoreDescThenPosAsc` | scores -32768 / -1 / 0 / 32767 and `pos` -32768 / 32767: exact hand-written expected order | yes |
| same | `radixSortByScore_allEqualScores_orderedByPosAscending` | ties fully broken by `pos` | yes |
| same | `radixSortByScore_unionBits_preservedPerMove` | random `uint32` union bits incl. NaN patterns travel with their move | yes |
| same | `radixSortByScore_singleDigitKeys_skippedPassesStillSorted` | equal scores, `pos` within one low byte (3 of 4 passes skipped) | yes |
| same | `radixSortByScore_emptyOrSingle_isNoop` | n = 0 writes nothing, n = 1 unchanged | no (contract guard) |
| `fastPartialSort` | `fastPartialSort_scoreComparatorAcrossSizeThresholds_matchFrozenReference` | n in {0, 1, 2, 120, 121, 122, 200, 322, 323, 324, 485}, negatives, ties, `int16` extremes: bytes identical to frozen copy | no (regression guard) |
| same | `fastPartialSort_policyComparatorAcrossSizeThresholds_matchFrozenReference` | same sizes, `PolicyComparator`, ties in policy | no (regression guard) |
| same | `fastPartialSort_middleBranch_matchesStdSortWithScoreComparator` | n in {122, 200, 323} | no (regression guard) |
| same | `fastPartialSort_middleBranchAllEqualScores_orderedByPos` | n = 200, ties broken by `pos` | no (regression guard) |
| `MovePicker` (MAIN) | `movePickerMain_listSizesAcrossSortBranches_sequenceUnchangedVsBaseline` | drained `Pos` sequence hash and count equal goldens captured from the pre-change picker; fixtures cover n <= 121 (15x15 `SQUARE3_LINE4`), 121 < n <= 323 (15x15 `FULL_BOARD`), n > 323 (20x20 `FULL_BOARD`), Freestyle and Renju | no (regression guard) |
| bench | gate, not a unit test | Nodes `17621937`, Hash `8c1f72e5` | - |

Red step: the `radixSortByScore` cases run against a declared stub (no-op body) and must fail
before the real body is written; the frozen-reference and `MovePicker` cases are regression
guards that pass before and must still pass after.

## Verification

- 2026-11-04, red step (gcc `build/`, Release, `rapfi_tests` with asserts live, confirmed by
  `./rapfi_tests --check-asserts-live` aborting on its sentinel):
  - Scaffolding only: `fastPartialSort` moved verbatim to `search/movesort.h`;
    `radixSortByScore` declared there with a no-op stub in `movepick.cpp`, not yet called by
    `fastPartialSort`.
  - Regression guards pass: the 4 `fastPartialSort_*` cases and
    `movePickerMain_listSizesAcrossSortBranches_sequenceUnchangedVsBaseline`. Goldens were
    captured from the unchanged picker: 86 moves (insertion branch), 218 / 218 (middle branch,
    Freestyle / Renju), 393 (partial_sort branch).
  - `radixSortByScore_emptyOrSingle_isNoop` passes (contract guard). The first content case,
    `radixSortByScore_int16Extremes_orderedByScoreDescThenPosAsc`, fails against the stub
    (`Assertion 'sameBytes(list, expected)' failed`): red as intended. The later radix cases do
    not run, because `assert` aborts the process.
  - Bench after the header move: `echo BENCH | build/pbrain-rapfi` gave Nodes `17621937`, Hash
    `8c1f72e5`, unchanged. This also refreshes the stale `build/` binary noted in the research.
  - The `MovePicker` golden check hashes the first `SortLimit` moves on every toolchain, and
    the full sequence only under libstdc++ (`__GLIBCXX__`), because the `partial_sort` tail of
    the 20x20 fixture is heap order and differs between standard libraries. No preset uses
    `-stdlib=libc++` today.
  - `rapfi_tests` / `ctest` are intentionally red until the implementation lands; commit the
    tests together with the green implementation.
- Constraints the tests pin for the implementation:
  - Size `keys` / `tmp` to `MAX_MOVES`, not `SortLimit`: the random-list case calls the helper
    with n = 485. Add `assert(n <= MAX_MOVES)`.
  - Return early for n <= 1. The prototype's pass-skip check reads `keys[0]` even when n = 0,
    and the no-op stub hides that, so `emptyOrSingle_isNoop` passing today proves nothing about
    the real body.
  - Define the helper with `NO_INLINE`, and add `static_assert(sizeof(ScoredMove) == 8)` plus an
    `offsetof` check that the union sits at offset 4.
- 2026-11-04, green step: `radixSortByScore` implemented in `search/movepick.cpp` (`NO_INLINE`,
  layout `static_assert`s, `assert(n <= MAX_MOVES)`, early return for n <= 1, arrays sized
  `MAX_MOVES`), called from the middle branch of `fastPartialSort` under
  `if constexpr (ScoreComparator)`. `rapfi_tests` 71/71 pass with asserts live (all 11 PERF-01
  cases).
- Bench gate (Nodes / Hash, all identical to the pre-change baseline built from HEAD `962fedd`):

  | build | config | before | after |
  |---|---|---|---|
  | gcc 15.2 Release AVX512+VNNI (`build/`) | built-in | 17621937 / 8c1f72e5 | 17621937 / 8c1f72e5 |
  | clang `x64-clang-AVX512VNNI` | built-in | 17621937 / 8c1f72e5 | 17621937 / 8c1f72e5 |
  | clang `x64-clang-AVX512VNNI` | yixin-net | 4587838 / 4447fd06 | 4587838 / 4447fd06 |

- Performance: single thread, `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`,
  5 interleaved runs (base, new, base, ...), baseline = HEAD `962fedd` built with the same flags in
  a scratch worktree; i7-1165G7, RAPL PL1 44 W. Median [min-max]:

  | build / config | Nodes/s base -> new | cycles:u base -> new | branch-misses:u |
  |---|---|---|---|
  | gcc / built-in | 840.7k [798.6k-871.8k] -> 986.4k [983.9k-1023.0k] (**+17.3%**) | 83.8G [82.4-84.1] -> 70.3G [70.0-71.3] (-16.1%) | 1.100G -> 0.457G (-58.5%) |
  | clang / built-in | 898.4k [516.3k-962.1k] -> 1047.2k [955.1k-1094.9k] (**+16.6%**) | 75.4G [74.9-91.3] -> 65.4G [64.6-66.0] (-13.3%) | 0.864G -> 0.442G (-48.8%) |
  | clang / yixin-net | 302.8k [284.9k-309.8k] -> 320.6k [285.3k-324.1k] (**+5.9%**) | 56.2G [55.7-57.0] -> 53.4G [52.7-54.7] (-4.9%) | 0.259G -> 0.147G (-43.3%) |

  - The node count is fixed, so `cycles:u` is the primary measure; base and new cycle ranges are
    disjoint in all three pairs. Clang base run 3 was an outlier (516k Nodes/s, 91.3G cycles,
    likely background load); the median is robust to it.
  - Instructions: gcc +5.4%, clang -2.2%, yixin-net -1.0%. The gain comes from fewer
    mispredictions, not fewer instructions.
  - Matches the research prototype (gcc +22%, clang +14%, yixin-net +5%) within run-to-run spread.
- Hotspot moved (`perf record -e branch-misses:u -c 20000`, clang, built-in):
  `std::__unguarded_partition_pivot` / `std::__final_insertion_sort` over `ScoreComparator` are
  gone; `radixSortByScore` is 2.85% of branch misses. `fastPartialSort<ScoreComparator>` self
  time is still 14.7% of branch misses: that is the inlined insertion branch (n <= 121), which the
  research left out of scope because its partial order for negative scores is behaviour. A
  possible follow-up item, not opened.
- PGO vs PGO (clang `x64-clang-AVX512VNNI`, built-in, same 5-run interleaved protocol). Base:
  the pre-change BUILD-01 PGO binary (built 2026-11-04 00:22 from HEAD sources, BENCH
  17621937 / 8c1f72e5). New: `scripts/pgo_build.sh x64-clang-AVX512VNNI` rerun after the change,
  BENCH 17621937 / 8c1f72e5.
  - Nodes/s 934.5k [919.3k-1034.6k] -> 1106.8k [1068.7k-1137.5k] (**+18.4%**);
    `cycles:u` 73.0G [71.8-73.4] -> 62.9G [62.4-63.6] (-13.9%, disjoint ranges);
    branch misses 0.851G -> 0.430G (-49.4%); instructions -2.7%.
  - So the gain survives PGO, and the profile in `build/x64-clang-AVX512VNNI-pgo-use` is now
    regenerated for the new sources.
- Exemptions: self-play is not run because the search tree is bit-identical (same Nodes/Hash on
  every build/config); a self-play match would replay the same games at fixed nodes. Self-play is
  mandatory for class A (CLAUDE.md hard rule 8), not for a class-B change with an unchanged tree.
- Not run: multi-thread NPS, MSVC / libc++ builds, `POLICY_TRAINING` builds. A `rapfi_tests`
  build under clang was not done (gcc only); the tests use nothing compiler-specific.
