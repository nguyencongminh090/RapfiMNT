# 2026-11-04 — PERF-01 research: move-list ordering and branch misses

Research step for [`../todo/PERF-01-movepick-branch-miss.md`](../todo/PERF-01-movepick-branch-miss.md)
(instruction: [`../instruction/PERF-01-movepick-branch-miss.md`](../instruction/PERF-01-movepick-branch-miss.md)).
No repo source, test or build file was changed. All instrumentation, prototypes and
micro-benchmarks were built in a throwaway copy under the session scratchpad
(`/tmp/claude-1000/.../scratchpad/{inst,mb,proto}`), not in the repo.

Labels used below: **[measured]** = number from a run done for this report;
**[code]** = read from the current source; **[estimate]** = reasoning, not measured.

## Summary and recommendation

- **Change:** in the middle branch of `fastPartialSort` only (`movepick.cpp:95-97`,
  121 < n <= 323, `ScoreComparator`), replace `std::sort` with an LSD radix sort on a packed
  `uint64` key.
- **Result [measured]:** a scratch prototype keeps Nodes/Hash identical. Median Nodes/s:
  **+22% gcc / +14% clang** (built-in), **+5% clang** (yixin-net). Branch misses fall 43-59%.
- **Lazy selection rejected:** 50-58% of these lists are read to the end, because the pruning
  uses `continue`.
- **Size of the gain:** the todo's "3-4%" ceiling holds only for yixin-net. In the built-in
  bench config the sort is about 25% of cycles.

## Findings

### Q1. How MovePicker consumes the sorted list

**Call sites of `fastPartialSort`** [code]:

| Site | Comparator | Notes |
|---|---|---|
| `movepick.cpp:177-178` ROOT ctor | `PolicyComparator` | only when `useNormalizedPolicy` (MCTS root, `mcts/search.cpp:415-419`) |
| `movepick.cpp:466` `scoreAndSortMoves` | `PolicyComparator` | normalized-policy path (MCTS MAIN, `mcts/search.cpp:427-432`) |
| `movepick.cpp:470` `scoreAndSortMoves` | `ScoreComparator` | AB search: `MAIN_MOVES` (`:502-514`), `DEFENDFOUR_MOVES` (`:529-538`), `DEFENDB4F3_MOVES` (`:540-555`) |
| `movepick.cpp:573` `QVCF_MOVES` | `ScoreComparator` | VCF move lists, usually small |

**What `fastPartialSort` really does** (`movepick.cpp:75-101`) [code]. It is three different
behaviours, and only one of them is a full sort:

- **n <= 121** (`InsertionSortLimit = MAX_MOVES/4`, `MAX_MOVES = 485`, `core/pos.h:39`):
  insertion sort of the moves with `score >= limit`, where `limit = 0` at every call site.
  Moves with a negative score are left in a deterministic but **unsorted** order (`:83-93`).
- **121 < n <= 323**: a full `std::sort` (`:95-97`). This is the hotspot.
- **n > 323** (`SortLimit = MAX_MOVES*2/3`): `std::partial_sort` of the first 323. The tail
  stays in heap order, which is specific to the standard-library implementation (`:98-100`).

So `InsertionSortLimit` and `SortLimit` change behaviour, not just speed, and must stay
exactly where they are. The reference for any replacement is a **frozen copy of today's
`fastPartialSort`, per branch**, not `std::sort`. This means two cases in the todo's test
list would fail against the current code if they used negative scores:

- `fastPartialSort_randomListsAcrossSizeThresholds_matchStdSort`, for n <= 121 and n > 323;
- `fastPartialSort_allEqualScores_orderedByPos`.

They need to be rephrased (see the measurement plan).

**How the list is read** [code]:

- `operator()` (`:479-587`) returns the TT move first, then an optional pass. After that it
  scores and sorts once per stage and falls into `ALLMOVES`. `ALLMOVES` calls
  `pickNextMove<Next>` (`:581`), which walks `curMove` forward, skipping `ttMove` and Renju
  forbidden points (`:288-336`).
- Nothing reads the list by index or random access. The search sees only `move`,
  `curMoveScore()` and `curMoveNormalizePolicy()`, through the picker (`search/ab/search.cpp`).
