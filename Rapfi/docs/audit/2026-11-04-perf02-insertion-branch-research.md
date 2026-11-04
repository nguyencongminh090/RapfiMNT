# 2026-11-04 — PERF-02 research: the insertion branch of `fastPartialSort`

Research step for [`../todo/PERF-02-insertion-branch-sort.md`](../todo/PERF-02-insertion-branch-sort.md)
(instruction: [`../instruction/PERF-02-insertion-branch-sort.md`](../instruction/PERF-02-insertion-branch-sort.md)).
No repo source, test or build file was changed. All instrumentation, prototypes and
micro-benchmarks live in a scratch git worktree and copies under the session scratchpad
(paths in the appendix).

Labels: **[measured]** = number from a run done for this report; **[code]** = read from the
current source (HEAD `a5eb8bc`); **[estimate]** = reasoning, not measured; **[unmeasured]** =
not run.

## Summary and recommendation

- **Bit-identical option found and verified.** The insertion branch is exactly:
  (1) a Lomuto-style partition by `score >= limit`, with `begin[0]` always kept in the prefix,
  then (2) a *stable* sort of that prefix under the comparator. Both steps can be done without
  per-element data-dependent branches.
- **Recommended candidate: C4 "branchless partition + AVX-512 stable rank sort".**
  - It matches the frozen reference byte for byte on all 6.35M dumped lists (built-in and
    yixin-net) and on 1.12M randomised lists. That includes negatives, ties, `int16` extremes and
    duplicate `pos` with different union bits [measured].
  - Bench is unchanged on gcc and clang, built-in (`17621937` / `8c1f72e5`) and yixin-net
    (`4587838` / `4447fd06`) [measured].
  - Engine `cycles:u`, 5 interleaved runs, median [min-max] [measured]:
    - clang built-in: **-3.4%**, ranges disjoint;
    - gcc built-in: **-2.2%**, ranges disjoint;
    - yixin-net: -0.3% (clang) and -1.0% (gcc), inside run-to-run spread.
  - Branch misses fall 11-14% (built-in) and 6-7% (yixin-net).
- **The gain is smaller than the todo's 14.7% share suggests, and comes from one list type.**
  - Almost all of the cost is in `MAIN_MOVES` lists with 65 <= n <= 121 (avg n 110, avg prefix
    64). They are 12% of the lists.
  - The 5.3M-list average of n = 14.8 hides this. 59% of the lists are QVCF lists with n <= 1,
    which cost nothing.
- **Option 2 (fully sort the short list) has no speed case.**
  - Per list it is no faster than C4 in the micro-benchmark.
  - It changes the tree heavily: built-in BENCH goes `17621937` -> **`11127201` / `997a1b04`**
    (-36.9% nodes), and yixin-net goes `4587838` -> **`5097934` / `fcb63f2b`** (+11.1%).
  - Built-in BENCH finishes about 36% sooner (10.7 s vs 16.6 s), but only because the tree is
    37% smaller. yixin-net needs 11% more nodes. The strength effect could go either way, and
    only an SPRT can tell.
  - So it is a search-behaviour change, to be judged by SPRT, not a speed-up. Not recommended
    under PERF-02.
- **Recommendation:** implement C4 as option 1, gated on `USE_AVX512`. Every other ISA keeps
  today's loop, which is bit-identical by construction. Keep `PolicyComparator` on the generic
  loop.
  - Expected value: about 2-3.5% of cycles on the built-in config, and about 0-1% (not resolved)
    on yixin-net.
  - Whether a ~2-3% gain justifies about 60 lines of AVX-512 code is the user's call. It is above
    spread on built-in, and not shown on yixin-net.

## Method and environment

- **Machine and power.** i7-1165G7 (Tiger Lake, AVX-512). RAPL PL1 was 24 W during all runs
  (`/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw = 24000000`), the throttled
  state noted in memory. That lowers absolute Nodes/s, but base and candidate share it, and the
  runs are interleaved. `cycles:u` with a fixed node count is the primary measure.
- **Builds.** Single thread, scratch worktree of HEAD `a5eb8bc`:
  - clang: `x64-clang-AVX512VNNI` cache variables (Release, SSE/AVX2/AVX512/VNNI, BMI2 off).
  - gcc 15.2: the `build/` cache (Release, AVX2/AVX512/BMI2/VNNI, `NO_COMMAND_MODULES=ON`).
  - Flags in both: `-ffp-contract=off -mavx512f -mavx512dq -mavx512bw -mavx512vnni -mavx512vl -mbmi2 -O3`.
