# SEARCH-08 — MCTS 64-bit visit counters without growing `Edge`

**Status:** done 2026-11-08 (phase 1, steps A-C; phase 2 not done, optional). Commits `233893b` (tests),
`7b34ee6` (A), `30252b0` (B), `d82d577` (C). Results in "Results" below.
**Class:** B (search data structure; the packed `Edge` word needs the derived-state audit below).

## Problem

MCTS visit counters are `uint32_t`: `Node::n` / `nVirtual` and `Edge::edgeVisits`
(`search/mcts/node.h`). In a long analysis (8 threads, about 5.9 M nodes/s) the root crosses 2^32
visits after roughly 12 minutes and wraps.

Evidence, `test_mcts.txt` (one analysis, uncommitted log), line 7320 -> 7330, same PV `I7 I9 J8 ...`:

| | line 7320 | line 7330 |
|---|---|---|
| `EV` | 88 | `-M-2147453648` (invalid mate score) |
| `W` / `D` | 68.14 / 3.25 | 149873 / 26876 (above 100%) |
| `V` (root visits) | 4291M | 521K |
| `N` (total nodes, 64-bit) | 4291M | 4296M |

`Node::updateStats` sums `uint32_t` edge visits into `nSum`; after the wrap the weights are garbage,
so root Q/D and the printed score are garbage until the end of the run (about 3300 more output lines).

Current mitigation (commit 629b5d0, temporary): `MCTSSearcher::search` stops when root visits reach
`3u << 30`. Analysis cannot continue past that point. This item removes the need for the cap.

## Decision (user, 2026-11-08): option B

Widen the counters but keep `sizeof(Edge) == 16`. Rejected alternatives and why:

- **A, plain `uint64_t` `edgeVisits`**: `Edge` grows 16 -> 24 bytes (+50% edge-array memory). SEARCH-02/05
  cut RSS 431 -> 195 MB at 2M nodes; leg T (4 threads, 256 MB) showed memory is the binding resource.
- **D, saturating clip**: the transposition rule in `searchNode` descends only when
  `childEdgeVisits >= childNodeVisits` (`search.cpp` ~line 620). A frozen edge makes this false forever,
  so the tree stops deepening. Worse than a clean stop.
- **C, per-node halving**: needs a "true total" separate from the edge sum and touches every
  edge-vs-node comparison.

## Design

`Edge` today (16 bytes, no padding): `Pos move` (int16) + `uint16 policy` + `atomic<uint32> edgeVisits`
+ `atomic<Node*> child`.

New layout, two `std::atomic<uint64_t>` words:

- `visitWord`: bits 0..47 = edge visits (max 2^48 - 1 = 2.8e14), bits 48..63 = quantized policy.
  `addVisits` is still a single `fetch_add`; no carry reaches the policy bits while visits < 2^48.
- `childWord`: bits 0..47 = `Node*`, bits 48..63 = `move` (int16, stored as uint16).

Rules:

- `setP` is constructor-only (edges are published by the release CAS in `Node::createEdgesFrom`). Make
  it private or assert it; the policy bits must never be rewritten after publication.
- `setChild(Node*)`: keep the move bits, store the pointer with release. Concurrent callers write the same
  pointer (`allocateOrFindNode` returns one node per hash), so the plain store stays benign.
- `getChild()`: acquire load, mask low 48 bits. `getMove()`: shift. `getVisits()`: acquire load, mask.
- Pointer width: require `(uintptr_t)ptr >> 48 == 0` in `setChild`, and say what happens when it fails
  (log with `ERRORL` and abort; one branch per expansion, not per visit). 32-bit targets (emscripten)
  fit trivially. Check ARM64 targets for MTE / pointer-authentication tag bits.
- `Node::n` -> `std::atomic<uint64_t>`. `nVirtual` stays `uint32_t` (bounded by in-flight playouts;
  `finishVisit` does `fetch_add(-newVisits)` and relies on its wrap). Record `sizeof(Node)` before and
  after; `nodeFootprint()` uses it. Goal: unchanged if padding absorbs the 4 extra bytes, otherwise
  record the delta.
- `static_assert(sizeof(Edge) == 16)` so `edgeArrayFootprint`, `EdgeArray` alignment and the memory
  budget (SEARCH-03) stay valid.