- LMR uses `moveCount` and `mp.curMoveScore()` (`search.cpp:1113-1114`, `:1787`). Move-count
  pruning (`:1642`) and policy pruning (`:1668-1669`) use `continue`, so the picker keeps
  being drained after the threshold.
- There is no re-use of the list for singular extension or ProbCut. A new picker is built per
  node (`search.cpp:738`, `:1481`, `:2251`).
- `POLICY_TRAINING` builds read the **whole sorted order** right after the sort, in
  `capturePolicyTraceOrder` (`:473`, `:575`, `:657-667`).
- `pickNextMove<Best>`: the `if constexpr (T == Best)` branch (`:294-298`) is **dead**. The
  only instantiation is `pickNextMove<Next>` at `:581`. This confirms
  `bao-cao-toi-uu-kernel-rapfi.md:21`, whose line numbers are older (`:556`).

**How many moves a list actually yields** [measured]. The instrumented scratch copy recorded,
per sorted list, how many entries were consumed and whether the list was exhausted. The
instrumented BENCH still gave Nodes `17621937` / Hash `8c1f72e5`.

| config | branch | lists | avg n | picked 1 | picked 2-16 | picked 65+ | exhausted |
|---|---|---|---|---|---|---|---|
| built-in | n<=121 insertion | 5,299,464 | 14.8 | 23% | 33% | 5% | 80%* |
| built-in | 121<n<=323 `std::sort` | 1,100,299 | 137.6 | **39.6%** | 10% | **50.4%** | **50.4%** |
| yixin-net | n<=121 insertion | 1,049,641 | 14.6 | 23% | 38% | 7% | 82%* |
| yixin-net | 121<n<=323 `std::sort` | 281,604 | 140.8 | **32.7%** | 9% | **58.5%** | **58.5%** |
| both | n>323 `partial_sort` | 0 | - | - | - | - | - |

\* Includes empty lists (n = 0).

- The largest n seen was 184 (built-in) and 186 (yixin-net). Lists of 200-323 do not occur in
  BENCH, so the `partial_sort` branch never runs there.
- The distribution is **bimodal**: a list either cuts off at the first move or is read to the
  end. Almost nothing falls in between. "Most nodes cut off after a few moves" holds for only
  about a third of the sorted lists.

**Profile of the current clang build, built-in config** [measured]
(`build/x64-clang-AVX512VNNI/pbrain-rapfi`, hash `8c1f72e5`, `perf record`, 1 thread):

| symbol | cycles | branch-misses |
|---|---|---|
| `std::__unguarded_partition_pivot<ScoredMove*,ScoreComparator>` | 11.6% | 29.2% |
| `std::__final_insertion_sort<...>` | 7.5% | 15.9% |
| `fastPartialSort<ScoreComparator>` self time | 5.6% | 7.2% |
| `MovePicker::scoreAllMoves<7>` | 12.8% | - |
| `Board::move<Rule 1>` | 13.6% | 9.5% |

The sort is about 25% of cycles and about 52% of branch misses in the bench gate config. The
7.1% / 41.5% in `docs/notes/notes.md` is for yixin-net, where `Accumulator::move` dominates.

Note: `build/pbrain-rapfi` (gcc) is **stale**. It prints `16733809` / `4621a98a`, the
pre-sync baseline. A gcc profile taken from it during this research was discarded.

### Q2. Candidate algorithms (n = 121..323, `ScoredMove` = 8 bytes)

Micro-benchmark [measured]:

- Input: real lists from the middle branch, dumped from the instrumented BENCH (every 4th
  `scoreAndSortMoves` / `ScoreComparator` list; 275,075 built-in, 70,401 yixin-net).
- Replay: each list is replayed with its recorded consumption.
- Hardware: i7-1165G7.
- Counter columns are per list, with the process start-up offset removed (a run with 0
  repetitions was subtracted).