- **Instrumentation.** Each list entering the insertion branch was dumped (header
  `{site, comparator, n}` + raw 8-byte `ScoredMove`s). The site is the `MovePicker` stage at the
  call. The instrumented BENCH still gave `17621937` / `8c1f72e5` and `4587838` / `4447fd06`.
- **Micro-benchmark.** `mb/mb.cpp` replays the dump in recorded order.
  - Counters come from in-process `perf_event_open` (cycles, instructions, branch-misses,
    branches; user only), compiled with the engine flags.
  - Per-list numbers include a copy of the list into a work buffer, about 47 cycles. The `copy`
    row is that overhead.
- **Engine runs.** `perf stat -e branch-misses:u,branches:u,cycles:u,instructions:u`, 5
  interleaved runs (A B [C] A B [C] ...). Medians with [min-max].
- **Profiling.** A separate clang build with `-g` added (`fastPartialSort<ScoreComparator>` has
  the same size, 0x20f bytes). `perf record -e branch-misses:upp -c 20000` and
  `-e cycles:upp -F 2000` (PEBS, so samples land on the eventing instruction), then
  `perf annotate --stdio`.

## Findings

### Q1. Exact semantics of the insertion branch [code + measured]

```cpp
for (ScoredMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
    if (p->score >= limit) {
        ScoredMove tmp = *p, *q;
        *p             = *++sortedEnd;
        for (q = sortedEnd; q != begin && comp(tmp, *(q - 1)); --q) *q = *(q - 1);
        *q = tmp;
    }
```

**Invariant.** Before step p:

- `[begin, sortedEnd]` is the sorted prefix;
- `(sortedEnd, p)` holds only moves with `score < limit` (call them "low" moves);
- `[p, end)` is untouched.

**Proof by induction:**

- If `p->score < limit`, nothing moves and the low region grows by one.
- Otherwise, `*++sortedEnd` is the first low move. If the low region is empty, it is `*p`
  itself. It is written to slot `p`, which is the end of the low region. `tmp` is then inserted
  into the prefix, which grows by one.

The insertion step never touches the low region, so the two parts are independent.

The output order is therefore:

1. **Prefix (positions `0 .. m-1`).** It holds `begin[0]` **whatever its score**, plus every
   `p >= 1` with `score >= limit`.
   - The prefix is sorted by `comp` and is **stable**. An element moves left only while
     `comp(tmp, prev)` is strictly true, so comp-equal elements keep their arrival order.
   - Arrival order is `begin[0]`, then the qualifying moves in input order.
   - Under `ScoreComparator`, a negative `begin[0]` sorts to the **last** prefix slot.
   - This happens a lot. `begin[0]` is negative in 51% of built-in `MAIN_MOVES` lists and 83%
     of yixin-net `MAIN_MOVES` lists [measured].
2. **Low tail (positions `m .. n-1`).** The low region behaves as a **FIFO queue**, read from
   left to right:
   - a low move found at `p` joins the back of the queue;
   - a qualifying move found at `p` moves the queue's front element to the back (into slot
     `p`). This is a rotation by one.

   The tail is the final queue contents, front to back. It is neither stable nor reversed. It
   depends on how qualifying and low moves are interleaved, which is the order Lomuto's partition
   produces.

**Equivalent model** (`mb/sorts.h: modelIns`): copy `begin[0]` plus the qualifying moves to
`pre[]` in arrival order, doing `a[p] = a[++j]` for each qualifying `p`; then `stable_sort(pre)`
and copy back.

**Check of the model against the frozen reference** (`refIns`, a verbatim copy of the loop)
[measured]. Mismatches:

| input | lists | ScoreComparator | PolicyComparator |
|---|---|---|---|
| built-in BENCH dump | 5,299,464 | 0 | - (none in BENCH) |
| yixin-net BENCH dump | 1,049,641 | 0 | - (none in BENCH) |
| random, n in {0,1,2,3,15,64,120,121}, 7 schemes x 20k | 1,120,000 | 0 | 0 |

The 7 random schemes:

- narrow scores with about 50% negatives;
- heavy ties;
- full `int16` range;
- only `{-32768, 0, 32767}`;
- all negative;
- none negative;
- **duplicate `pos` with equal score but different union bits.**

Policy values in the random lists include ties and +-0.

**Stability is load-bearing, not theoretical.**

- Real `DEFENDFOUR_MOVES` and `DEFENDB4F3_MOVES` lists contain **duplicate `pos`** (2,122
  built-in and 576 yixin-net duplicate pairs). The `DEFEND_FOUR` and VCF generators overlap.
- The duplicates can carry different union bits. A negative-control variant ranked by
  `(key, union bits)` instead of `(key, arrival index)` mismatched **1,027 real built-in lists**
  and 77,850 random lists.