- Widen the type everywhere a visit count is held in a local: `search.cpp` lines ~316, 359, 523, 620-621,
  693, 743, `cpuctExplorationFactor`, `puctSelectionValue`, `readyToExpand`, `updateStats` (`nSum`,
  `childN`), `getVisits()` callers at 1218/1290/1327/1341. Per-playout counts (`newVisits`,
  `actualNewVisits`, `maxNumVisitsPerPlayout`) stay `uint32_t`.
- Remove the `MaxRootVisits` cap from `search()`; keep a guard far below 2^48 (for example 2^46) so the
  policy bits can never be corrupted.

### Two phases (separate commits)

1. **Pure widening.** Float accumulators in `updateStats` unchanged. Below 2^32 visits the MCTS result
   must be bit-identical to HEAD.
2. **Precision (only if phase 1 is clean).** `qSum`, `qSqrSum`, `dSum` as `double`: with weights near
   4e9 a float sum has an ulp of 256 and drops small edges. Expected error about 6e-8 relative, so this
   is optional. It changes MCTS numbers slightly, so measure it on its own.

## Derived-state audit (fill in before code)

List every reader and writer of: edge visits, policy bits, move, child pointer; anything that copies or
memcmp's an `Edge`; `EdgeArray` alloc size math (`EdgeArrayAlloc`, `edgeArrayFootprint`); the node
recycling path (it must still see a cleared/consistent `child`); `RootMove::numNodes` (already
`uint64_t`); output formatting of `V` (K/M suffix beyond 4294M).

Filled in 2026-11-08, from HEAD 629b5d0 (`search/mcts/{node.h,node.cpp,search.cpp}`; no other
engine file touches `Edge`; `tests/mcts_memory_tests.cpp` reads `getMove`/`getP`).

Sizes at HEAD (gcc 15.2, x86-64): `sizeof(Edge) = 16`, `sizeof(Node) = 56` (54 bytes of fields:
`terminalValue` is the last field, 2 bytes of tail padding), `sizeof(EdgeArray) = 8`, `alignof(Node) = 8`.

**Edge visits** (`edgeVisits`, `atomic<uint32_t>`).
- Writers: `searchNode` only: `addVisits(1)` after evaluating a newly allocated child,
  `addVisits(actualChildNewVisits)` after a descent, `addVisits(1)` on the transposition skip path.
  `addVisits` returns the value *before* the add (its comment says "after"; no caller uses it).
- Readers: `selectChild` (`childVisits` into `puctSelectionValue`), `searchNode` transposition rule
  (`childEdgeVisits >= childNodeVisits`), `Node::updateStats` (`childN` weights, `nSum`, and the
  `childN == 0` break that relies on explored edges forming a prefix), `computeSelectionValues`
  (`visitWeight`), `updateRootMovesData` (`rm->numNodes`, already `uint64_t`).
- Every reader converts to `float` (`uint32 -> float` and `uint64 -> float` round the same integer
  the same way), or compares/sums integers. Below 2^32 the widened code computes the same values.

**Policy bits** (`policy`, `uint16_t`). Writer: `Edge::setP`, called only from the constructor, which
runs in `Node::createEdgesFrom` (placement new into a private array before the release CAS on
`edges`). Readers (`getP`): `selectChild` (explored and first unexplored edge),
`computeSelectionValues`, `selectByRawPolicy`, `updateRootMovesData` (`policyPrior`), tests.
After the repack `setP` becomes private and constructor-only; the 16-bit value is unchanged.

**Move** (`move`, `Pos` = int16). Written only by the constructor. Readers (`getMove`): `selectChild`
(root-move filter), `searchNode` (`board.move`), `extractPVOfChildNode`, `updateRootMovesData`,
tests. `Pos::PASS` is -1, so the packed form must sign-extend.

**Child pointer** (`child`, `atomic<Node*>`). Writer: `searchNode` (`setChild` after
`allocateOrFindNode`; concurrent writers store the same pointer, the table returns one node per
hash). Readers (`getChild`, acquire): `selectChild` (a null child ends the explored prefix),
`updateStats`, `computeSelectionValues`, `applyLCBSelection`, `overrideBestByMateBounds`,
`extractPVOfChildNode`, `recursiveApply` (reachability marking for recycling),
`updateRootMovesData`. Initial value null; the packed form starts as `(move << 48) | 0`, so
`getChild()` is null until `setChild`.

**Copies / memcmp of `Edge`**: none. `Edge` holds atomics, so it is not copyable; it is only built in
place in `createEdgesFrom` and destroyed trivially by `delete[]` of `EdgeArrayAlloc` in `~Node`.