| variant | gcc ns | gcc br-miss | clang ns | clang br-miss | same sequence as `std::sort`? |
|---|---|---|---|---|---|
| `std::sort(ScoredMove, ScoreComparator)` (today) | 4257 | 566 | 3592-3816 | 380-392 | reference |
| `std::sort` on packed `uint64` key | 3140 | 406 | 2913 | 405 | yes, 0 mismatches |
| branchless Lomuto quicksort on `uint64` (median of 3, insertion <= 16) | 2227 | 196 | 1817 | 167 | yes, 0 mismatches |
| **LSD radix 4 x 8-bit on `uint64`** (skip pass if one digit) | **1548** | **14** | **1312** | **13** | yes, 0 mismatches |
| lazy: `min_element` for the first pick, `std::sort` rest on the 2nd | 2784 | 346 | 2673 | 239 | yes, 0 mismatches |
| lazy: branchless min-scan first, radix rest on the 2nd | 1170 | 11 | 961 | 15 | yes, 0 mismatches |

All variants produced byte-identical full sequences on every dumped list in both configs. No
list had a duplicate `pos`.

Assessment of each option:

- **Per-pick lazy selection** (`pickNextMove<Best>`-style) [estimate]. It costs about n per
  pick, so about n²/2 ≈ 9.5k comparisons on the 50-58% of lists that are drained, against
  about n·log2(n) ≈ 1k for a sort. It needs one well-predicted min-scan for one-pick lists.
  It loses overall, so it is rejected.
- **`partial_sort` of a prefix then lazy**. The prefix sort is wasted on one-pick lists, and
  the lazy tail is quadratic on drained lists. Rejected.
- **Packing alone** (`std::sort` on `uint64`). This makes the comparison a single integer
  compare, but introsort's partition branches are still data-dependent. Gain about 25%.
- **Branchless partition (Lomuto)**. About 2x faster, misses cut about 2.5x. The recursion and
  insertion tail still mispredict.
- **Sorting network**. Not practical at n = 121..323 (thousands of compare-exchanges, a size
  that varies per list). Not tried.
- **Counting sort on `clampMoveScore`'s range** (`eval/scoretables.h:74-79` clamps to the full
  `int16`). It needs 65,536 buckets for an exact key, or a second pass for `pos` ties. Rejected
  in favour of radix, which handles the full 32-bit (score, pos) key in 2-4 passes of 256.
- **LSD radix** is the fastest single algorithm. It has almost no data-dependent branches, and
  its cost is linear in n plus a fixed histogram cost.

Equivalence condition shared by all of the above [code]. `ScoreComparator`
(`game/movegen.h:75-81`) orders by `score` descending, then `pos` ascending (signed `int16`,
`core/pos.h:85-94`). This is a strict total order on lists of distinct `pos`, so the sorted
sequence is **unique**. Any correct full sort, stable or not, gives the same sequence.

### Q3. See the recommended approach, risks and measurement plan below.

### Q4. Knowledge lookup (CLAUDE.md hard rule 10)

Two `SKILLS_TREE/tools/find -k knowledge` queries were run:

- "move ordering sort branch mispredictions game engine search performance";
- "CPU micro-optimization profiling branchless code radix sort benchmarking methodology".

**No matching note.** The nearest hits (`apptype-game-engine`, `db-benchmarking-sqlite`,
`torch-performance-tuning`) cover other subjects and were not used. The recommendation rests
on the measurements in this report.

## Recommended approach and equivalence argument

**What.** In `fastPartialSort`, branch `nMoves <= SortLimit` only, change the code to:

```cpp
if constexpr (std::is_same_v<Comparator, ScoredMove::ScoreComparator>)
    radixSortByScore(begin, nMoves);   // NO_INLINE helper (core/platform.h:51/57)
else
    std::sort(begin, end, comp);
```

**How `radixSortByScore` works.**

- Build the key `k = (uint16(32767 - score) << 48) | (uint16(pos ^ 0x8000) << 32) | unionBits`.
  `unionBits` are the 4 bytes of the `rawScore` / `policy` union, carried along, not compared.
- Run a stable LSD radix sort on bits 32..63 (4 passes of 8 bits, with `uint16` histograms).
  A pass is skipped when all keys share that digit.
- Decode back in place. The decode restores every byte of each `ScoredMove`.

The scratch prototype is about 40 lines. Its code is reproduced verbatim in the appendix.

**Equivalence.**