So any replacement must be stable. PERF-01's "distinct `pos`, so the order is unique" argument
does **not** carry over to this branch.

### Q2. Where the time and the mispredictions are (clang, built-in, `-g` build) [measured]

Whole-program shares:

| symbol | cycles:upp | branch-misses:upp |
|---|---|---|
| `fastPartialSort<ScoreComparator>` (not inlined: a weak out-of-line symbol) | 6.07% | 13.8% (14.2% non-PEBS) |
| `radixSortByScore` | 8.69% | 2.84% |
| `Board::move<Rule 1>` | 15.7% | 17.1% |

Inside `fastPartialSort<ScoreComparator>` (`perf annotate`, region sums):

| region | cycles:upp | branch-misses:upp |
|---|---|---|
| inner shifting loop (`comp(tmp, *(q-1))` + `q != begin`) | **69.0%** | **72.7%**: comp exit `cmp/jne` 65.8%, `q != begin` 6.9% |
| outer loop + `score >= limit` test + swap | 28.9% | 27.3%: `score >= limit` `cmp/jl` 24.0%, `p < end` 3.4% |
| size dispatch, radix tail-call, `partial_sort` | 0.1% | 0% |
| entry / epilogue | 2.0% | 0% |

- The self time is entirely the insertion branch. The middle-branch dispatch (`ja` + tail jump to
  `radixSortByScore`) and the `partial_sort` code carry no measurable samples.
- The inner shift loop is about 70% of both cycles and misses. The `score >= limit` test is about
  a quarter of the misses.
- So partition-only fixes, such as C1 below, cannot win much.

### Q3. Real list distribution (ScoreComparator only; no PolicyComparator list occurs) [measured]

Built-in config, 5,299,464 lists:

| site | share of lists | avg n | n distribution | avg prefix m | negatives (% of moves) | `begin[0]` < 0 | lists with prefix ties | dup-pos pairs |
|---|---|---|---|---|---|---|---|---|
| `MAIN_MOVES` | 11.7% | **110.7** | 100% in 65..121 | **64.4** | 42.3% | 51.0% | 99.2% (32.8 tie pairs/list) | 0 |
| `DEFENDFOUR_MOVES` | 27.2% | 4.5 | 56% 2-4, 38% 5-8 | 4.5 | 1.2% | 2.6% | 6.5% | 2,101 |
| `DEFENDB4F3_MOVES` | 2.3% | 7.4 | 59% 5-8, 29% 9-16 | 7.0 | 8.3% | 20.9% | 16.2% | 21 |
| `QVCF_MOVES` | 58.8% | 0.7 | 65% n=0, 11% n=1 | 2.0 | 0.0% | 0.0% | 2.1% | 0 |

yixin-net config, 1,049,641 lists:

| site | share | avg n | avg m | negatives | `begin[0]` < 0 | prefix ties | dup pairs |
|---|---|---|---|---|---|---|---|
| `MAIN_MOVES` | 11.2% | 108.8 (all 65..121) | 62.9 | 42.9% | 83.5% | 97.3% (9.1 pairs/list) | 0 |
| `DEFENDFOUR_MOVES` | 32.5% | 5.3 | 5.3 | 0.1% | 0.1% | 7.1% | 468 |
| `DEFENDB4F3_MOVES` | 3.2% | 8.4 | 8.3 | 0.6% | 1.8% | 13.6% | 108 |
| `QVCF_MOVES` | 53.1% | 0.8 | 2.0 | 0.0% | 0.0% | 1.7% | 0 |

Consequences:

- **Work is concentrated in the 12% of lists that are `MAIN_MOVES`.** These have
  65 <= n <= 121, about 47 negatives and about 64 prefix moves each. Reference cost
  [measured, clang micro]: 6,744 cycles and 84 branch misses per list, against 28 cycles for
  n <= 1 and 134 for n = 2..8.
- **Ties in the prefix are the norm in `MAIN_MOVES`.** Order among them is by `pos` (the
  comparator), and among duplicates by arrival.
- Option 2 (full sort) changes the output of **11.3%** (built-in) and **11.0%** (yixin-net) of
  lists. That is essentially every `MAIN_MOVES` list.

### Q4. Candidates (ScoreComparator, n <= 121)

All candidates except C0 and the `o2_*` rows share the bit-identical decomposition from Q1.

**Branchless partition.** For each `p >= 1`:

- `y = a[j+1]`, `f = score >= limit`;
- `a[p] = f ? y : a[p]` (done as a 64-bit mask select, not a branch);
- `pre[j+1] = a[p]`, then `j += f`.

Only the queue front is ever moved, and the prefix slots `a[0..m)` are overwritten afterwards.

