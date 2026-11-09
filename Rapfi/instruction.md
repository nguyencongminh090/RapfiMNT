# Instructions

Index of execution guidance (approach, pitfalls, boundaries) for `TODO.md` items. Each line links
to a detail file under `docs/instruction/`. Read the matching entry here before starting any
`TODO.md` task — see [CLAUDE.md](CLAUDE.md). A missing entry is fine; not every task has one.

## Active

- [SEARCH-12 — MCTS root with a five or flex four aborts in assert builds](docs/instruction/SEARCH-12-mcts-forced-root-policy-assert.md) — set `hasPolicy` in the ROOT picker's one-hot branches; tests first

## Done

- [VCN-03 — VCN search mode under the MCTS searcher](docs/instruction/VCN-03-mcts-vcn.md) — slices 2–5 done 2026-11-09; slice 5: guard removed, PASS in PV like AB, Part C zero wrong answers (AB 27/27 proven, MCTS 23/27), default pass prior kept
- [SEARCH-10 — MCTS: add the cut moves back when every kept reply is lost](docs/instruction/SEARCH-10-mcts-readd-cut-moves.md) — done 2026-11-09 (see todo results)
- [SEARCH-09 — MCTS mate bound unsound under the policy-mass edge cut](docs/instruction/SEARCH-09-mcts-bound-under-edge-cut.md) — done 2026-11-09 (see todo results)
- [SEARCH-08 — MCTS 64-bit visit counters without growing `Edge`](docs/instruction/SEARCH-08-mcts-64bit-visits.md) — done 2026-11-08 (see todo results)
- [PERF-02 — Branch misses in the short-list (insertion) branch of `fastPartialSort`](docs/instruction/PERF-02-insertion-branch-sort.md)
- [PERF-01 — Branch-miss reduction in move-list ordering](docs/instruction/PERF-01-movepick-branch-miss.md)
- [BUILD-01 — Profile-guided optimization (PGO) build](docs/instruction/BUILD-01-pgo-build.md)
- [VCN-02 — VCN tree search: attack-oriented search quality and real-world correctness](docs/instruction/VCN-02-attack-search-quality.md)
- [VCN — Victory by Continuous N-level attack search mode (VC2..VC4; VC5 internal)](docs/instruction/VCN-search-mode.md)
- [EVAL-01 — VNNI signed-input affine via +128 input-bias trick](docs/instruction/EVAL-01-vnni-i8i8-input-bias.md)
- [YXANALZ — user-supplied root candidates](docs/instruction/YXANALZ-user-root-candidates.md)
