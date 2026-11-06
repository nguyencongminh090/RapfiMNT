# SEARCH-01 — Explicit handling of nodes facing a flex-four threat

**Status:** done (2026-11-06). Bench bit-identical; see Verification. Research:
[`../audit/2026-11-06-oppo4-threat-node-research.md`](../audit/2026-11-06-oppo4-threat-node-research.md).

**Class:** B (pruning code). The bench must not move.

## Goal

In `search()`, a node with `oppo4 && !oppo5` never assigns `eval`, so `eval` stays `VALUE_NONE`
(-30002). Steps 7 and 8 and `likelyFailHigh` then do arithmetic on that sentinel, and the resulting
behaviour looks accidental. Measurements show the behaviour is sound and that the "obvious"
`eval = ss->staticEval` is not stronger than base. So state the behaviour explicitly, keep it
bit-identical, and stop the code from inviting that one-line fix.

## Decision note (what / expected / how measured)

Decided 2026-11-06 (user approved candidate R0 after the research report).

- **What:**
  1. New `razorThreatNode<R>(depth, alpha)` in `search/ab/parameter.h`, equal to
     `VALUE_NONE + razorMargin<R>(depth) < alpha`. Step 7 uses it when `oppo4` is set.
  2. `!oppo4` guard on Step 8 (futility).
  3. `!oppo4` guard on `likelyFailHigh`.
  4. A comment at the `oppo4` branch says why `eval` stays unset.
- **Why bit-identical holds, all rules:**
  - Step 7: same expression, with `eval == VALUE_NONE` substituted.
  - Step 8: `futilityMargin >= 0` and `beta >= -VALUE_MATE`, so `VALUE_NONE - margin >= beta` is
    never true.
  - `likelyFailHigh`: `failHighMargin(d, true) > 0` for `d > 0` in every rule and
    `beta >= -VALUE_MATE`, so `VALUE_NONE >= beta + margin` is never true.
- **Expected:** bench unchanged, built-in `17621937` / `8c1f72e5`, yixin-net `4587838` / `4447fd06`.
  No Elo change, so no A/B. No run-time cost expected.
- **How measured:** the bench gate below, on the exact tree, on gcc, clang and clang PGO; `perf stat`
  against base for speed. Function tests pin the rule.
- **Not done:**
  - Removing the now constant `oppo4` argument of `failHighMargin` and the `FailHighOppo4Bonus`
    table. It is dead code but unrelated to making this behaviour explicit.
  - Fixing the parent eval stored in TT at these nodes and read by `vcfsearch` (0.97% of vcf nodes
    read it, 0.24% change a verdict). Candidate H does this, moves the tree, and has no predicted
    Elo sign, so it does not qualify for an A/B yet.
  - Enabling futility at these nodes (candidate J). Rejected: no margin is safe, see research.

## Function -> test-case list (written before code)

File `tests/razor_tests.cpp`, added to `TEST_DRIVER_SOURCES`. Boundaries are worked out by hand from
the current tuned tables, so re-tuning `Razor*` must update them on purpose.

| Unit | Case name | Expected |
|---|---|---|
| `razorThreatNode` | `razorThreatNode_standardBelowRazorDepth_dropsForEveryReachableAlpha` | Standard margin is 0 below 2.6, so `true` for d in {0.5, 1, 2, 2.59} and alpha in {-30001, -29500, -6000, 0, 6000, 29500, 30001} |
| same | `razorThreatNode_standardBelowRazorDepth_alphaEqualToSentinel_doesNotDrop` | strict compare: alpha = -30002 gives `false`, -30001 gives `true` |
| same | `razorThreatNode_atOrAboveRazorDepth_noDropUpToThreshold` | all rules, d in {2.6, 3, 8, 30}: `false` at alpha = 2765 and for alpha in {-30001, -6000, 0} |
| same | `razorThreatNode_atOrAboveRazorDepth_dropsAboveThreshold` | all rules, same depths: `true` at alpha = 2766 and at 29500 |
| same | `razorThreatNode_freestyleShallow_noDropAtVeryShortMatedAlpha` | d = 1: margin 26, `false` at -29976, `true` at -29975; d = 2.5: margin 49, `false` at -29953, `true` at -29952 |
| same | `razorThreatNode_renjuShallow_noDropAtVeryShortMatedAlpha` | d = 1: margin 17, `false` at -29985, `true` at -29984; d = 2.5: margin 30, `false` at -29972, `true` at -29971 |
| same | `razorThreatNode_allRules_matchesBaseExpressionWithSentinelEval` | sweep d in [0, 30] step 0.1, every alpha in [-30002, 30001]: equals the frozen base expression `eval + razorMargin<R>(d) < alpha` with `eval = VALUE_NONE` |
| `search()` Steps 7-8, `likelyFailHigh` | bench | no unit test reaches these branches in isolation, so the gate is `17621937` / `8c1f72e5` and yixin-net `4587838` / `4447fd06` |