**Prefix sort.** Stability is obtained by sorting on `key = (k32 << 32) | arrival_index`, where
`k32 = (uint16(32767 - score) << 16) | (pos ^ 0x8000)`. This is the PERF-01 key with the index
as the low half. The keys are distinct, and ascending key order equals stable `ScoreComparator`
order.

**C4 rank sort.** `rank_i = #{j : key_j < key_i}`, computed as a broadcast plus
`_mm512_cmplt_epu64_mask` plus `popcount` over 8 keys per step, with padding keys `UINT64_MAX`.
Then `a[rank_i] = pre[i]`. Cost is about m*ceil(m/8) vector compares, with no data-dependent
branch except the trip counts.

**Verification** [measured, full output in `mb/verify_all.txt`]:

- Every bit-identical candidate in the table had 0 mismatches on both dumps and on all 1.12M
  random lists, with both the clang and the gcc micro-benchmark binaries.
- `ce_engine_c4` passed the same checks. It is a verbatim port of the engine prototype's
  `insertionBranchByScore`, with `ScoredMove` replaced by `SM` and `popcount` by
  `__builtin_popcountll`. It also had 0 mismatches on 280k extra random lists with
  `limit` in {-32768, -5, -1, 1, 3, 100, 32767}.
- The negative control `cx_unstable_ctrl` failed as expected: 1,027 built-in lists, 262
  yixin-net lists and 77,850 random lists. This shows the duplicate-`pos` check has teeth.
  It is the only reason `mb verify` / `mb random` exit with code 1.

Micro-benchmark per list, on the full built-in dump (5.30M lists, replayed 2x), engine flags
[measured]:

| name | bit-identical | clang cyc | clang br-miss | gcc cyc | gcc br-miss |
|---|---|---|---|---|---|
| `copy` (overhead only) | - | 46 | 0.78 | 47 | 0.79 |
| C0 `c0_ref` (frozen loop) | reference | 802-871 | 11.7 | 690-741 | 12.4-14.6 |
| C1 branchless partition + insertion on `ScoredMove` | yes | 909 | 11.8 | 853 | 10.7 |
| C2 branchless partition + insertion on packed key | yes | 733 | 13.1 | 936 | 16.5 |
| C7 branchy partition + insertion on packed key | yes | 714 | 12.9 | 864 | 19.9 |
| C3 branchless partition + scalar rank sort | yes | 459 | 4.0 | 673 | 1.5 |
| **C4 branchless partition + AVX-512 rank sort (popcount)** | **yes** | **332-363** | **3.6** | **386-406** | **1.5** |
| C8 C4 transposed (8 ranks per vector, masked add) | yes | 336-343 | 4.1-4.6 | 428-506 | 2.5-5.8 |
| C5 branchless partition + LSD radix of the prefix | yes | 853 | 6.5 | 1189 | 3.8 |
| C6 hybrid: rank <= 16/32/48, else radix | yes | 429-484 | 4.6 | 555-642 | 2.3-2.5 |
| C10 hybrid: n <= 8/16/32 reference loop, else C4 | yes | 367-375 | 4.3-4.6 | 388-394 | 2.2-2.4 |
| O2 `o2_rankT`: full stable rank sort of the whole list | **no** (11.3% of lists differ) | 436-473 | 2.9-3.3 | 383-409 | 3.0-4.0 |
| O2 `o2_radix`: full LSD radix of the whole list | **no** | 952-1174 | 3.5-5.0 | 1222-1227 | 3.7-3.8 |

Ranges are across separate invocations (`mb_agg.txt`, `mb_builtin.txt`).

Per n bucket, built-in, clang / gcc cycles per list (`mb_buckets.txt`):

| n | lists (x3 reps) | C0 ref | C4 | C8 | O2 rankT |
|---|---|---|---|---|---|
| 0-1 | 7.08M | 28 / 29 | 29 / 34 | 28 / 34 | 28 / 35 |
| 2-8 | 6.61M | 134 / 133 | 132 / **173** | 129 / 148 | 123 / 119 |
| 9-16 | 0.34M | 460 / 451 | 353 / 344 | 259 / 639 | 236 / 257 |
| 17-64 | 1.2k | 809 / 807 | 468 / 533 | 415 / 539 | 348 / 375 |
| 65-121 | 1.86M | **6,744 / 5,422** | **2,559 / 2,622** | 2,330 / 3,210 | 3,248 / 2,783 |

Takeaways:

- **The setup-cost pitfall is real but small.** On gcc, C4 costs about 40 cycles more per list
  at n = 2..8. Keeping the reference loop for n <= 8 (C10) removes that, but the aggregate gain
  is within micro-benchmark noise (gcc 388 vs 396, clang 367 vs 363). C4 alone is the simpler
  choice.
