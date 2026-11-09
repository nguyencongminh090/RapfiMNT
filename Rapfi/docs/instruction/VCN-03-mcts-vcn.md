# VCN-03 — execution guidance

Task: [`../todo/VCN-03-mcts-vcn.md`](../todo/VCN-03-mcts-vcn.md). Option B (native MCTS-VCN), done in
slices (§8.1). Slice 0 (guard) and the bound work it depended on (SEARCH-09, SEARCH-10) are done.
**This file covers slice 2, key and root identity, only.** Slices 3–5 get their own sections here
before they start.

## Slice 2 — key and root identity

### Goal

MCTS nodes searched in VCN mode never mix with nodes from an ordinary search, from another level,
or from the other attacker, including across searches through graph reuse. With VCN off,
everything is bit-identical. No VCN search *behaviour* is added in this slice: no pass edge, no
VC4/VC5 terminals. Those are slices 3–4.

### Design (from todo §3.1, fixed)

- **Per-node level, derived from the board.**
  `level = min(VC5, rootLevel + (board.passMoveCountOfSide(defender) - rootDefenderPasses))`,
  where `defender = ~options.vcnMode.attacker`, and `rootLevel = options.vcnMode.n`.
  `rootDefenderPasses` is captured once per search, before the workers start (in `searchMain` or
  `setupRootNode`, stored in `MCTSSearcher::scratch`). No `Node` field and no recursion parameter
  (`Node` stays 56 bytes).
- **Node key** = `board.zobristKey() ^ Vcn::hashXor(options, level)`. `hashXor` is `0` when VCN
  is off, so the key equals the zobrist key exactly.
- **One helper** (for example `mctsNodeKey(const Board &, const SearchOptions &, int
  rootDefenderPasses)`, or a searcher method) used at every key site:
  - child allocation in `searchNode` (`search/mcts/search.cpp:649-650`);
  - root allocation in `setupRootNode` (`search.cpp:1318`);
  - the `node.getHash()` assert in `searchNode` (`search.cpp:565`).
  
  Grep again for `zobristKey()` and `allocateOrFindNode(` under `search/mcts/` before editing.
- **Share the VCN helpers.** `namespace Vcn` (`levelIncrement`, `childLevel`, `vc5AttackerLoses`,
  `hashXor`) sits inside the anonymous namespace of `search/ab/search.cpp:244-276`. Move it as is
  to a shared header (for example `search/vcn.h`) and include it from both searchers. This is a
  pure move with no change to the code. AB must stay bit-identical (bench and `tests/vcn_tests.cpp`).
- **Root reuse.** The early return in `setupRootNode` (`search.cpp:1312`) compares only the move
  list. Also compare the root key: store the previous root key in `GraphState` next to
  `previousPosition` and rebuild the root when it differs. Otherwise a VCN search on the same
  position as the last ordinary search would reuse the ordinary root. Decide whether the
  `recycleOldNodes` condition needs the same key test; nodes of the other mode just become
  unreachable and age out. Write the decision down.
- **The guard stays.** `searcherSupportsVcn()` (`command/gomocup.cpp:72`) keeps refusing `YXVCN`
  and warning under MCTS until slice 5. VCN under MCTS is not usable until the pass edge and the
  terminals exist. Tests drive the searcher directly, with `SearchOptions::vcnMode` set, and do
  not go through the protocol.

### Tests (write first, `<function>_<scenario>_<expectedBehavior>`)

The todo's §7 slice-2 list, made concrete:

- `mctsNodeKey_vcnOff_equalsZobrist`
- `mctsNodeKey_vcnOn_differsByLevelAndAttacker`: same board; level VC2 vs VC3, and attacker
  BLACK vs WHITE, give four distinct keys, all different from the zobrist key.
- `mctsNodeKey_defenderPassRaisesLevel_cappedAtVc5`: the key after one or two defender passes
  equals the key computed with the raised level. At VC5 a further pass keeps VC5.
- `setupRootNode_samePositionVcnToggled_rebuildsRoot`: ordinary search, then a VCN search on the
  same moves. The root node differs (key/hash), and the next ordinary search gets the ordinary
  root back.
- `mctsVcn_afterVcnSearch_nextOrdinarySearchReusesNoVcnNode`: after a VCN search, every node
  reached by an ordinary search has a key equal to its board's zobrist key. Walk the tree from the
  root via edges, replaying the moves on a `Board`.
- `vcnHelpers_movedToSharedHeader_abBehaviourUnchanged`: covered by the existing
  `tests/vcn_tests.cpp` staying green. Name the check in the todo; no new test needed.

Each test builds its own `Board`, is deterministic, and has asserts live (`rapfi_tests` forces
`-UNDEBUG`). An MCTS search in a test needs an evaluator: follow the existing MCTS tests
(`tests/mcts_*_tests.cpp`) for the setup, and use a small fixed node count.

### Pitfalls

- **The VCF TT key is separate.** `SimpleVCF` uses `board.zobristKey()` for the shared VCF TT
  (`search.cpp:115`). Leave it alone in this slice; its VCN semantics (`vcfOnly`) are slice 4.
  Write that down in the todo, so nobody thinks slice 2 covered it.
- **Passes and zobrist.** Check how `Board::move(Pos::PASS)` changes `zobristKey()` (side to
  move only?). The level formula relies on the defender's pass count, not on the key, so this is
  fine either way, but confirm it and write it down.
- **`rootDefenderPasses` visibility.** Write it before `runOnAllThreads` dispatches the workers
  (the same happens-before as `graph.root`; see the comment at `searchMain`). Do not store it in
  `SearchOptions`, which the protocol owns.
- **Attacker.** `think()` sets `options.vcnMode.attacker = board.sideToMove()`
  (`command/gomocup.cpp:127`). Tests that drive the searcher directly must set it themselves.
- **Bit-identity with VCN off** is the main risk. Check the bench, and also an MCTS fixed-node
  golden output with VCN off, because BENCH is alpha-beta and does not cover MCTS. Reuse
  `build/search10/` (or `build/search09/final_extract.sh`) for the golden.
- `real_test` and `test_mcts.txt` in the repo root: do not edit or commit.

### Don't touch (slice 2)

- `expandNode`, the MovePicker pass stages, priors (slice 3). `evaluateNode` and the terminals,
  `SimpleVCF`, `wincheck.h` (slice 4).
- The SEARCH-09/10 bound and widening code, `Edge`/`Node`/`EdgeArray` sizes, and the MCTS
  defaults (the cut stays on in VCN, decision §8.4).
- AB VCN behaviour. The helper move must be a pure move.
- The protocol guard in `command/gomocup.cpp` (it stays until slice 5).

### Definition of done (slice 2)

- The tests above pass with asserts live, the full `rapfi_tests` suite is green, and the
  fail-first state is recorded.
- Bench `17621937 / 8c1f72e5`. The MCTS golden output with VCN off is identical to the HEAD of
  branch `search-09-10-mcts-bound`.
- `tests/vcn_tests.cpp` is green (AB VCN unchanged after the helper move).
- Todo §9 has a "Slice 2" progress entry with the numbers and decisions (the recycle condition,
  the zobrist/PASS finding, the VCF TT key deferred). The status line says slice 2 is done.
  VCN-03 stays **Active** in `TODO.md`, since slices 3–5 remain. Update the index line's
  progress text.
- Self-play is not needed: with VCN off the change is bit-identical. The golden output is the
  guard.
- Commit on branch `search-09-10-mcts-bound`, in conventional style (`test:`, `search:`,
  `docs:`), only if the user asked.
