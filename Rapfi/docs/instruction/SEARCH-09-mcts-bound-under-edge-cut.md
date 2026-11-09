# SEARCH-09 — execution guidance

Task: [`../todo/SEARCH-09-mcts-bound-under-edge-cut.md`](../todo/SEARCH-09-mcts-bound-under-edge-cut.md).
Chosen fix: **option (a)**, decided 2026-11-09. A node whose edge list was cut cannot become a proven
loss from its kept children alone. Option (b), adding the cut moves back, is out of scope.

## Where the cut is recorded (deviation from the todo wording)

The todo says to use "a spare bit" so that `Node` stays 56 bytes. Use the **padding in `EdgeArray`**,
not `Node`:

- `struct alignas(alignof(Edge)) EdgeArray` (`search/mcts/node.h`, about line 131) holds a single
  `uint32_t numEdges` before an 8-byte-aligned flexible `Edge edges[]`. That leaves 4 bytes of
  padding, so `sizeof(EdgeArray) == 8`.
- Add `uint32_t numMoves;`, the number of generated moves before the cut. Add an accessor
  `bool edgesCut() const { return numEdges < numMoves; }`. `createEdgesFrom` (`node.cpp:177`)
  writes it next to `numEdges`. That happens before the release CAS, so the field is immutable
  once published. It needs no atomics and adds no new race.
- Add `static_assert(sizeof(EdgeArray) == 8)`. The size math depends on it: `numAllocs` in
  `createEdgesFrom`, `edgeArrayFootprint`, and the existing
  `sizeof(Edge) % sizeof(EdgeArray) == 0` check. `Node` is untouched; its 56-byte `static_assert`
  still holds.
- Record this choice in the todo's decision note. Storing the count, not a bool, also gives
  VCN-03 and a later option (b) the information they need.

## The fix (semantics)

Treat the cut moves as **one unexplored child with an unknown bound** `(-inf, +inf)`. In
`Node::updateStats` (`node.cpp:236`), after the edge loop:

```cpp
if (edgeArray->edgesCut())
    maxBound |= ValueBound {};   // cut moves: unknown, so no proven loss / proven draw-or-worse
```

- This leaves `maxBound.lower` unchanged, so a proven win from a kept child still propagates. It
  sets `maxBound.upper` to `+inf`, so the node can never be proven lost (or capped) by kept
  children alone. This is the exact sound result. Do not invent a "non-mate clamp" value.
- The loop `break`s at the first unvisited edge, which already folds `ValueBound{}`. The extra fold
  must run on every path after the loop, including the no-break path. Folding twice is harmless.
- Terminal nodes stay as they are (`if (!isTerminal())` guard). The root is never cut.

## Approach

1. **Baselines from HEAD before touching code.** Keep outputs under `build/search09/`, not in the
   repo.
   - Run `echo BENCH | ./pbrain-rapfi`. It must report Nodes `17621937`, Hash `8c1f72e5`.
   - Golden output with the cut **off** (`mcts_edge_policy_mass = 1.0`): single thread, fixed node
     count, a few fixed positions (Standard). Save the root move list (move, visits, Q, value).
     This guards bit-identity.
   - Mate-claim count with the cut **on** (default `0.99`). Use a fixed-node MCTS search over a
     position set, for example the first 100 of `build/oppo4-ab/op_std_main.txt` plus positions
     with an open three. For each position, record whether the final root value is a mate score
     (`+M`/`-M` in the output) and the chosen move. Repeat with `1.0` on the same set. A position
     that is mate with `0.99` but not with `1.0` is a candidate false proof.
2. **Write the todo's test list as tests that fail first**, in a new
   `tests/mcts_bound_tests.cpp` added to `TEST_DRIVER_SOURCES` (`CMakeLists.txt:696`). Build
   synthetic graphs: call `createEdgesFrom` with a hand-made policy array that has more than 8
   moves and a cut, attach children, make them terminal with `setTerminal` or `tryMarkTerminal`,
   give the edges visits, then call `updateStats` and check `getBound()`. Follow the fixtures in
   `tests/mcts_visits_tests.cpp` / `tests/mcts_memory_tests.cpp`. Add these tests beyond the
   todo's list:
   - `updateStats_edgesCut_keptChildProvenLoss_parentWinLowerUnchanged`: the proof direction is
     kept.
   - `updateStats_edgesCut_allKeptChildrenDraw_upperNotCapped`.
   - `edgeArray_sizeUnchanged`: `sizeof(EdgeArray) == 8` and `sizeof(Node) == 56`.