- **Radix loses on these lists.** Its four 256-entry histograms cost more than they save at
  m of about 64 (C5, C6). This confirms the instruction's warning.
- **C8 is compiler-sensitive.** It is the best on clang, but gcc compiles its lane-scatter loop
  badly (15.7 misses per list at n = 9..16). C4 is consistent on both compilers.
- **Insertion on a packed key (C2, C7) halves the instructions but not the misses.** The exit
  branch of the shift loop stays unpredictable.
- **Engine estimate from the micro-benchmark** [estimate]: (C0 - C4) x 5.30M lists is about
  2.7G cycles on clang (about 4% of 66G) and about 1.6G on gcc (about 2.2% of 70G). The measured
  engine deltas below are 2.25G and 1.57G.

### Q5. Engine prototype C4: perf stat, 5 interleaved runs [measured]

The prototype is in `proto/c4`:

- `insertionBranchByScore(ScoredMove*, size_t, Score limit)` is `NO_INLINE` in `movepick.cpp`;
- `fastPartialSort` calls it under `#ifdef USE_AVX512` + `if constexpr (ScoreComparator)`;
- every other comparator and ISA runs the unchanged loop.

| build / config | Nodes / Hash (base = C4) | Nodes/s base -> C4 | cycles:u base -> C4 | branch-misses:u | instructions:u |
|---|---|---|---|---|---|
| clang / built-in | 17621937 / 8c1f72e5 | 1062.8k [1028.5-1092.6] -> 1105.2k [1008.1-1120.2] (+4.0%) | 65.98G [65.54-66.26] -> 63.73G [63.69-64.03] (**-3.4%**, disjoint) | 0.449G -> 0.398G (-11.4%) | 184.3G -> 176.5G (-4.2%) |
| gcc / built-in | 17621937 / 8c1f72e5 | 1064.4k [1048.1-1104.1] -> 1102.7k [1057.1-1123.3] (+3.6%) | 70.20G [69.84-70.58] -> 68.63G [68.06-69.21] (**-2.2%**, disjoint) | 0.456G -> 0.393G (-13.8%) | 186.4G -> 184.6G (-0.9%) |
| clang / yixin-net | 4587838 / 4447fd06 | 362.6k [315.4-370.8] -> 366.2k [344.6-374.7] (+1.0%) | 51.23G [50.39-53.08] -> 51.07G [50.87-52.01] (-0.3%, **overlap**) | 0.146G -> 0.137G (-6.2%) | 111.7G -> 110.4G (-1.1%) |
| gcc / yixin-net | 4587838 / 4447fd06 | 359.3k [355.1-369.7] -> 364.4k [360.7-371.6] (+1.4%) | 51.62G [51.25-51.97] -> 51.08G [50.81-51.68] (-1.0%, **overlap**) | 0.148G -> 0.137G (-7.4%) | 122.5G -> 122.3G (-0.1%) |

- Run-to-run spread of `cycles:u` within a variant: 0.5-1.1% on built-in, 1.2-5.3% on
  yixin-net (clang base run 4 was 53.08G).
- Nodes/s ranges overlap in every pair, so Nodes/s alone does not resolve the gain.
- On built-in the measured cycle drop matches the micro-benchmark estimate within about 15%.
- yixin-net: the expected gain of about 0.9% is at or below spread with 5 runs. More runs (10+
  interleaved) would be needed to claim it [unmeasured].
- PGO vs PGO: [unmeasured].

### Q6. Option 2: fully sort the whole short list (tree-changing) [measured]

Prototype `proto/o2`: under `ScoreComparator`, the n <= 121 branch calls `radixSortByScore` on
the whole list, which gives the same unique order as `std::sort` up to duplicate ties.

| build / config | Nodes / Hash | cycles:u (5 runs) | Nodes/s | BENCH total time |
|---|---|---|---|---|
| clang / built-in | **11127201 / 997a1b04** (-36.9% nodes) | 42.47G [42.22-44.45] (-35.6% vs base) | 1043.1k [871.4-1098.9] (-1.9%) | 10.7 s vs 16.6 s |
| gcc / built-in | **11127201 / 997a1b04** | 46.39G [46.23-46.79] (-33.9%) | 1055.8k [1010.8-1060.3] (-0.8%) | 10.5 s vs 16.6 s |
| clang / yixin-net (1 run) | **5097934 / fcb63f2b** (+11.1% nodes) | - | 321.8k (1 run) | - |

