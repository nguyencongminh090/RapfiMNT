# Test note: why does N/MS (NPS) fall during a long search?

Date: 2026-11-04. Type: measurement note (no code changed, no bench impact).

## Question

In a Yixin Board log (8 threads, MIX9SVQ standard, position `n3n2k2`), `n/ms` was ~2200 at
depth 8, ~1400 at depth 13-15, ~1100-1180 at depth 20-27 and then fell steadily to ~785 at
depth 33 (91M nodes, 117 s). Which factor causes the drop?

Hypotheses before measuring (from the log alone): (1) cache/TT pressure as the tree grows,
(2) CPU thermal/power throttling under sustained AVX512 load, (3) search-shape change
(longer threat lines), (4) multithread contention, (5) noisy `n/ms` at tiny `tm`.

## Setup

- Machine: Intel i7-1165G7 (4 cores / 8 threads, laptop, 28 W class), L2 5 MiB, L3 12 MiB,
  14 GB RAM, Linux. Max turbo 4.7 GHz.
- Engine: `Rapfi/build/yixin-net/pbrain-rapfi` (0.44.1, clang++ AVX512 VNNI), weights
  `mix9svqstandard_bs15`. Config copied with `coord_conversion_mode = "none"` (the GUI config
  uses `flipY_X`); everything else unchanged. Default `default_tt_size_kb` is 32768 (32 MB).
- Rule: Standard (`INFO rule 1`), board 15.
- Position `n3n2k2` = N3, N2, K2 (black, white, black; white to move). Letter = column
  (a=0), number = row. In 0-based piskvork coordinates with `y = number - 1`:
  N3 = `13,2`, N2 = `13,1`, K2 = `10,1`. (Using `y = 15 - number` instead is a mirror image
  of the board, an equivalent position.) The first PV lines (`K5 L4 K6`, evals -44, -31,
  -64) are identical to the GUI log, so the position matches.
  Sent as `BOARD` with engine stones = 1: `13,2,2` / `13,1,1` / `10,1,2` / `DONE`.
- Each run: `START 15`, `INFO rule 1`, `INFO thread_num T`, `INFO hash_size KB`,
  `INFO timeout_match 0`, `INFO timeout_turn 100000`, then the board. 100 s per run, 30 s
  cool-down between runs, runs strictly sequential. During each run the average CPU MHz
  (`/proc/cpuinfo`, all cores) and the hottest thermal zone were sampled every second.

Matrix: threads {1, 8} x hash {32 MB, 1 GB}.

## Results (n/ms; node count and elapsed time in brackets)

| depth | T1 / 32MB | T1 / 1GB | T8 / 32MB | T8 / 1GB |
|---|---|---|---|---|
| 13 | 254 (15K, 0.06s) | 254 | 1900 (39K, 0.02s) | 1542 (61K, 0.04s) |
| 18 | 227 (138K, 0.6s) | 233 | 1348 (250K, 0.19s) | 1258 (354K, 0.28s) |
| 22 | 229 (456K, 2.0s) | 237 | 1112 (1.8M, 1.6s) | 1175 (3.0M, 2.5s) |
| 26 | 240 (2.1M, 8.9s) | 247 | 1113 (10M, 9.6s) | 1097 (13M, 11.9s) |
| 28 | 239 (4.3M, 17.7s) | 247 | 869 (21M, 25s) | 884 (23M, 26s) |
| 30 | 236 (7.8M, 33s) | 249 | 755 (45M, 61s) | 798 (37M, 47s) |
| 32/33 | 233 (22M, 98s) | 253 (23M, 92s) | 720 (67M, 94s) | 759 (74M, 97s) |

Frequency / temperature during the 8-thread runs (both hash sizes the same):

- 0-15 s: average ~3300 -> 3000 MHz, hottest zone ~98 C (also hot at the start, left over
  from the previous run).
- from ~20 s to the end: average locked at ~2000-2100 MHz, hottest zone ~72-83 C.

## Findings