**`EdgeArray` size math**: `createEdgesFrom` allocates
`(sizeof(EdgeArray) + n * sizeof(Edge)) / sizeof(EdgeArrayAlloc)` units of
`aligned_storage<sizeof(EdgeArray), alignof(EdgeArray)>`; the static_asserts at `node.h` require
`alignof(EdgeArray) == alignof(Edge)` and `sizeof(Edge) % sizeof(EdgeArray) == 0`.
`edgeArrayFootprint(n) = sizeof(EdgeArray) + n * sizeof(Edge) + EdgeArrayAllocOverhead` feeds
`graphMemoryUsed` (SEARCH-03 budget) on insertion and on the recycle sweep. Two `uint64_t` atomics
keep `sizeof(Edge) = 16` and `alignof(Edge) = 8`, so all of this is unchanged on 64-bit targets.
On wasm32 the old `Edge` was 12 bytes (4-byte pointer) and the new one is 16: +33% edge memory on
that target only; recorded, not blocking.

**Node recycling**: `recycleOldNodes` marks nodes reachable from the root through non-null
`getChild()` (`recursiveApply`, age compare-and-exchange) and erases the rest from the node table.
It relies on `getChild()` returning exactly the stored pointer (masking must not drop bits) and on
reachable nodes never pointing at unreachable ones. Unchanged. Node age stays `uint32_t`: a 16-bit
age would wrap and make `recursiveApply` skip a still-reachable node whose old age equals the global
age, so its children would be freed under live edges.

**Node visits** (`Node::n`, `atomic<uint32_t>`). Writers: `setTerminal` / `setNonTerminal` (store 1,
release), `incrementVisits`, `finishVisit`. Readers: `selectChild` (`parentVisits` into
`cpuctExplorationFactor`), `searchNode` (discard when 0; `newVisits` cap
`uint32_t(parentVisits * MaxNewVisitsProp) + 1`, which becomes a float-to-uint32 conversion out of
range, i.e. undefined behaviour, once `0.36 * visits >= 2^32`; `readyToExpand`), transposition rule
(`childNodeVisits`), `applyLCBSelection` (`childVisits < 2`, `sqrt(var / visits)`),
`updateRootMovesData`, `setupRootNode` (`== 0`), `recycleOldNodes` (message), `getQVar`
(`visits < 2`, float weight), `search()` (the `MaxRootVisits` cap).

**Terminal value** (`terminalValue`, `atomic<Eval>` = int16). Writers: constructor (`VALUE_NONE`),
`setTerminal`, `setNonTerminal` (relaxed store `VALUE_NONE`), `tryMarkTerminal` (one-shot CAS from
`VALUE_NONE`). Reader: `isTerminal` (`searchNode`, `updateStats`). See the deviation below: it moves
into the top 16 bits of the 64-bit `n` word.

**`nVirtual`** stays `uint32_t` (in-flight playouts only; `finishVisit` relies on unsigned wrap of
`fetch_add(-newVisits)`).

**Output**: `RootMove::numNodes` is `uint64_t`; `nodesText(uint64_t)` prints `4294M`, then `G`
above 1e11, so `V` formats correctly past 2^32. `Engine::nodesSearched()` is already 64-bit.

**ARM64 / pointer width**: supported ARM64 presets are Windows (`arm64-msvc-*`, `base-arm64-clang`):
user space is 47 bits and heap data pointers carry no tag. macOS arm64 uses pointer authentication
for code/return pointers only, and its user space is 47 bits. Linux/Android with MTE or Top-Byte-Ignore
heap tagging (Android 11+ Scudo puts a tag in bits 56..63) is not a supported preset; there the
`setChild` check fires, logs with `ERRORL` and aborts instead of corrupting the move bits. 32-bit
targets (wasm32) fit trivially.

### Deviation: `Node` packs `terminalValue` with the visit count