- **Per-list speed.**
  - Option 2 is not faster than C4 per list. With the best full-sort kernel (`o2_rankT`):
    clang 436-473 vs 332-363 cycles, gcc 383-409 vs 386-406.
  - With radix as the full-sort kernel it is 2-3x slower than C4.
  - The engine prototype used radix, so its Nodes/s is pessimistic. A rank-sort version would
    be at about C4 speed [estimate].
- **The big cycle drop on built-in is fewer nodes, not faster sorting.** Sorting the negative
  tail changes move order in `MAIN_MOVES`, so LMR, move-count and policy pruning cut a
  different tree. On yixin-net the same change **adds** 11% nodes.
- Whether that is a strength gain or a loss is unknown [unmeasured]. It needs the todo's SPRT
  self-play, a new bench baseline in `CLAUDE.md` and `docs/process/change-process.md`, and the
  user's decision. It is a search-ordering experiment, not a performance fix, and is out of
  scope for a bit-identical PERF-02.

### Q7. PolicyComparator path [code + measured]

- **[code]** It does go through this branch. The `ROOT` ctor (`movepick.cpp:209`) and
  `scoreAndSortMoves` (`:497`) call `fastPartialSort(..., 0, PolicyComparator{})` when
  `useNormalizedPolicy` is set, which is the MCTS root and main path.
  - Membership in the prefix uses `score >= 0`, but the sort uses `policy`. The Q1 model holds
    unchanged with that comparator.
- **[measured]** BENCH (alpha-beta) produced **0** PolicyComparator lists in both configs. The
  path's frequency under MCTS is [unmeasured].
- **[measured]** The Q1 model under `PolicyComparator` (stable sort of the prefix) had 0
  mismatches on 1.12M random lists with policy ties and +-0.
- **Recommendation:** keep it on the generic loop. The float key has no order-equivalent integer
  packing (+0/-0 compare equal, NaN), as PERF-01 found. Nothing in BENCH would measure a change
  there.

## Candidate table (summary)

| candidate | bit-identical | micro cyc/list vs C0 (clang / gcc) | micro br-miss/list vs C0 | engine cycles:u delta, built-in (clang / gcc) | engine delta, yixin-net (clang / gcc) | bench |
|---|---|---|---|---|---|---|
| C0 reference | - | 802-871 / 690-741 | 11.7 / 12.4-14.6 | - | - | 17621937 / 8c1f72e5 |
| **C4 partition + AVX-512 rank** | **yes** (6.35M real + 1.12M random) | **-58% / -45%** | -69% / -89% | **-3.4% / -2.2%**, disjoint, spread ~1% | -0.3% / -1.0%, overlapping, spread 1-5% | 17621937 / 8c1f72e5 |
| C10 n <= 8 ref, else C4 | yes | -57% / -44% | -63% / -83% | [unmeasured] | [unmeasured] | same (by construction) |
| C8 transposed rank | yes | -60% / -32% | -63% / -60% | [unmeasured] | [unmeasured] | same |
| C3 scalar rank | yes | -47% / -9% | -66% / -88% | [unmeasured] | [unmeasured] | same |
| C2/C7 key insertion | yes | -16..-18% / +17..+26% | +10..+12% / +13..+36% | rejected | - | same |
| C5/C6 radix prefix | yes | -2..-50% / +60..-25% | - | rejected | - | same |
| O2 full sort (rankT kernel) | **no** | -48% / -45% | -75% / -76% | not comparable (tree differs) | - | **11127201 / 997a1b04**; yixin 5097934 / fcb63f2b |

## Risks

- **ISA coverage.**
  - Only the AVX-512 path was built and measured. AVX2, SSE, NEON, WASM and MSVC builds would
    keep the reference loop, so they are bit-identical by construction but get no gain.
  - An AVX2 rank sort (4 x u64 per compare) is plausible, but its speed is [unmeasured].
  - Tests must run on an AVX-512 build to exercise the new code. `rapfi_tests` inherits the
    `rapfi` compile definitions (`CMakeLists.txt:708`), so `USE_AVX512` reaches it. A non-AVX-512
    test build would silently test only the fallback.
- **Stability.** The new code must break ties by **arrival index**, not by union bits, and not
  rely on "distinct `pos`". Real lists have duplicate `pos` with differing union bits, and an
  unstable variant fails on 1,027 real lists.
- **`begin[0]` quirk.** The prefix always includes `begin[0]`. A "cleaner" partition that tests
  `begin[0]` too would change the tree. The todo's planned case
  `fastPartialSort_insertionBranch_nonNegativePrefixSortedByScoreThenPos` is **wrong as written**
  whenever `begin[0] < 0`.
- **The `limit` parameter.** Every call site passes 0. The prototype takes `limit` and compares
  `score >= limit`, the same as the loop. The engine-port check covered 7 non-zero limits on
  random lists, with 0 mismatches.