1. **Key map.** `32767 - score` is strictly decreasing in `score` over all of `int16`
   (range 0..65535). `pos ^ 0x8000` is strictly increasing in signed `pos`. So
   `key(a) < key(b)` holds exactly when `ScoreComparator(a, b)`, provided `(score, pos)`
   differ.
2. **Unique order.** Within one list the `pos` values are distinct (measured on all dumped
   lists, 0 duplicates). So the strict total order has exactly one sorted permutation.
   `std::sort` (any library) and the radix sort both produce it.
   - If a duplicate `pos` ever occurred, both copies would have the same `score` (it is a
     function of `pos` within one scoring pass). The stable radix keeps them in input order.
     `std::sort` might swap them, but in the observable fields (`pos`, `score`) the two
     copies are identical.
3. **Same bytes out.** Decoding restores `pos`, `score` and the union bits exactly, so
   `curScore` and `curPolicy` are unchanged.
4. **Other branches untouched.** The insertion branch, the `partial_sort` branch and all
   `PolicyComparator` sites run today's code, so their implementation-defined orders are
   unchanged.

[measured] Bench evidence is identical Nodes/Hash on gcc and clang, built-in and yixin-net.

**Lazy first pick on top of radix** is optional. In the micro-benchmark it beat radix alone by
about 25% per list, but it was not measured end to end. It adds picker state, and it conflicts
with the `POLICY_TRAINING` full-order read (`capturePolicyTraceOrder`). Do it in a later slice
only if an end-to-end run shows a gain larger than run-to-run spread.

## Risks

- **Nodes that drain the list.** These make up 50-58% of middle-branch lists. The radix cost
  does not depend on how many moves are consumed, so draining costs nothing extra. This is why
  radix is preferred over lazy selection.
- **Threshold coupling.** Moving `InsertionSortLimit` / `SortLimit`, or routing the
  `n > 323` branch through radix, **changes the tree**:
  - the tail order differs from `partial_sort`'s heap order;
  - on 20x20 `full_board`, drained lists of more than 323 moves are likely (not measured, since
    BENCH never reaches n > 186).
  Any such change is a separate, behaviour-changing decision.
- **Determinism across standard libraries.** It gets better, not worse: the new path is our own
  integer code, identical on libstdc++, libc++ and MSVC. MSVC was not built or measured.
- **`PolicyComparator` must stay on `std::sort`.** Under that comparator `+0.0f` and `-0.0f`
  compare equal but have different bits, and NaN breaks the order, so a bit-packed float key
  would not be equivalent.
- **Stack.** `keys` + `tmp` + `hist` + `off` is about 10.3 KB with `MAX_MOVES` arrays, or
  about 7.7 KB sized to `SortLimit` (323). Keep it in a `NO_INLINE` leaf helper, so the frame is not folded
  into the recursive `search` frame. MSVC worker threads default to a 1 MB stack.
- **Layout assumption.** `sizeof(ScoredMove) == 8`, with the union at offset 4. Guard it with a
  `static_assert` and `offsetof`.
- **PGO (BUILD-01).** The radix has almost no data-dependent branches, so it should be
  insensitive to the profile [estimate]. Profiles go stale after the change. Compare plain vs
  plain and PGO vs PGO. PGO was not measured here.
- **`POLICY_TRAINING`.** Untouched by radix-only, since the full order is still produced
  eagerly. Not built here.
- **Dump coverage.** Only `scoreAndSortMoves` / `ScoreComparator` middle-branch lists were
  dumped (1 in 4). QVCF lists and `PolicyComparator` lists were not dumped. QVCF lists were
  counted in the consumption statistics.

## Measurement plan

Prototype results already taken [measured]:

- Scratch builds of the current HEAD sources, base vs radix. Same flags as the
  `x64-clang-AVX512VNNI` preset for clang, and the `build/` flags for gcc 15.2.
- Single-thread `BENCH`; `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`.
- 5 interleaved runs (A B A B ...); medians reported.