The plan said "record the delta if padding does not absorb the 4 extra bytes". It does not: only 2
bytes are padding, so a plain `atomic<uint64_t> n` makes `sizeof(Node)` 64. Nodes live in
`std::set` rb-tree nodes (32 bytes of links + `Node`); with glibc that is a 96-byte chunk at 56 and a
112-byte chunk at 64, about +16 bytes per node, which breaks the "RSS unchanged" requirement of
option B (measured below). Fix in the spirit of the `Edge` design: one `atomic<uint64_t> visitWord`,
bits 0..47 = visits, bits 48..63 = `terminalValue` (int16). `incrementVisits` / `finishVisit` stay a
single `fetch_add` (no carry while visits < 2^48, guarded at 2^46); `setTerminal` /
`setNonTerminal` store visits = 1 and the tag in one release store; `tryMarkTerminal` is a CAS loop
on the whole word (retry when a concurrent `fetch_add` changes the visits, give up when the tag is no
longer `VALUE_NONE`); `isTerminal` is a relaxed load of the tag. `sizeof(Node)` stays 56
(`static_assert` on 64-bit). Extra tests: `Node_size_is56BytesOn64BitTargets`,
`Node_setTerminal_freshNode_visitsOneAndTerminal`,
`Node_isTerminal_visitsAboveTwoPow32_staysFalseUntilMarked`,
`Node_tryMarkTerminal_concurrentWithIncrementVisits_visitsPreserved`,
plus `Edge_addVisits_returnsVisitsBeforeTheAdd` and `readyToExpand_visitsAboveTwoPow32_true`.

## Test list (write before code; new file `tests/mcts_visits_tests.cpp`, add to `TEST_DRIVER_SOURCES`)

- `Edge_size_is16Bytes`
- `Edge_getP_roundTripUnchangedForAllQuantizedPolicies` (same values as before the change)
- `Edge_addVisits_acrossTwoPow32_countsExactly`
- `Edge_addVisits_nearTwoPow48_policyBitsIntact` (policy and move unchanged, up to the guard)
- `Edge_setChild_preservesMoveAndReturnsPointer` (negative `Pos` such as `PASS` included)
- `Edge_getChild_initiallyNull_andVisitsZero`
- `Edge_concurrentAddVisits_totalIsExact` (several threads, total above 2^32 via large deltas)
- `Node_incrementVisits_acrossTwoPow32_getVisitsIs64Bit`
- `Node_finishVisit_acrossTwoPow32_nVirtualReturnsToZero`
- `updateStats_edgeVisitsAboveTwoPow32_qIsWeightedAverage` (edges primed with `addVisits(3e9)` etc.)
- `cpuctExplorationFactor_visitsAboveTwoPow32_monotonicAndFinite`
- `puctSelectionValue_childVisitsAboveTwoPow32_noWrap`
- `searchNode_rootPrimedAboveTwoPow32_visitsKeepGrowing` (prime root edge/`n` to 2^32 - 1000, run a few
  playouts, check `V` grows and Q stays in [-1, 1])