1. **Not cache / TT pressure.** The single-thread run stays flat at 227-253 n/ms while the
   tree grows from 15K to 23M nodes, even with the 32 MB hash (which fills early). A 1 GB
   hash changes nothing measurable for 8 threads (759 vs 720 at the end).
2. **The drop is driven by CPU frequency, not by the search.** For 8 threads the slow-down
   happens at ~10-25 s of wall time (1113 -> 869 n/ms) in both hash settings, exactly when
   the average clock falls from ~3000 MHz to ~2000 MHz. The ratio matches: 2000/3000 = 0.67
   and 750/1110 = 0.67. This is the laptop power/thermal limit (turbo budget exhausted,
   then a sustained limit), made worse by 8 threads running AVX512 at once. It is a
   property of this machine and run length, not an engine defect.
3. **Multithreading scales 4.8x on 4 cores / 8 threads** (~1100 vs ~230 n/ms before the
   throttle). That is the normal Hyper-Threading result and not a contention problem.
4. **Early high values are not comparable.** `n/ms` at `tm` of 1-20 ms is dominated by
   rounding and the initial turbo burst (8-thread 1900-2200 at depth 8-13), so the "drop"
   from ~2200 to ~1100 over the first second is mostly an artifact. The single-thread run
   shows per-thread speed does not fall there (233 -> 254). I did not isolate the remaining
   part (thread start-up, SMP overlap while the tree is small); it is small next to
   finding 2.

## Not tested / caveats

- Frequency was the all-core average from `/proc/cpuinfo` and the hottest thermal zone, not
  per-core `turbostat` data (not installed). The single-thread MHz trace is noisy because
  idle cores are in the average. The attribution to throttling rests on matching timing and
  the matching ratio; no run with a forced fixed clock was done.
- The runs started warm (thermal zone ~93-98 C at second 1), so the turbo phase was shorter
  than from a cold start. The first-phase length would be longer on a cool machine.
- One run per cell; run-to-run variance not measured. Positions other than `n3n2k2` not
  tried.

## Suggested follow-up (not done)

- Repeat the 8-thread run with the frequency pinned (`cpupower frequency-set`, or a
  performance cap) or with 4 threads, to confirm the NPS stays flat once the clock is fixed.
- For engine benchmarking on this machine, compare NPS only over the same wall-clock window
  after warm-up, or use `BENCH` (fixed node count) for correctness and a cool, pinned-clock
  run for speed.

## Addendum: re-run with `throttled` (PL1/PL2 raised to 44 W)

Date: 2026-11-04. Before this run, `throttled` was started with the existing
`/etc/throttled.conf` ([AC]: PL1 = PL2 = 44 W). `intel-rapl:0` then reported
`constraint_0` (long_term) = `constraint_1` (short_term) = 44 W; it was 24 W / 51 W before.
The service itself showed `inactive` afterwards, but the limits were applied.

Evidence: one Yixin Board GUI run (8 threads, same position, default 32 MB hash, config of
the GUI), supplied by the user. No frequency/temperature trace was recorded for this run.

| elapsed | nodes | n/ms before (GUI, throttled off) | n/ms after (GUI, 44 W) |
|---|---|---|---|
| ~12 s | 13M | ~1110 (own run, 9.6 s) | 1136 |
| ~28-37 s | 30-39M | ~870 at 25 s | 1067-1073 |
| ~62 s | 65M | ~755 at 61 s | 1045 |
| ~86-87 s | 90M | 785 at 117 s (91M) | 1037 |
| ~138-166 s | 141-170M | not reached | 1023-1025 |

Reading: the sharp fall (-33% in the first ~25 s) is gone; NPS now declines about 10% over
166 s (1136 -> 1023), against -33% before. This supports the power-limit (PL1 = 24 W)
explanation from the main findings. The remaining slow decline was not isolated (likely
sustained heat or search shape; no trace to tell).