| build / config | Nodes / Hash (both) | Nodes/s base -> radix | cycles | branch-misses |
|---|---|---|---|---|
| gcc / built-in | 17621937 / 8c1f72e5 | 864,540 -> 1,055,521 (**+22.1%**) | 82.6G -> 69.7G (-15.6%) | 1.103G -> 0.453G (-59%) |
| clang / built-in | 17621937 / 8c1f72e5 | 901,055 -> 1,024,650 (**+13.7%**) | 77.5G -> 66.6G (-14.0%) | 0.867G -> 0.444G (-49%) |
| clang / yixin-net | 4587838 / 4447fd06 | 352,260 -> 370,974 (**+5.3%**) | 53.5G -> 50.5G (-5.6%) | 0.258G -> 0.146G (-43%) |

- The node count is identical across runs, so `cycles:u` measures the work for a fixed tree
  directly and is less noisy than wall-clock Nodes/s.
- The base and radix cycle ranges are **disjoint** in all three build/config pairs:
  - gcc built-in: 82.6-83.2G vs 69.5-69.9G;
  - clang built-in: 74.3-78.3G vs 64.4-67.6G;
  - clang yixin-net: 53.1-54.2G vs 50.4-50.7G.
- So the gain holds with 5 runs, including yixin-net. On Nodes/s alone, the yixin-net ranges
  overlap (325k-358k vs 351k-379k).

Plan for the real implementation (after the decision note is written in the todo file):

1. **Tests first** (in `tests/`, add to `TEST_DRIVER_SOURCES`, `rapfi_tests` with asserts live).
   Rephrase the list so the reference is a frozen copy of today's `fastPartialSort` (copied
   into the test file), per branch:
   - `fastPartialSort_randomListsAcrossSizeThresholds_matchFrozenReference`, for n in
     {0, 1, 2, 120, 121, 122, 200, 322, 323, 324, 485}. Use a seeded `PRNG`, distinct `pos`,
     scores that include negatives, ties and the `int16` extremes (-32768, 32767).
   - `fastPartialSort_middleBranch_matchesStdSortWithScoreComparator`.
   - `fastPartialSort_middleBranch_preservesUnionBits`.
   - `fastPartialSort_policyComparator_matchesFrozenReference`.
   - `fastPartialSort_allEqualScores_middleBranchOrderedByPos`. Restrict the "ordered by pos"
     claim to the middle branch, or use non-negative scores for the insertion branch.
   - `movePicker_mainSearch_moveSequenceUnchangedVsBaseline`. This may need a test-only hook,
     since `fastPartialSort` is in an anonymous namespace.
2. **Bench gate.** gcc 15.2 Release AVX512+VNNI `echo BENCH | ./pbrain-rapfi` must give
   `17621937` / `8c1f72e5`. Also check yixin-net `4587838` / `4447fd06` and a clang build.
3. **Performance.**
   - Command: `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`.
   - 5 interleaved runs, plain vs plain on `x64-clang-AVX512VNNI`.
   - Both configs: built-in, and `--config build/yixin-net/config.toml` run from
     `build/yixin-net/` with BENCH on stdin. The `bench` CLI argument is rejected when command
     modules are compiled out.
   - Report median Nodes/s plus the min-max range. Then rebuild PGO and compare PGO vs PGO.
4. **Confirm the hotspot moved.** Run `perf record -e branch-misses:u -c 20000` and
   `-e cycles:u -F 2000`. The `std::__unguarded_partition_pivot` and `__final_insertion_sort`
   entries should disappear for `ScoreComparator`.
5. Optional: a multi-thread run, and a 20x20 `full_board` spot check that the n > 323 branch
   still uses `partial_sort`.

## Unverified items

- The split of `fastPartialSort`'s 5.6% self-time between the inlined insertion branch and the
  introsort glue. `perf annotate` was not run.
- MSVC / libc++ builds, PGO builds, `POLICY_TRAINING` builds, multi-thread NPS. None was run.
- Boards or configs that produce n > 186, including 20x20 `full_board`. Consumption beyond
  the 323 prefix was 0 in BENCH, but it is not known elsewhere.
- The end-to-end value of lazy-first-pick on top of radix. Only the micro-benchmark was run.
- The micro-benchmark ran on an isolated stream of lists. Branch-predictor and cache state
  differ inside the engine; the end-to-end prototype numbers above are the authoritative ones.
- Whether the `n <= 121` insertion branch (5.3M lists in BENCH) is worth its own item. It is
  out of scope here: its partial order for negative scores is behaviour.