- `searchNode_transpositionRule_edgeAboveNodeStillDescends` (guards option D's failure mode)
- Regression guard: single-thread, fixed 2M-node MCTS search from a fixed position gives identical root
  move list (visits, Q) before and after phase 1. Capture the golden output from HEAD first.

Added before code (2026-11-08), see the deviation in the audit: `Edge_addVisits_returnsVisitsBeforeTheAdd`,
`Node_size_is56BytesOn64BitTargets`, `Node_setTerminal_freshNode_visitsOneAndTerminal`,
`Node_isTerminal_visitsAboveTwoPow32_staysFalseUntilMarked`,
`Node_tryMarkTerminal_concurrentWithIncrementVisits_visitsPreserved`, `readyToExpand_visitsAboveTwoPow32_true`.
The two `searchNode_*` tests run a real single-thread MCTS search through `Search::Engine` with a
deterministic stand-in evaluator (`HashEvaluator`, the test binary loads no network); the root is
reused on the second search of the same position.

Tests must run with asserts live (see `docs/process/unit-tests.md`).

## Measure

1. Bench stays `17621937 / 8c1f72e5` (built-in config is alpha-beta, so it should not move).
2. Peak RSS, one fixed 2M-node search, Standard, default config: same as before (195 MB at `pm99`
   default). Any rise is a failure of option B.
3. NPS, MCTS, 1 thread and 8 threads, fixed time, same position, median of repeated runs, PGO build
   (`scripts/pgo_build.sh x64-clang-AVX512VNNI`). Accept within noise; above -1% means look again.
4. Long run: infinite analysis for 20 minutes or more on a position with a dominant PV (the one in
   `test_mcts.txt`). Across 2^32 root visits, `EV` and `W` must be continuous, `V` monotonic, `W <= 100`.
5. Fallback decision: if the packed layout costs more than 1% NPS or the pointer-width check fails on a
   supported target, fall back to option A behind the same accessor API and record the memory cost.

## Results — 2026-11-08

Measurement files: `build/search08/` (not in the repo). Fixed position for the golden, RSS and long
run: Standard, `7,7 9,8 6,7 7,9` (the SEARCH-02 position), `INFO MAX_MEMORY 6000000000` for the
2M-node runs (with the 32 MB default the graph closes early and RSS says nothing).

| Check | HEAD 629b5d0 | After d82d577 | Result |
|---|---|---|---|
| `sizeof(Edge)` / `sizeof(Node)` (x86-64) | 16 / 56 | 16 / 56 (both `static_assert`ed) | pass |
| Bench (gcc Release, built-in config) | 17621937 / 8c1f72e5 | 17621937 / 8c1f72e5 (also after A and B; PGO build too) | pass |
| Golden: 1 thread, 2M nodes, `SHOW_DETAIL 2`, root list every 100K nodes (exact visits, 6-digit winrate, PV) | `golden_base.extract` (11103 lines, deterministic over 2 runs) | identical after A, B, C (gcc) and HEAD-PGO vs final-PGO | pass |
| Peak RSS, same 2M-node search (gcc) | 185.4 - 188.5 MB (6 runs) | 185.5 - 188.2 MB (A 185.7, B 185.5, C 4 runs) | pass, within run noise |
| Cost, PGO, fixed nodes, `perf` user cycles per node, median of 5 interleaved | 1 thr 18428, 8 thr 20294 | 1 thr 18575 (+0.8%), 8 thr 20369 (+0.4%) | pass (below the 1% fallback line) |
| Tests (`rapfi_tests`, asserts live) | 14 of 20 new tests fail (wrap), 6 guards pass | 136/136 | pass |
| Long run, PGO, 8 threads, 2 GB, infinite, 1200 s | (HEAD stops at 3<<30) | PV1 `G9` V 1.80e10, N 1.80e10; crossing 2^32 at 338 s | pass |

- Rejected plain widening of `Node::n` (measured, not committed): `sizeof(Node)` 64 and peak RSS
  193.5 MB (+7.8 MB) on the same search. Hence the packed node word (deviation in the audit).
- Long run across 2^32 (`longrun_final.log`, `longrun_check.txt`): 1200 one-second lines, PV1 is `G9`
  throughout, V and N never decrease, `W` 97.68 - 99.89 (never above 100), no mate-string `EV`, largest
  step between consecutive lines 16 in `EV` and 0.29 in `W`; around the crossing (335 - 341 s) EV 731 - 732,
  W 99.81 - 99.82. Speed about 12.8 M visits/s after the graph closed at the 2 GB budget.
- NPS, fixed time (5 s, 6 interleaved rounds) was too noisy to resolve 1%: medians 1 thread
  153.6K -> 160.6K, 8 threads 762K -> 737K, paired ratios 0.70 - 1.30. Power state: PL1 24 W / PL2
  51 W (`throttled` inactive). So the decision uses user-mode cycles per node at a fixed node count,
  which do not depend on clock speed (`cycles_ab.log`): instructions +0.09% at 1 thread.
- Engine-driven tests run with a stand-in evaluator (`HashEvaluator`), because the test binary loads
  no network.

### Deviations from the instruction file

- `Node` packs `terminalValue` into the visit word (see the audit) instead of only recording a size delta,
  because the size delta raised RSS.
- Long-run position: `test_mcts.txt` has no BOARD record, so its position cannot be recovered. The run used
  the golden position, where `G9` takes about 99% of the visits. Its `W` is near 99.8%, higher than the 68%
  in `test_mcts.txt`.
- NPS is measured as cycles per node (see above). Fixed-time NPS was run and recorded but is too noisy on
  this laptop.
- Tests were committed first in a separate `test:` commit (`233893b`). It fails by design until step C.
  Step A turned 8 of the failing tests green, B another 4, and C the 2 engine-driven tests.
- Phase 2 (`double` accumulators) was not done. It is optional, and it changes MCTS numbers, so it
  needs its own measurement. Open: with weights near 4e9 a float `qSum` has an ulp of 256.

## Known limits

- 48-bit pointer assumption (x86-64 user space is 47 bits; 5-level paging only if the process asks for
  high addresses).
- `float` precision in `updateStats` until phase 2.