Caveats: single run, a different frontend than the controlled matrix, no frequency or
temperature samples, and the "before" figures come from two different runs (the GUI log
and the controlled `t8_h32M` run). A controlled before/after (same script, `throttled`
off then on, with the MHz/temperature sampling) was not done.

## Addendum: is cache miss a bottleneck? (`perf stat`, single-thread BENCH)

Date: 2026-11-04. `perf stat` on `pbrain-rapfi --config <yixin-net config> bench` (clang AVX512
VNNI build, 16 MB TT, 1 search thread, 12 s). Note: this uses the yixin-net config and weights,
so `Nodes 4587838 / Hash 4447fd06` differ from the built-in-config baseline in `CLAUDE.md`
(17621937 / 8c1f72e5); that is expected and not a regression.

| counter | value | derived |
|---|---|---|
| cycles / instructions | 52.6G / 112.4G | IPC 2.14 (~4.15 GHz) |
| L1D loads / misses | 34.8G / 5.42G | 15.6% miss, ~48 MPKI |
| LLC loads / misses | 59.0M / 1.62M | 2.7% miss, 0.014 MPKI |
| cache-references / misses | 473M / 46M | 9.8% |
| branches / misses | 7.67G / 248M | 3.2% miss, 2.2 MPKI |

Reading: demand misses reaching DRAM are negligible (1.6M x ~200 cycles ~ 0.3G of 52.6G cycles,
under ~1%). L1 misses are frequent but almost all hit L2 (only 59M go to L3). Branch
mispredicts cost roughly 247M x ~15-20 cycles ~ 4-5G cycles, ~8-9% of the run (rough
estimate). IPC 2.14 is already high. This agrees with the thread/hash matrix: single-thread
NPS stayed flat as the tree grew.

Caveats: `LLC-load-misses` counts demand loads only (prefetched TT/NNUE lines are not
counted); the 16 MB bench TT partly fits in the 12 MB L3, so a large TT in a real game could
miss more (the matrix showed 1 GB hash +~8% n/ms vs 32 MB at 1 thread, one run each, not
analysed further); multithread cache sharing not measured; cycle costs above are estimates.

## Addendum: where do branch misses come from? (`perf record`, single-thread BENCH)

Date: 2026-11-04. Same setup as the previous addendum (yixin-net config, so
`default_candidate_range = "full_board"`, ~200+ moves per node). `perf record -e
branch-misses:u -c 20000` and `-e cycles:u -F 2000`, 1 search thread.

| symbol | share of branch-misses | share of cycles |
|---|---|---|
| `std::__introsort_loop<ScoredMove*,...ScoreComparator>` | 27.3% | 4.5% |
| `std::__final_insertion_sort<ScoredMove*,...ScoreComparator>` | 14.2% | 2.6% |
| `Board::move<Rule 0/1/2>` (3 instantiations) | 17.7% | 14.9% |
| `Accumulator::evaluatePolicy` | 5.9% | 5.6% |
| `Accumulator::move` | 4.4% | 38.4% |
| `MovePicker::scoreAndSortMoves` | 4.3% | - |
| `search<...>` (3 rules) | ~7.0% | ~1.7% (one) |

Reading: the move-list sort (`fastPartialSort` -> `std::sort` for 121 < n <= 323 moves,
`movepick.cpp:95`) is ~41.5% of all branch misses but only ~7.1% of cycles. Branch misses cost
~8-9% of cycles overall (previous addendum), so removing the sort's share bounds the gain at
roughly 3-4% NPS (estimate, not measured). The dominant cycle cost, `Accumulator::move`
(38%), is compute, not branch-bound.

Observation for a TODO: both comparators are a strict total order (tie-break by `pos`,
`game/movegen.h`), so any correct sort/selection method yields the same order. A lazy
selection (select the next-best move on demand, since most nodes cut off after the first few
moves) should keep the move order, hence search and bench hash, identical. Not verified.

Caveats: bench uses the yixin-net config; the built-in default candidate range gives fewer
moves per node, so the sort share may be smaller there (not measured). Percentages are
sampled (`-c 20000`). No code changed.