## Verification

All on gcc `build/`, Release, AVX-512+VNNI, 2026-11-06.

- Red step: `razor_tests.cpp` failed to compile (`'razorThreatNode' has not been declared in
  'Search::AB'`), as intended.
- Green step: `rapfi_tests` 86/86 pass, 7 of them new. `./rapfi_tests --check-asserts-live` aborts
  on its sentinel (exit 134), so asserts are live.
- Mutation check: changing `<` to `<=` in `razorThreatNode` failed
  `razorThreatNode_standardBelowRazorDepth_alphaEqualToSentinel_doesNotDrop`. Reverted, then the
  engine and tests were rebuilt and tests passed 86/86 again.
- Bench gate on that rebuilt binary (Nodes / Hash), identical to the baseline:

  | build | built-in | yixin-net |
  |---|---|---|
  | gcc Release AVX512+VNNI (`build/`) | 17621937 / 8c1f72e5 | 4587838 / 4447fd06 |

- Clang: `x64-clang-AVX512VNNI` (clang 21.1.8), `rapfi_tests` built with the preset's options in a
  scratch directory: 86/86 pass, sentinel aborts (exit 134).
- Bench gate on other toolchains: `17621937` / `8c1f72e5` and `4587838` / `4447fd06` on all 5 runs
  of each config, for gcc, clang `x64-clang-AVX512VNNI` and its PGO build
  (`scripts/pgo_build.sh`, 1 BENCH training run). The binaries differ from base, the search does not.
- Performance: `perf stat -e cycles:u,instructions:u,branch-misses:u`, pinned to one CPU, 5
  interleaved rounds, with the order alternating each round. The baseline is HEAD `1ed1c28`, built
  with the same flags from a scratch export; PGO builds use their own profile. i7-1165G7 at RAPL PL1
  24 W, so absolute Nodes/s is not comparable with other tables. Median [min-max] of `cycles:u`:

  | build / config | base | new | cycles:u | instructions:u | branch-misses:u | Nodes/s |
  |---|---|---|---|---|---|---|
  | gcc / built-in | 69.27G [68.58-69.40] | 68.91G [68.27-69.52] | -0.51%, overlap | +0.05% | -0.11% | -0.15% |
  | gcc / yixin-net | 52.18G [51.40-52.70] | 51.97G [51.64-52.40] | -0.40%, overlap | +0.02% | +0.97% | +0.03% |
  | clang / built-in | 63.40G [63.02-63.54] | 63.15G [62.95-63.47] | -0.40%, overlap | -0.23% | -0.09% | +0.07% |
  | clang / yixin-net | 51.31G [51.17-51.45] | 50.93G [50.86-51.45] | -0.74%, overlap | -0.09% | -0.56% | +0.75% |
  | clang PGO / built-in | 60.63G [60.32-60.95] | 60.94G [60.54-61.97] | +0.52%, overlap | -0.05% | -1.19% | -0.39% |
  | clang PGO / yixin-net | 50.58G [50.45-51.13] | 50.79G [50.55-51.04] | +0.42%, overlap | -0.08% | +2.50% | +0.04% |

  All `cycles:u` ranges overlap, so no speed difference was resolved at 5 runs. Instruction counts
  are stable between runs and move by at most 0.23%. Differences of about 0.5% could not be excluded.
  The gcc build here has command modules on, unlike `build/`, and BMI2 on, unlike the clang preset.
- The `!oppo4` guards have no unit test of their own, because they are no-ops by construction: only
  the bench and the algebra above cover them.