3. **Position test.** Use step 1's scan to find a real position where `0.99` gives a false mate,
   then confirm it by hand. One check: `1.0`, or AB/VCF on the claimed line, finds the saving
   reply. Freeze that position and node count in the test, or in a script under `build/search09/`
   if a full MCTS search is too slow for `rapfi_tests`. **If no such position turns up within a
   reasonable scan, say so in the todo.** Do not build a fake one or claim it was reproduced. The
   synthetic unit tests then serve as the failing tests.
4. **Implement:** add the `EdgeArray` field and accessor, write it in `createEdgesFrom`, and add
   the fold in `updateStats`. Nothing else.
5. **Verify** (see Definition of done). Then rebuild the release with
   `scripts/pgo_build.sh x64-clang-AVX512VNNI` and run self-play on the PGO build.

## Pitfalls

- **Who reads `bound`:** only `computeSelectionValues` → `overrideBestByMateBounds`
  (`search/mcts/search.cpp:713`, `:812`, `:889`) and the root-move value report (`:1357-1365`).
  PUCT selection does not read it. Expect changes only where mate proofs are involved. Grep again
  before editing in case this changed.
- **Only the `upper` direction is unsound.** Do not touch `lower`, `ValueBound::operator|=`, or
  terminal handling. Weakening `lower` would throw away valid proofs.
- **Transpositions:** a node reached through several parents shares one edge array, and the flag
  belongs to that node. This is correct. Do not key the flag on the parent edge.
- **Bit-identity with the cut off:** with `massKeep >= 1`, `selectEdgeCount` returns `numMoves`,
  so `edgesCut()` is false and the fold is skipped. If the golden output differs, the change
  leaked outside the cut path. Stop and find out why; do not re-baseline.
- **MCTS is not covered by BENCH.** The built-in config is alpha-beta. A green bench only shows
  that the alpha-beta search and the shared code were not disturbed. The golden diff, the unit
  tests and the mate-claim scan are the real guards.
- **Tests with asserts live:** `rapfi_tests` forces `-UNDEBUG`. Never do that to the `rapfi`
  target.
- A plain `ninja` build is only a compile check (gcc here). Strength numbers come from the PGO
  build.
- Self-play: the expected effect on strength is small, because false mate overrides are rare. A
  tie inside noise is the expected pass. Do not tune or extend the run to chase significance.

## Don't touch

- `mcts_edge_policy_mass`, `mcts_min_edges`, `mcts_expand_min_visits` and the graph-budget
  defaults (SEARCH-02/03/05).
- The layouts of `Edge` and `Node` (SEARCH-08: 16 and 56 bytes), and `selectEdgeCount`'s cut
  rule.
- `overrideBestByMateBounds` and the selection/best-move logic. The fix belongs in the bound, not
  in its consumers.
- `game/pattern.*`, bitkeys, zobrist, `movegen.cpp`, the alpha-beta search, and the VCN code
  (VCN-03 comes later and builds on this).
- Uncommitted data files in the repo root (`real_test`, `test_mcts.txt`). Read them; do not edit
  or commit them.

## Definition of done

- All of the todo's tests plus the extra ones above pass in `rapfi_tests` with asserts live. The
  full suite is green, and the new tests failed before the fix (record that in the todo).
- Bench reports `17621937 / 8c1f72e5`. The golden output with the cut off is identical.
- Mate-claim scan before/after with `0.99`: report the counts and every position whose claim
  changed. Report the position test as reproduced or not reproduced, honestly.
- Self-play of fix vs base (HEAD) with `0.99`, using SEARCH-05 stage-1 settings:
  `OPFILE=op_std_main.txt build/oppo4-ab/run.sh <fix> <base> 1 120 tc=0/2` (Standard, 1 thread,
  256 MB, SPRT [-10, 10]). Put the binaries in their own dirs under `build/oppo4-ab/`. Pass means
  no regression beyond about 1 SE (about ±24 Elo at 120 games).
- Todo: decision note finalised (option (a), `EdgeArray` padding), results and numbers written,
  status updated. `TODO.md` and this file's `instruction.md` line moved to `## Done` with the date.
  VCN-03's "blocked by SEARCH-09" note updated.
- Commits in conventional style: `test:` (failing tests), `search:` (fix), `docs:` (results).
  Branch first if on `main` and the user asked for commits; otherwise leave the changes
  uncommitted and report.