- **Stack.** `pre[129]` + `key[129]` is about 2 KB in a `NO_INLINE` leaf, the same pattern as
  PERF-01. `assert(n <= MAX_MOVES/4)`.
- **Size of the gain.** The win is 2-3.5% on built-in and not resolved on yixin-net. It is real
  but modest compared with PERF-01 (+17%).
- **PGO.** Profiles go stale. PGO vs PGO was not measured [unmeasured].
- **Open items for the implementation step.** Run 10+ interleaved yixin-net runs to resolve the
  gain there. Run PGO vs PGO. Measure an AVX2 variant if non-AVX-512 targets matter.
- **Measurement conditions.** PL1 was 24 W (throttled). Relative deltas come from interleaved
  runs, but absolute Nodes/s is not comparable with the PERF-01 tables (44 W).

## Recommended option and what the decision note and tests should cover

**Option 1, candidate C4.**

- Add `NO_INLINE void insertionBranchByScore(ScoredMove*, size_t n, Score limit)` in
  `search/movepick.cpp`, declared in `search/movesort.h`. Use `static_assert`s on the layout,
  `assert(n <= InsertionSortLimit)` and an early return for n < 2.
- In `fastPartialSort`'s insertion branch, call it under `#ifdef USE_AVX512` and
  `if constexpr (std::is_same_v<Comparator, ScoredMove::ScoreComparator>)`.
- Everything else stays on today's loop. Use the engine's `popcount` (`core/platform.h`), which
  compiles to `popcnt` under the AVX-512 flags on both compilers [measured].
- The middle branch, the `partial_sort` branch and `PolicyComparator` are untouched.

**Decision note.**

- **What:** bit-identical replacement of the `ScoreComparator` insertion branch on AVX-512
  builds.
- **Expected:** `cycles:u` -2..-3.5% on built-in, -0..-1% on yixin-net, branch misses -6..-14%,
  bench unchanged.
- **How measured:** as in Q5, plus PGO vs PGO and, for yixin-net, 10 interleaved runs.

**Test list.** Extend `tests/movesort_tests.cpp` and reuse the frozen copy.

- `fastPartialSort_insertionBranch_matchesFrozenReference`. Sizes n in
  {0, 1, 2, 3, 8, 9, 15, 16, 17, 64, 120, 121}, which covers the 8-key padding edges. Score
  schemes: narrow with ties, full `int16`, extremes only, all negative, none negative.
- `fastPartialSort_insertionBranchDuplicatePos_stableByArrival`. Duplicate `pos` with equal
  score and different union bits. This pins stability.
- `fastPartialSort_insertionBranchNegativeFirst_firstMoveKeptInPrefix`. `begin[0] < 0` must end
  as the last prefix element. This replaces the wrong-as-written `...nonNegativePrefixSorted...`
  case.
- `fastPartialSort_insertionBranchPrefix_isBeginZeroPlusNonNegativesStablySorted`. The corrected
  prefix property.
- `fastPartialSort_insertionBranchTail_isFifoQueueOrder`. The tail equals the queue model from
  Q1. Use a small hand-written example plus the frozen reference.
- `fastPartialSort_insertionBranchUnionBits_preserved`.
- Keep `fastPartialSort_policyComparatorAcrossSizeThresholds_matchFrozenReference` as the guard
  that Policy stays generic.
- Keep the `MovePicker` goldens unchanged.
- Bench gate: `17621937` / `8c1f72e5` on gcc `build/`, plus clang and yixin-net
  (`4587838` / `4447fd06`).

Option 2, if the user wants it, is a separate search-behaviour item. It needs SPRT, the new
bench value `11127201` / `997a1b04`, and baseline updates.

Knowledge lookup (CLAUDE.md hard rule 10):
`SKILLS_TREE/tools/find -k knowledge "branchless SIMD small array sorting partition stable rank sort AVX-512 performance"`
returned no relevant note (top hits were PostgreSQL partitioning and NLP data splits). The
recommendation rests on the measurements above.

## Reproduction

