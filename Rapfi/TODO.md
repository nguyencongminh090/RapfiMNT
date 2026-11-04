# TODO

Index of tracked work. Each line links to a detail file under `docs/todo/`. Pair every entry with
a matching `instruction.md` line when execution guidance exists. See
[CLAUDE.md](CLAUDE.md) for the process this file is part of.

## Active

(none)

## Done

- [PERF-01 — Branch-miss reduction in move-list ordering](docs/todo/PERF-01-movepick-branch-miss.md) — LSD radix sort replaces `std::sort` in the middle branch of `fastPartialSort` (121 < n <= 323, `ScoreComparator` only); tests in `tests/movesort_tests.cpp`. Class B. Done 2026-11-04 — bench bit-identical (17621937 / 8c1f72e5; yixin-net 4587838 / 4447fd06), median Nodes/s +17.3% gcc / +16.6% clang built-in, +5.9% yixin-net, +18.4% clang PGO vs PGO, branch misses -43..-59%.
- [BUILD-01 — Profile-guided optimization (PGO) build](docs/todo/BUILD-01-pgo-build.md) — `PGO_MODE`/`PGO_PROFILE` CMake options + `scripts/pgo_build.sh <preset>` (gen -> BENCH -> merge -> use, LTO on). Class C. Done 2026-11-03 — bench bit-identical (17621937 / 8c1f72e5), median NPS +3.6% (975150 -> 1009968) on clang-AVX512VNNI.
- [VCN-02 — VCN tree search: attack-oriented search quality and real-world correctness](docs/todo/VCN-02-attack-search-quality.md) — VC4 full-board fours (item 1), no double-three win (1b), pruning kept (4), threat ordering -32% nodes (6) done. Items 5 (horizon value) and 7 not planned (need SPSA); acceptance `h8j9f6i10` tracked by move G5, eval > 600 unmet. Class B. Closed 2026-11-03 with known limitations (see the doc).
- [VCN — Victory by Continuous N-level attack search mode (VC2..VC4)](docs/todo/VCN-search-mode.md) — attacker must win while the defender may pass; new `INFO VCTHREAD 0..3` (Yixin GUI option) and one-shot `YXVCN <2..4>`; VC5 is internal only. Class B + C. Done 2026-10-06 — bench bit-identical (16733809 / 4621a98a), unit tests passing (see the doc).
- [EVAL-01 — VNNI signed-input affine via +128 input-bias trick](docs/todo/EVAL-01-vnni-i8i8-input-bias.md) — feed `input ^ 0x80` to one `vpdpbusd` for mix9 `starBlock`'s `value_corner_down` and fold `-128·Σrow(w)` into the bias at load time (VNNI builds only). Class B, opt-in, mix9-only. Done 2026-09-09 — bench bit-identical (16733809 / 4621a98a), 26/26 unit tests (12 new); sign-split `vpand/vpandn/vpsubd/vpaddd` + 2nd `vpdpbusd` removed from `value_corner_down`.
- [YXANALZ — user-supplied root candidates](docs/todo/YXANALZ-user-root-candidates.md) — new `YXANALZ x,y ... DONE` command: analyze an explicit user allow-list of depth-0 root moves instead of engine-generated candidates. Class C. Done 2026-09-09 — bench bit-identical (16733809 / 4621a98a), 14/14 unit tests.