## Appendix A: prototype code (scratch only, verbatim)

This is the diff that produced the identical-hash measurements above. It is applied to a copy
of HEAD `search/movepick.cpp`. It is prototype quality: before landing, use `NO_INLINE` from
`core/platform.h` instead of `[[gnu::noinline]]` and add an `offsetof` guard.

```diff
@@ -29,6 +29,8 @@
 #include <optional>
 #include <array>
 #include <limits>
+#include <cstring>
+#include <type_traits>
 
 namespace {
 
@@ -70,6 +72,49 @@
     }
 }
 
+/// PROTOTYPE (scratch only): full sort under ScoreComparator via an LSD radix sort on a packed
+/// 64-bit key: [63:48] = 32767 - score (descending score), [47:32] = pos ^ 0x8000 (ascending
+/// signed pos), [31:0] = the union bits (carried, not compared).
+[[gnu::noinline]] void radixSortByScore(ScoredMove *begin, size_t n)
+{
+    static_assert(sizeof(ScoredMove) == 8);
+    uint64_t keys[MAX_MOVES], tmp[MAX_MOVES];
+    uint16_t hist[4][256] = {};
+    for (size_t i = 0; i < n; i++) {
+        uint32_t u;
+        std::memcpy(&u, reinterpret_cast<const char *>(&begin[i]) + 4, 4);
+        uint32_t hi = (uint32_t(uint16_t(32767 - int(begin[i].score))) << 16)
+                      | uint16_t(uint16_t(int16_t(begin[i].pos)) ^ 0x8000u);
+        keys[i] = (uint64_t(hi) << 32) | u;
+        hist[0][hi & 255]++;
+        hist[1][(hi >> 8) & 255]++;
+        hist[2][(hi >> 16) & 255]++;
+        hist[3][(hi >> 24) & 255]++;
+    }
+    uint64_t *src = keys, *dst = tmp;
+    for (int p = 0; p < 4; p++) {
+        const int shift = 32 + 8 * p;
+        if (hist[p][(src[0] >> shift) & 255] == n)
+            continue;
+        uint16_t off[256], s = 0;
+        for (int d = 0; d < 256; d++) {
+            off[d] = s;
+            s += hist[p][d];
+        }
+        for (size_t i = 0; i < n; i++)
+            dst[off[(src[i] >> shift) & 255]++] = src[i];
+        std::swap(src, dst);
+    }
+    for (size_t i = 0; i < n; i++) {
+        uint64_t k = src[i];
+        ScoredMove &m = begin[i];
+        m.pos   = Pos(int16_t(uint16_t((k >> 32) & 0xFFFF) ^ 0x8000u));
+        m.score = Score(32767 - int((k >> 48) & 0xFFFF));
+        uint32_t u = uint32_t(k);
+        std::memcpy(reinterpret_cast<char *>(&m) + 4, &u, 4);
+    }
+}
+
 /// Partial sort the move list up to the score limit. It dynamiclly decides
 /// which sorting algorithm to use based on how many moves are in the list.
 template <typename Comparator>
@@ -93,7 +138,10 @@
             }
     }
     else if (nMoves <= SortLimit) {
-        std::sort(begin, end, comp);
+        if constexpr (std::is_same_v<Comparator, ScoredMove::ScoreComparator>)
+            radixSortByScore(begin, nMoves);
+        else
+            std::sort(begin, end, comp);
     }
     else {
         std::partial_sort(begin, begin + SortLimit, end, comp);
```

## Appendix B: scratch artefacts (session-local, may be gone)

- Instrumented copy (consumption counters, list dump): `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/inst/src`, build `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/inst/b`.
- List dumps: `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/inst/builtin.bin`, `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/inst/yixin.bin`.
- Micro-benchmark: `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/mb/mb.cpp`.
- Prototype trees: `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/base`, `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/radix`. Builds: `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/b-{base,radix}-{gcc,clang}`.
- Run logs: `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/clang_builtin.txt`, `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/clang_yixin.txt`, `/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/a888fdc9-9001-4aac-b791-3ce6b793f192/scratchpad/proto/gcc_builtin.txt`.