```bash
S=/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/c64209c3-a546-4931-bd6a-1f3b74270344/scratchpad/perf02
# dump lists (instrumented copy)
echo BENCH | PERF02_DUMP=$S/inst/builtin.bin $S/inst/b/pbrain-rapfi
(cd <repo>/Rapfi/build/yixin-net && echo BENCH | PERF02_DUMP=$S/inst/yixin.bin $S/inst/b/pbrain-rapfi --config config.toml)
# micro-benchmark / verification (engine flags)
cd $S/mb && clang++ -O3 -std=gnu++17 -ffp-contract=off -mavx512f -mavx512dq -mavx512bw -mavx512vnni -mavx512vl -mbmi2 -o mbe_clang mb.cpp
./mbe_clang random; ./mbe_clang verify ../inst/builtin.bin; ./mbe_clang stats ../inst/builtin.bin
./mbe_clang bench ../inst/builtin.bin c4_part_rank512 2      # NLO=/NHI=/SITE= env filters
# engine runs (interleaved, perf stat)
$S/runs/run.sh clang_builtin builtin 5 $S/b-base-clang/pbrain-rapfi $S/proto/b-c4-clang/pbrain-rapfi
$S/runs/summ.sh $S/runs/clang_builtin.csv
# annotate
echo BENCH | perf record -e branch-misses:upp -c 20000 -o bmp.data $S/b-base-clang-g/pbrain-rapfi
perf annotate -i bmp.data --stdio --no-source 'void Search::fastPartialSort<ScoredMove::ScoreComparator>(ScoredMove*, ScoredMove*, short, ScoredMove::ScoreComparator)'
```

## Appendix: scratch artefacts (session-local, may be gone)

Scratch root:
`/tmp/claude-1000/-run-media-ngmint-Data-Programming-Programming-C---Project-Lab-Rapfi-V2-rapfi/c64209c3-a546-4931-bd6a-1f3b74270344/scratchpad/perf02`.
All paths below are relative to it.

- `wt/`: a git worktree of HEAD `a5eb8bc` (detached), registered in the repo's
  `.git/worktrees`. Remove it with `git worktree remove --force <scratch root>/wt`, or with
  `git worktree prune` once `/tmp` has been cleared.
- Baseline builds: `b-base-clang/`, `b-base-gcc/`, `b-base-clang-g/` (the `-g` copy for
  annotate).
- Instrumented copy: `inst/src/` (dump hook in `search/movesort.h` and `search/movepick.cpp`),
  build `inst/b/`. Dumps: `inst/builtin.bin` (647 MB) and `inst/yixin.bin` (127 MB).
- Micro-benchmark: `mb/sorts.h` (frozen reference, model, all candidates) and `mb/mb.cpp`.
  Outputs: `mb/mb_builtin.txt`, `mb/mb_buckets.txt`, `mb/mb_agg.txt`.
- Prototypes: `proto/c4/` (bit-identical, builds `proto/b-c4-{clang,gcc}`) and `proto/o2/`
  (full sort, builds `proto/b-o2-{clang,gcc}`).
- Engine run logs: `runs/{clang_builtin,gcc_builtin,clang_yixin,gcc_yixin}.csv`, plus
  `runs/run.sh` and `runs/summ.sh`.
- Profiles: `annot/{bm,bmp,cyc,cycp}.data` and `annot/ann_*.txt`.

Prototype C4 core (`proto/c4/search/movepick.cpp`, scratch quality):

```cpp
NO_INLINE void insertionBranchByScore(ScoredMove *a, size_t n, Score limit)
{
    constexpr size_t Cap = MAX_MOVES / 4 + 8;
    if (n < 2) return;
    alignas(64) ScoredMove pre[Cap];
    alignas(64) uint64_t   key[Cap];
    pre[0] = a[0];
    size_t j = 0;
    for (size_t p = 1; p < n; p++) {               // branchless Lomuto, begin[0] pinned
        uint64_t x, y;
        std::memcpy(&x, &a[p], 8);
        std::memcpy(&y, &a[j + 1], 8);
        const uint64_t f = uint64_t(a[p].score >= limit), mask = 0 - f;
        const uint64_t v = (y & mask) | (x & ~mask);
        std::memcpy(&a[p], &v, 8);
        std::memcpy(&pre[j + 1], &x, 8);
        j += f;
    }
    const size_t m = j + 1;
    for (size_t i = 0; i < m; i++) {                // stable key: (score desc, pos asc, arrival)
        const uint32_t k32 = (uint32_t(uint16_t(32767 - int(pre[i].score))) << 16)
                             | uint32_t(uint16_t(int16_t(pre[i].pos)) ^ 0x8000u);
        key[i] = (uint64_t(k32) << 32) | i;
    }
    const size_t m8 = (m + 7) & ~size_t(7);
    for (size_t i = m; i < m8; i++) key[i] = ~uint64_t(0);
    for (size_t i = 0; i < m; i++) {                // rank sort, 8 keys per compare
        const __m512i ki = _mm512_set1_epi64((long long)key[i]);
        int r = 0;
        for (size_t t = 0; t < m8; t += 8)
            r += popcount(_mm512_cmplt_epu64_mask(_mm512_load_si512(key + t), ki));
        a[r] = pre[i];
    }
}
```
