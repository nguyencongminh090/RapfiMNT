# VCN-03 — VCN search mode under the MCTS searcher (research report)

**Status:** option B chosen (2026-11-08, user). Slice 0 (guard) done 2026-11-08; slice 1 is
SEARCH-09/10 (done 2026-11-09); slice 2 (node key and root identity) done 2026-11-09; slice 3
(defender pass edge and its prior) done 2026-11-09; slice 4 (VC4/VC5 terminals; AB primitives
reused instead of factoring AB, a deviation from §8.1) done 2026-11-09, see §9; slice 5 not started. The protocol guard stays until slice 5. Depended on [SEARCH-09](SEARCH-09-mcts-bound-under-edge-cut.md), done 2026-11-09
(option (a); `EdgeArray::numMoves` / `edgesCut()` available), and on
[SEARCH-10](SEARCH-10-mcts-readd-cut-moves.md), done 2026-11-09 (option (b)): when every kept reply of
a cut node is proven lost, the node's edge array is replaced by the full move list, so the edge cut
no longer blocks a proof ("every defender reply loses" is provable again). Caveat: once the graph is
closed (SEARCH-03 memory budget reached) no widening happens, and such a node stays unprovable (sound,
not complete). The cut no longer blocks a proof except in a closed graph. VCN mode keeps the cut
(decision 4, §8). Decisions in §8.

**Class (if implemented):** B (search behaviour) + C (command surface). The MCTS node key change
(§3.1) follows the same reasoning as the AB TT key in [VCN](VCN-search-mode.md): a VCN-only XOR,
identity (`0`) when VCN is off, so not class A. Re-check that call before any code.

**Knowledge lookup (CLAUDE.md rule 10):** `SKILLS_TREE/tools/find -k knowledge "MCTS proof number
search solver threat space search pass move gomoku"` returned no matching note (top hits were
security / database notes). The design opinions below come from reading this tree's code, not from
a library note.

## 1. Question and short answer

*Can MCTS run the VCN mode (`INFO VCTHREAD`, `YXVCN`)?*

Not today. `SearchOptions::vcnMode` (`search/searchcommon.h:193`) reaches the MCTS searcher, but
no MCTS code reads it: every use is in `search/ab/search.cpp`. With `searcher = mcts`, `YXVCN 3`
runs an **ordinary** MCTS search and prints its result as if it were a VCN answer. No error,
no warning.

It can be made to work, but MCTS is a weak fit for VCN, because VCN is a **proof** question
("can the attacker force a win at level N?") while MCTS gives a **statistical estimate**. The only
proof channel MCTS has is the mate `ValueBound` propagation, and that channel has a soundness gap
under the current default edge cut (§4.1). Recommendation in §6.

## 2. What VCN means today (AB implementation, the reference)

From [VCN](VCN-search-mode.md), [VCN-02](VCN-02-attack-search-quality.md) and `search/ab/search.cpp:243-277`:

| Element | AB behaviour |
|---|---|
| Attacker | Side to move at `think()` (`command/gomocup.cpp:111`). Root never passes. |
| Defender pass | `MovePicker` stages `MAIN_PASS`, `DEFENDFOUR_PASS`, `DEFENDB4F3_PASS` (`search/movepick.cpp:578`); never against a five, never twice in a row. |
| Level | Per ply, `Vcn::childLevel`: a defender pass raises the level by one, capped at VC5. |
| VC4 | Attacker node goes to `vcfsearch` with `vcfOnly` (fours only, no stand-pat, `quickWinCheck(..., vcfOnly)` so a double three is **not** a win — VCN-02 item 1b; whole-board VCF generation — VCN-02 item 1). Defender node: `vcfdefend` returns "defender wins by passing" when the attacker made no five threat. |
| VC5 | Attacker without a five on the board loses (`Vcn::vc5AttackerLoses`). |
| TT key | `board.zobristKey() ^ Vcn::hashXor(options, level)`; `0` when VCN is off. |
| Null move / database | Off while VCN is on. |
| Value | "Attack chance"; heuristic pruning kept; result is a search result, not a certificate (VCN-02 §Settled decisions). |

## 3. What MCTS would have to change

Code read: `search/mcts/search.cpp` (`evaluateNode` 466, `expandNode` 412, `searchNode` 514,
`selectChild` 311, best-move / PV 670-930, `setupRootNode` 1223, `updateRootMovesData` 1324),
`search/mcts/node.{h,cpp}` (`createEdgesFrom`, `updateStats`, `ValueBound`).

### 3.1 Node identity (graph key)

- Nodes are keyed by `board.zobristKey()` (`allocateOrFindNode`, `search.cpp:289`), and
  `searchNode` asserts `node.getHash() == board.zobristKey()` (`search.cpp:517`).
- **Inside one VCN search the level is a function of the position.** The attacker never passes,
  so the number of defender passes since the root equals (attacker stones added) − (defender stones
  added) (± one by side to move). Same stones + same side to move ⇒ same pass count ⇒ same level.
  Transpositions inside one search are therefore level-safe without a key change.
- **Across searches it is not safe.** The node table survives between searches (`clear(false)`,
  graph reuse in `setupRootNode`). A VCN node must not be reused by an ordinary search or with the
  other attacker. So MCTS needs the same per-node `hashXor(level, attacker)` as AB, applied at
  every key site: `allocateOrFindNode` callers, root setup (`search.cpp:1248`), and the assert at
  517. With a per-node level in the key, nodes at the same level reached from different root
  levels correctly share a key: the value depends only on (position, level, attacker).
- `setupRootNode` returns early when `rootPosition == graph.previousPosition`
  (`search.cpp:1242`). A switch between ordinary and VCN search on the same position would reuse
  the wrong root. The early-out must also compare the VCN mode (or the root key).
- Level per node is needed during search (VC4/VC5 rules), including in `evaluateNode` and
  `expandNode`, which take no level. It needs neither a recursion parameter nor a `Node` field
  (`Node` is kept at 56 bytes by SEARCH-08): `Board` counts passes per side
  (`passMoveCountOfSide`, `game/board.h:330`), so
  `level = min(VC5, rootLevel + passMoveCountOfSide(~attacker) − rootDefenderPasses)`, O(1)
  wherever needed, with `rootDefenderPasses` captured once in `searchMain`.

### 3.2 Expansion: the defender pass edge

- `expandNode` builds a `MAIN` picker with `generatePassMove` unset (`search.cpp:430`). Setting it
  for defender nodes yields `Pos::PASS` first, with `curPolicy = 0.0f` (`movepick.cpp:586`).
- **Policy 0 breaks the MCTS ordering invariant.** `selectChild` stops at the first unexplored
  edge and assumes it has the highest remaining policy (`search.cpp:351`, comment at 378). PASS at
  index 0 with prior 0 is first in order but last in policy. Its PUCT term `U` is 0 forever, so
  once visited it is only revisited on `Q`. In VCN the pass is the defender's **main refutation**
  (a quiet attacker move is refuted by a pass), so a zero prior is wrong in substance too.
  Needs an explicit pass prior (config value, e.g. `mcts_vcn_pass_prior`), and the edge array
  re-sorted or the pass placed by prior.
- The policy-mass edge cut (`mcts_edge_policy_mass = 0.99`, non-root, SEARCH-05) runs on the
  sorted list; with a real prior the pass survives the cut, but see §4.1.
- Root expansion uses the `ROOT` picker, which never yields PASS. Correct (attacker root).
- Evaluator: `board.move(Pos::PASS)` is already supported by the evaluator hooks
  (`eval/evaluator.h:153`, `eval/evaluator.cpp:131`), as AB uses it. No NN change needed.

### 3.3 Leaf evaluation and terminals (`evaluateNode`, `search.cpp:466`)

Current order: board full / maxMoves → draw; `quickWinCheck` → mate; `SimpleVCF::vcf` → mate;
else NN value. Under VCN:

| Level / side | Needed rule | Gap today |
|---|---|---|
| any, defender to move with own VCF | defender wins | already right (`SimpleVCF::vcf` on side to move) |
| any, attacker to move with VCF | attacker wins | right, but `SimpleVCF` uses the local `QVCF` picker (`search.cpp:140`): far VCFs missed — the VCN-02 item 1 bug |
| VC4, attacker to move, no VCF | **attacker loses** (defender will pass) | today falls through to the NN value |
| VC4, attacker to move | `quickWinCheck` must use `vcfOnly` | the runtime wrapper `quickWinCheck(Rule, ...)` (`game/wincheck.h:118`) has no `vcfOnly` parameter; a double three is scored as a win — the VCN-02 item 1b bug, also inside `SimpleVCF::vcfsearch` (`search.cpp:100`) |
| VC4, defender to move, attacker made no five threat | defender wins by passing | not modelled |
| VC5, attacker to move, no five | attacker loses | not modelled |
| board full / maxMoves | draw, as AB (user decision 2026-11-08); VC5 loss checked first | already `getDrawValue`; VC5 check must come before it |
| VC2/VC3 leaf | NN value as "attack chance" | NN value is the value of the *real* game; after a defender pass it sees an extra attacker tempo, which matches "attack chance" in direction but is not calibrated (same caveat as AB's eval stand-pat) |

`SimpleVCF` cannot be reused for VC4 as is. Either give it a `vcfOnly` mode (whole-board VCF
generation, `quickWinCheck(..., vcfOnly)`, no stand-pat) or factor the AB VCN VCF code out of the
anonymous namespace of `search/ab/search.cpp` so both searchers call one VC4 solver.

### 3.4 Selection and terminal value at the defender's VC4 node

A VC4 defender node where the attacker's last move made no five threat is a terminal defender
win. With §3.3 handled as a terminal in `evaluateNode`, the existing `tryMarkTerminal` /
`ValueBound` path propagates it unchanged.

### 3.5 Output and root

- `updateRootMovesData` turns proven bounds into `mate_in` / `mated_in` (`search.cpp:1363`), so a
  proven VCN line prints as `+M` / `-M` like AB. A non-proven line prints a win-rate value.
- PV may contain `PASS` (`extractPVOfChildNode`); the printer must print it the way AB's VCN PV
  does (AB does not format it specially either — VCN §Deliberately not done).
- Singular-root early stop (`search.cpp:1207`) and `numNodesAfterSingularRoot` are fine.
- `recycleOldNodes` is key-agnostic; no change.

## 4. Findings that matter beyond a plain port

### 4.1 The mate-bound proof is unsound under the policy-mass edge cut (general, verify first) — headline finding

`createEdgesFrom` keeps only the leading edges up to `massKeep` (`node.cpp:187`), and
`updateStats` builds the parent `ValueBound` only from the edges that exist (`node.cpp:253-274`).
A move removed by the cut is invisible to the bound. So a node whose **kept** children are all
proven wins for the opponent is marked proven lost even if a cut move saves it, and its parent can
then report a false `+M`.

- **It changes the move played, not only the printed score.** The false proven loss of a child
  raises the parent's `maxBound.lower` to a mate value; `selectBestmoveOfChildNode` then runs
  `overrideBestByMateBounds` (`search.cpp:889`), which multiplies every non-proven child's
  selection value by `1e-9`. The engine plays the falsely "proven" move.
- **Only the proven-loss direction (`upper`) is unsound.** One proven-winning child is a valid
  proof whether or not other moves were cut.
- **Origin:** the cut is this fork's own addition (SEARCH-02) and became the default on
  2026-11-08 (SEARCH-05, `0.99`). SEARCH-05 measured strength and memory, not proof soundness.
- **Which nodes can be hit:** `mctsMinEdges = 8` (`search/searchconfig.h:57`), so a node with ≤ 8
  replies (most nodes facing a four) is never cut. A test position needs > 8 replies: a defence
  against an open three with scattered counter-fours, or a quiet node, where the only saving move
  has low policy.

- In VCN this is the central case: "every defender reply loses" is exactly the proof, and a
  defender reply with low policy (often a pass or a far counter-four) is the one most likely to be
  cut.
- In ordinary MCTS it can also produce false mates; with `mctsEdgePolicyMass = 0.99` (default
  since SEARCH-05) it is live. **Not yet confirmed with a position** — first step of any work here
  should be a failing test (a position where the only defence has < 1 % policy).
- Fix direction: a node with cut edges may lower `bound.upper` only to a non-mate value (keep
  "proven lost" only when `numEdges == numMoves`), or disable the cut on defender nodes while VCN
  is on. Store "was cut" in a spare bit, not a new field (SEARCH-08 keeps `Node` at 56 bytes).

### 4.2 MCTS proves less than AB per node

AB at VC4 runs a dedicated VCF search; MCTS reaches a proof only when every reply below has been
expanded and proven, one playout at a time, with expansion delay (`mcts_expand_min_visits`) and
the closed frontier (SEARCH-03) both slowing it. For short forced lines (VC4, most VC3) AB will be
much faster to a proof. MCTS could be better at deep VC2 positions where AB's horizon is the
limit (VCN: "VC2 is depth-limited ... no proof"), because its estimate improves with time — this
is the only case where MCTS-VCN has a clear reason to exist, and it is unmeasured.

### 4.3 Silent wrong answer today

With `searcher = mcts`, `YXVCN` / `INFO VCTHREAD` silently run ordinary MCTS. This is a surface
bug regardless of what is chosen below.

## 5. Options

| Option | What | Cost | Risk |
|---|---|---|---|
| **A0 — guard** | When `vcnMode.enabled()` and the searcher is MCTS: `ERRORL` + refuse (or fall back to an ordinary search with a clear message). | Small, class C. | None. |
| **A — delegate to AB** | `YXVCN` / `INFO VCTHREAD` search runs on an AB searcher even when MCTS is configured. `SearchEngine::setupSearcher` (`search/searchengine.cpp:285`) moves the new searcher in (the MCTS searcher and its graph are destroyed) and re-creates all threads, so swapping per command loses the graph; the alternative is a second, persistent `ABSearcher` held by the engine for VCN. **Also:** there is one global `TT` (`search/hashtable.h:94`); MCTS resizes it to its small VCF slice (`search.cpp:1026`), so an AB VCN search under an MCTS config would run with that small TT unless it is resized around the VCN search. | Medium (engine owns two searchers; TT sizing between AB and MCTS). | Low: VCN semantics already tested in AB. |
| **B — native MCTS-VCN** | §3.1–3.5: key XOR, level through recursion, pass prior, VC4/VC5 terminals, VC4 VCF solver shared with AB, sound bounds (§4.1), draw = attacker loss. | Large, class B (+C). Touches `evaluateNode`, `expandNode`, `searchNode`, `setupRootNode`, `SimpleVCF`, `wincheck.h`, `movepick` (prior), `node.cpp` (bounds). | Medium–high: proof soundness, graph reuse, bench must stay bit-identical with VCN off. |
| **C — hybrid** | Native MCTS for VC2/VC3 tree, every VC4 node solved at evaluation by the shared AB VCF solver (terminal at once). | B minus most of the VC4 tree work; same §4.1 requirement. | Medium. |

## 6. Recommendation

1. **Do A0 now-ish** (when asked): it fixes a silent wrong answer at near-zero cost.
2. **Before B or C, test §4.1** on ordinary MCTS. If the false-mate case reproduces, it is a bug in
   its own right (separate todo, likely `SEARCH-09`), and B/C cannot be correct without it.
3. **Prefer A over B/C** unless there is a measured reason: AB already implements and tests VCN,
   and most VCN questions are short forced lines where AB proves faster. Choose C (not B) only if
   a measurement shows MCTS finding VC2 wins AB misses at the same time budget — that measurement
   can be done cheaply with an AB-vs-"MCTS ordinary search on the post-pass position" comparison
   before any MCTS-VCN code exists.

## 7. Draft slices and test list (if B or C is chosen)

Written per CLAUDE.md rules 2-3; names follow `<function>_<scenario>_<expectedBehavior>`.

Slice 0 — guard (A0)
- `yxvcn_mctsSearcher_errorsAndDoesNotSearchAsVcn` — refuse: `ERRORL`, no search, persistent
  level untouched (an ordinary result to an explicit VCN query is the §4.3 bug again)
- `infoVcthread_mctsSearcher_warns` — warn, keep the level (it takes effect again under AB)
- `infoSearchType_mctsWithVcnLevelSet_warns` (added 2026-11-08: closes the VCTHREAD-then-
  `SEARCH_TYPE mcts` order, where neither guard above fires). `think()` itself does not refuse:
  a GUI `TURN` with no move would hang, so a persistent level under MCTS stays an ordinary search
  after the warning.

Slice 1 — sound bounds (§4.1, applies with VCN off too)
- `updateStats_allKeptChildrenLostButEdgesCut_noProvenLoss`
- `updateStats_allChildrenLostNoCut_provenLoss` (current behaviour kept)
- position test: only defence has policy < 1 %, MCTS must not print `+M`

Slice 2 — key and root identity
- `mctsVcnKey_vcnOff_equalsZobrist` (bench bit-identical guards this too)
- `setupRootNode_samePositionVcnToggled_rebuildsRoot`
- `mctsVcn_afterYxvcn_nextOrdinarySearchReusesNoVcnNode`

Slice 3 — pass edge
- `expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior`
- `expandNode_vcnDefenderFacingFive_noPassEdge`
- `expandNode_vcnAttacker_noPassEdge`
- `expandNode_vcnOff_edgesUnchanged`

Slice 4 — VC4 / VC5 terminals (shared VCF solver)
- `evaluateNode_vc4AttackerDoubleThreeOnly_attackerLoses` (VCN-02 1b)
- `evaluateNode_vc4AttackerFarVcf_attackerWins` (VCN-02 1)
- `evaluateNode_vc4DefenderNoFiveThreat_defenderWins`
- `evaluateNode_vc5AttackerNoFive_attackerLoses`
- `evaluateNode_vcnBoardFull_returnsDrawLikeAb`
- `evaluateNode_vc5AttackerNoFiveOnFullBoard_attackerLosesNotDraw`

Slice 5 — end to end, same positions as the AB VCN tests (`tests/vcn_tests.cpp`)
- `mctsVcn_vc2/vc3/vc4_openThreeAttacker_mates`
- `mctsVcn_vc4_attackerWithoutVcf_loses`
- `mctsVcn_vc3_attackerWithoutThreat_defenderPassRefutes`

Measurement (rule 9): bench bit-identical (`17621937` / `8c1f72e5`, built-in config is alpha-beta,
so also an MCTS fixed-node golden output with VCN off); AB vs MCTS on the VCN-02 position set and
`h8j9f6i10` at equal time: proven / correct / wrong counts, time to proof.

## 8. Decisions (user, 2026-11-08)

1. Option: **B** (native MCTS-VCN), decided by the user 2026-11-08. No implementation started.
   Slice order: 0 guard (A0) → 1 SEARCH-09 → 2 key and root identity → 3 pass edge and prior →
   4 VC4/VC5 terminals with a VCF solver shared with AB (factored out of `search/ab/search.cpp`)
   → 5 end-to-end on the `tests/vcn_tests.cpp` positions. Measurement as in §7.
2. §4.1 opened as its own todo: [SEARCH-09](SEARCH-09-mcts-bound-under-edge-cut.md). B depends on it.
3. Board full / maxMoves under VCN = **draw** (`getDrawValue`), the same as AB. The
   `evaluateNode_vcnBoardFull_attackerLoses` test in §7 becomes
   `evaluateNode_vcnBoardFull_returnsDrawLikeAb`; the §3.3 table row reads "draw, as AB".
   Note: VC5 "attacker without five loses" is checked **before** the draw, as in AB
   (`search/ab/search.cpp:1285`).
4. VCN mode does **not** turn off the policy-mass edge cut (user, 2026-11-09). SEARCH-10 restores
   provability; the closed-graph caveat is accepted. The TSan reports seen during SEARCH-10 are all in
   the lockless VCF TT (`search/hashtable.cpp`), pre-existing, and left as is for now (user).

## 9. Progress

### Slice 0 — guard (A0), done 2026-11-08 (class C)

- `command/gomocup.cpp`: `searcherSupportsVcn()` (alpha-beta only; slice 2+ removes this one
  check). `YXVCN` under MCTS: `ERRORL` and return before `vcnMode.n` is touched (no search, the
  `INFO VCTHREAD` level is untouched). `INFO VCTHREAD` with a level > 0 under MCTS, and
  `INFO SEARCH_TYPE mcts` while a level is set: `ERRORL` warning, level kept (it applies again
  after a switch back to alpha-beta). `think()` is unchanged: a `TURN` under MCTS with a
  persistent level is still an ordinary MCTS search, now announced by the warning.
- Tests (`tests/vcn_tests.cpp`, asserts live, red before the code — the YXVCN test failed on the
  missing MCTS message): `yxvcn_mctsSearcher_errorsAndDoesNotSearchAsVcn`,
  `infoVcthread_mctsSearcher_warns`, `infoVcthread_alphabetaSearcher_noMctsWarning` (guard does
  not fire under AB), `infoSearchType_mctsWithVcnLevelSet_warns`. `rapfi_tests` 140/140 pass.
- Bench (gcc 15.2.0, Release, AVX512+VNNI, built-in config): Nodes `17621937`, Hash `8c1f72e5`,
  unchanged (protocol-only change).
- Known gap: a config reload (`reloadConfigEachMove`) or a config file that selects MCTS does
  not go through the `SEARCH_TYPE` handler, so it does not warn; the search is still ordinary
  MCTS, as before this slice.

### Slice 2 — node key and root identity, done 2026-11-09 (class B, bit-identical with VCN off)

Instruction: [`../instruction/VCN-03-mcts-vcn.md`](../instruction/VCN-03-mcts-vcn.md) "Slice 2".

- `search/vcn.h` (new): `namespace Vcn` (`levelIncrement`, `childLevel`, `vc5AttackerLoses`,
  `hashXor`) moved verbatim out of the anonymous namespace of `search/ab/search.cpp`, now
  `Search::Vcn`; the AB call sites are unchanged (found through `using namespace Search`). Added
  to `HEADERS`.
- `search/mcts/searcher.h`: `mctsNodeVcnLevel(board, options, rootDefenderPasses)` =
  `min(VC5, n + passMoveCountOfSide(~attacker) - rootDefenderPasses)` and
  `mctsNodeKey(board, options, rootDefenderPasses)` = `zobristKey()` when VCN is off (early
  return, so the identity is structural), else `zobristKey() ^ Vcn::hashXor(options, level)`.
  `MCTSSearcher::scratch.rootDefenderPasses` (set in `searchMain` before `setupRootNode` and before
  `runOnAllThreads`) and `graph.previousRootKey` (next to `previousPosition`). `Node` stays 56
  bytes; no recursion parameter.
- `search/mcts/search.cpp`: the helper at every key site — child allocation in `searchNode`, root
  allocation in `setupRootNode`, the `node.getHash()` assert in `searchNode` (moved below the
  `options`/`searcher` locals it now needs). The root-reuse early return also requires
  `rootKey == graph.previousRootKey`. Re-grep: no other `zobristKey()` / `allocateOrFindNode(`
  key site under `search/mcts/`; the remaining `zobristKey()` uses (`SimpleVCF`, `search.cpp:115`
  and the prefetch at 220) are the VCF TT, see below.
- Decision, recycle condition: **unchanged** (no key test). A VCN search on the same moves as the
  previous ordinary search rebuilds the root and, since `rootPosition.size() >=
  previousPosition.size()`, recycles from the VCN root: the ordinary graph is unreachable and is
  erased at once (not aged out later). Toggling back rebuilds the ordinary root from scratch.
  Correct in both directions (keys never mix); the cost is lost ordinary work after a VCN query on
  the same position. Skipping the recycle when only the key changed would keep that work; it is a
  behaviour change, to be weighed in slice 5 when YXVCN under MCTS is reachable. With VCN off the
  condition is untouched, so bit-identity holds by construction.
- Finding, PASS and zobrist (verified in `game/board.cpp` `Board::move`): a pass leaves
  `currentZobristKey` unchanged and only flips `currentSide`; `zobristKey()` =
  `currentZobristKey ^ zobristSide[currentSide]`, so a pass changes the key only through the
  side-to-move term. Same stones + same side to move with different pass counts collide on the
  zobrist key alone; in VCN mode the per-node level in the XOR separates them (the level comes from
  the defender pass count, not from the key).
- Deferred to slice 4: the `SimpleVCF` VCF TT key (`search/mcts/search.cpp:115`,
  `board.zobristKey()`) is **not** VCN-keyed in slice 2. Its VCN semantics (`vcfOnly`) belong to
  slice 4; until then a VCN search under MCTS (unreachable through the protocol) shares VCF TT
  entries with ordinary search.
- Tests (names from the instruction file, which refine the §7 draft names
  `mctsVcnKey_vcnOff_equalsZobrist` / `mctsVcn_afterYxvcn_...`; `tests/mcts_vcn_tests.cpp`, new, registered in `TEST_DRIVER_SOURCES` and
  `tests/testmain.cpp`): `mctsNodeKey_vcnOff_equalsZobrist`,
  `mctsNodeKey_vcnOn_differsByLevelAndAttacker`, `mctsNodeKey_defenderPassRaisesLevel_cappedAtVc5`,
  `setupRootNode_samePositionVcnToggled_rebuildsRoot` (root compared by key only: the recycled
  ordinary root may be reallocated at the same address),
  `mctsVcn_afterVcnSearch_nextOrdinarySearchReusesNoVcnNode` (walks the VCN graph right after the
  VCN search, then the ordinary graph after the next ordinary search, replaying edge moves on a
  `Board`). `vcnHelpers_movedToSharedHeader_abBehaviourUnchanged` is covered by
  `tests/vcn_tests.cpp`: 38/38 pass.
- Fail-first (`build/vcn03s2/tests_failing_first_stub.txt`): against HEAD the file would not
  compile (`mctsNodeKey`, `search/vcn.h` absent; shown by inspection, not built). With the helpers stubbed (key = zobrist, level =
  root level) and not applied at the sites: `vcnOff` passes (expected), `differsByLevelAndAttacker`,
  `defenderPassRaisesLevel`, `samePositionVcnToggled` (VCN root hash == zobrist) and
  `afterVcnSearch` (VCN root hash == zobrist) fail. The ordinary-graph half of the last test passes
  vacuously before the change; its VCN-graph half is the real check.
- Results: `rapfi_tests` 166/166 (161 before + 5 new), asserts live (`--check-asserts-live`
  aborts). Bench (gcc 15.2.0, Release, AVX512+VNNI, built-in config): Nodes `17621937`, Hash
  `8c1f72e5`, unchanged. MCTS fixed-node golden with VCN off (4 Standard positions, 300K nodes, 1
  thread, final root block, harness copied from `build/search10/`): identical to the HEAD baseline
  (`bce9ffa`) for both `cfg_off` (cut off, the SEARCH-09/10 golden; the baseline also matches
  `build/search10/golden_ab_*`) and `cfg_on` (default 0.99 cut). Outputs in `build/vcn03s2/`.
  No self-play (VCN off is bit-identical; the golden is the guard).
- The in-engine assert `node.getHash() == mctsNodeKey(...)` in `searchNode` is live in
  `rapfi_tests` and ran on every playout of the two VCN searches in the engine tests. Only single
  thread is exercised; worker visibility of `rootDefenderPasses` rests on the `runOnAllThreads`
  happens-before.
- The protocol guard `searcherSupportsVcn()` is unchanged; tests drive the searcher directly with
  `SearchOptions::vcnMode` and set the attacker themselves.

### Slice 3 — defender pass edge and prior, done 2026-11-09 (class B, bit-identical with VCN off)

Instruction: [`../instruction/VCN-03-mcts-vcn.md`](../instruction/VCN-03-mcts-vcn.md) "Slice 3".
No deviation from its design.

- `search/mcts/search.cpp`: `generateNodeMoves(board, options, moves, policies)` (anonymous
  namespace) is the one move generation of a non-root node, called by `expandNode<false>` (then
  `createEdgesFrom` with the cut limits, which is what `Node::createEdges` did) and by `widenNode`
  (then `replaceEdgesWithFull`, unchanged). Pass rule = AB's (`search/ab/search.cpp:1450`): VCN on,
  side to move != attacker, `!p4Count(attacker, A_FIVE)`; it sets the `MAIN` picker's
  `generatePassMove`. The picker's own checks (no pass stage against a five, no pass after a pass)
  are untouched. Root expansion keeps the `ROOT` picker (never passes). `vcnAttackOrder` is not
  used. With VCN off (and at attacker / facing-five nodes) the helper runs today's collect loop
  verbatim and returns; the pass branch is a separate loop, so the hot path does no extra work
  per move.
- Prior: `applyVcnPassPrior` sets `P(pass) = mcts_vcn_pass_prior`, scales every board move by
  `1 - prior` (sum stays about 1), puts the pass after the board moves and `std::stable_sort`s the
  whole list by descending policy. The cut (`selectEdgeCount` / `createEdgesFrom`) runs after
  that, not special-cased.
- Config: `SearchCfg.mctsVcnPassPrior` (`search/searchconfig.h`), key `mcts_vcn_pass_prior`, parsed
  next to `mcts_min_edges` in `config.cpp`, clamped to `[0, nextafter(1, 0)]`; negative and NaN
  give 0. **Default 0.25 is not measured**; slice 5 measures it and may tune it.
- Ordering check (instruction: "check first"): the picker's board moves are **not** fully sorted
  by normalised policy, in `MAIN`, `DEFENDFOUR` and `DEFENDB4F3` alike. All three use
  `scoreAndSortMoves` -> `fastPartialSort(limit 0, PolicyComparator)`; for `n <= MAX_MOVES / 4`
  (121 on the 22-cell max board, so every defend list and most `MAIN` lists) the insertion branch
  orders only the moves with `score >= 0` (score = clamped raw policy score, negative for
  negative logits); the tail is in generation order. So the whole list is re-sorted (the
  instruction's fallback), only when a pass was generated. The same is true of ordinary MCTS
  today (VCN off); not changed here (bit-identity) — a separate issue in the picker/sort layer,
  for its own todo if wanted. Evidence: the test `..._edgesSortedByPolicyDescendingAndSumToOne`
  (`MAIN` stage) asserts as a precondition that the picker's own list is unsorted under a
  negative-logit stand-in policy; for `DEFENDFOUR` / `DEFENDB4F3` the claim is from code reading
  (same sort call), and `..._FacingOpenThree_passEdgeAndSortedDefendList` checks the result there
  is sorted. That the real network gives negative scores rests on the code comment near
  `vcnThreatBonus` (about -111..387), not on a measurement.
- Prior 0: the pass edge is **kept and placed last** (policy 0, board policies unscaled, bit-equal
  to the ordinary ones). Reason: the move set stays AB's. With no pass edge, MCTS-VCN could prove
  an attacker win that AB refutes by a pass (unsound); with the pass last at prior 0, a defender
  node may only stay unproven longer (sound). Since the pass is appended after the board moves
  before the stable sort, it sorts behind board moves of equal (zero) policy.
- Renju: the defender's pass does not interact with the forbidden-point filter (the pass stage
  returns before the board moves are generated) nor with `validateOpponentCMove` (stage choice
  only); covered by a Renju test with black defending next to a forbidden 3x3 point.
- Test exposure: the helper stays in the anonymous namespace (no linkage change on the hot path)
  and is reached through the existing test-forwarder header `search/mcts/searchtesting.h`
  (`Search::MCTS::Testing::generateNodeMoves`, same pattern as the SEARCH-08 forwarders). Tests
  bind a board to the engine's main thread after installing a stand-in evaluator on it (a bare
  `Board` has no thread, so the picker would fall back to classical policy). `widenNode` stays
  internal: the widen test drives its step (helper output -> `replaceEdgesWithFull`) on a cut node
  built from the helper output; `widenNode` calls the helper by construction.
- Tests (`tests/mcts_vcn_tests.cpp`, asserts live): `config_mctsVcnPassPrior_parsedAndClamped`,
  `expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior`,
  `expandNode_vcnDefender_edgesSortedByPolicyDescendingAndSumToOne`,
  `expandNode_vcnDefenderFacingFive_noPassEdge`,
  `expandNode_vcnDefenderFacingOpenThree_passEdgeAndSortedDefendList` (extra, `DEFENDFOUR` stage),
  `expandNode_vcnAttacker_noPassEdge`,
  `expandNode_vcnOff_edgesUnchanged` (against a frozen copy of the pre-slice picker loop, both
  stand-in policies), `expandNode_vcnDefenderPassPriorZero_passLastOrAbsentAndBoardPoliciesUnscaled`
  (pins "pass last"), `expandNode_vcnDefenderRenjuBlack_passEdgeAndNoForbiddenMove` (extra, Renju),
  `widenNode_vcnDefenderCutNodeWithPassEdge_widensAndKeepsPass`,
  `searchNode_vcnDefenderPass_childKeyHasRaisedLevel` (2000-node VC3 search; asserts at least one
  PASS child, each with key `mctsNodeKey` at a raised level, `zobrist ^ hashXor(VC4)` after one
  pass). `RAPFI_TEST_SKIP` now takes a comma-separated list (used by the fail-first driver).
- Fail-first (`build/vcn03s3/tests_failing_first_stub.txt`, driver `failfirst.sh`): with the
  forwarder stubbed to today's generation and the config field present but not parsed, 7 of the
  first 10 fail (config: 0.4 not read; prior, sorted, prior-0, Renju: no pass; widen: no pass kept;
  searchNode: no PASS child). `facingFive`, `attacker` and `vcnOff` pass before and after, as
  expected (they guard the unchanged cases). The `FacingOpenThree` test was added after the stub
  run; it asserts a pass the stub never generates (fail-first by inspection, not run).
- The widen test drives `widenNode`'s step, not `widenNode` itself, so the in-engine debug assert
  "every kept move is regenerated" has not yet run with a kept PASS edge.
- Results: `rapfi_tests` 177/177 (166 + 11 new, `tests/vcn_tests.cpp` included), asserts live
  (`--check-asserts-live` aborts, `build/vcn03s3/asserts_live.txt`). Bench (gcc 15.2.0, Release,
  AVX512+VNNI, built-in config): Nodes `17621937`, Hash `8c1f72e5`, unchanged. MCTS fixed-node
  golden with VCN off (harness from `build/vcn03s2/`, 4 Standard positions, 300K nodes, 1 thread,
  final root block): identical to branch HEAD `4149332` for `cfg_off` (cut off) and `cfg_on`
  (0.99 cut), 8/8; the HEAD baseline also equals the slice-2 golden. Outputs in `build/vcn03s3/`.
  No self-play (VCN off is bit-identical; VCN on is not reachable from the protocol).
- Not in this slice: terminals / `SimpleVCF` / VCF TT key (slice 4; a VCN search under MCTS now
  plays PASS through `quickWinCheck` and `SimpleVCF` unchanged, and the engine-driven tests ran
  without an assert), protocol guard and PV printing of PASS (slice 5).

### Slice 4 — VC4 / VC5 terminals, done 2026-11-09 (class B, bit-identical with VCN off)

Instruction: [`../instruction/VCN-03-mcts-vcn.md`](../instruction/VCN-03-mcts-vcn.md) "Slice 4".

- **Deviation from §8.1 (the user may overrule it).** §8.1 said the VC4 solver would be shared with
  AB, factored out of `search/ab/search.cpp`. It is not: AB's `vcfsearch`/`vcfdefend` are tied to
  AB's stack, node types, PV, eval stand-pat, policy tracing and timing. MCTS reuses the same
  primitives with the same flags instead: `quickWinCheck(..., vcfOnly)`, the QVCF picker with
  `forceAllowB4InVCF = fullBoardVCF = vcfOnly`, and AB's values (`mated_in(ply + 2)` for a VC4
  attacker with no VCF, `mate_in(ply + 1)` for a VC4 defender facing no five, `mated_in(ply)` for a
  VC5 attacker with no five). `search/ab/search.cpp` is not edited. The AB cross-check below is the
  guard that the two copies agree.
- `game/wincheck.h`: the runtime wrapper `quickWinCheck(Rule, ...)` has a defaulted
  `vcfOnly = false` after `beta` and passes both through. Its default beta
  (`VALUE_MATE_IN_MAX_PLY`) differs from the template's (`VALUE_INFINITE`), and it gates the
  deeper Renju checks. So every new call passes `beta` explicitly; existing calls are unchanged.
- `search/mcts/search.cpp`, `SimpleVCF`: `vcfsearch`/`vcfdefend` are templated on
  `bool VcnOn`. `vcf(rule, board, ply)` is the `VcnOn = false` instantiation and runs today's
  code: the same `quickWinCheck` arguments, the stand-pat at 0, picker flags false, and the
  zobrist key. `vcf(rule, board, ply, VcnContext)` is the VCN mode. `VcnContext {level, attacker,
  keyXor}` is computed once at the entry, since the level is constant along a VCF chain. AB's rules
  are decided **per node** from the side to move, as AB does, not once per call. This matters:
  inside a rule-6 chain (a VC4 defender facing a five), the attacker node after the block must be
  fours-only too. A per-call flag would run it plain and store a plain result under the VC4 key.
  - Attacker node at VC4: `vcfOnly`. That means `quickWinCheck(..., vcfOnly)`, the QVCF picker
    with both flags, and no stand-pat: the start value is `mated_in(ply + 2)`, and a missing VCF
    returns it.
  - Attacker node at VC5 with no five: `mated_in(ply)`, before the draw check (AB `:2106`).
  - Defender in `vcfdefend` at VC4 facing no attacker five: `mate_in(ply + 1)`. This mirrors AB
    `:2372-2377` and is a **small deviation from the instruction's "defender nodes unchanged"**. It
    is reachable when the VC4 attacker's block of a defender five is not a four (the QVCF picker's
    DEFENDFIVE stage yields any block). The defender's blocking of a five is unchanged, and a
    forbidden Renju block still loses.
  - Entry: a VC4 attacker always enters `vcfsearch`, even when the defender has a five, as AB's
    `search()` does (`:1228`). The picker yields the block, which must continue the VCF.
- **VCF TT key**: in VCN mode, `zobristKey() ^ Vcn::hashXor(options, level)` with the entry level,
  for store, probe and the `vcfdefend` prefetch. VCN off: exactly `zobristKey()`. VCF-only results
  never meet plain results. VC2/VC3 VCN results are separated from ordinary ones too.
- `evaluateNode<false>`, rule order (VCN on only; the level is `mctsNodeVcnLevel` with
  `scratch.rootDefenderPasses`, read through the searcher as `widenNode` does; VCN off reads
  nothing new):
  1. VC5, attacker to move, `Vcn::vc5AttackerLoses`: terminal `mated_in(board.ply())`.
  2. Board full / maxMoves: draw (unchanged; after rule 1, as AB).
  3. `quickWinCheck(rule, board, ply, VALUE_MATE_IN_MAX_PLY, vcfOnly)`, with
     `vcfOnly = attacker to move && VC4`, then the maxMoves clamp (unchanged).
  4. VC4, defender to move, `!p4Count(attacker, A_FIVE)`: terminal `mate_in(board.ply() + 1)`.
  5. VC4, attacker to move: terminal = `SimpleVCF::vcf(..., VcnContext)`. It is always terminal,
     never the network value. The value is the VCF result as returned. With no stand-pat, that is
     `mated_in(ply + 2)` when there is no VCF, or a mate when there is one. The one exception is a
     chain that reaches the board-full / maxMoves draw, where it is that draw value, as AB returns.
     **This is a small deviation from the instruction's wording ("otherwise mated_in(ply + 2)")**,
     made to match AB in that corner.
  6. Otherwise (VC2/VC3, a VC4 defender facing a five, a VC5 defender): the VCN-mode
     `SimpleVCF::vcf`, then the network value. With VCN off, today's `SimpleVCF::vcf`.
  A VC4 defender facing a five now resolves by the block line. If the attacker has no VCF after the
  forced block, the defender wins at `mate_in(ply + 3)` (AB's value); before this slice it fell
  through to the network value. The root (`evaluateNode<true>`) is unchanged.
- Test exposure: the `search/mcts/searchtesting.h` forwarders `Testing::evaluateNode` (the
  `evaluateNode<false>` instantiation) and `Testing::simpleVcf(board, options, level, ply)`.
  Tests build a `Node` directly, set `scratch.rootDefenderPasses` themselves, and bind the board
  to the main thread with the stand-in evaluator, as in slice 3.
- Tests (`tests/mcts_vcn_tests.cpp`, asserts live; fixtures are copies of the `tests/vcn_tests.cpp`
  positions): `quickWinCheckRuntime_vcfOnlyFlag_matchesTemplate`,
  `evaluateNode_vc4AttackerDoubleThreeOnly_attackerLoses`, `evaluateNode_vc4AttackerFarVcf_attackerWins`
  (`farVcfAfterForcedPair`), `evaluateNode_vc4AttackerNoVcf_attackerLosesNotNnValue`,
  `evaluateNode_vc4DefenderNoFiveThreat_defenderWins`,
  `evaluateNode_vc4DefenderFacingFive_notTerminalByPassRule` (`!= mate_in(ply + 1)`, `== mate_in(ply
  + 3)` by the block line), `evaluateNode_vc4AttackerBlocksFiveWithNonFour_attackerLoses` (the VC4
  attacker must block a defender five and the block is not a four: `mated_in(ply + 2)`, not a draw;
  this pins the `vcfdefend` mirror and rule 5 together), `evaluateNode_vc5AttackerNoFive_attackerLoses` (VC4 root plus one
  defender pass, so the level is derived, not set), `evaluateNode_vcnBoardFull_returnsDrawLikeAb`
  (via maxMoves), `evaluateNode_vc5AttackerNoFiveOnFullBoard_attackerLosesNotDraw`,
  `evaluateNode_vc3_unchangedFromOrdinaryRules` (4 positions, VC3 vs VCN off: same terminal flag,
  value and utility), `evaluateNode_vcnOff_unchanged` (double three, quiet, facing a five,
  maxMoves, Renju white four-three; the Renju case passes either way, because SimpleVCF also finds
  the win, so the wrapper's beta is pinned in `quickWinCheckRuntime_*`: `quickWinCheck(RENJU, ...)`
  with and without `vcfOnly` reports the four-three, while the template's default beta skips it),
  `simpleVcf_vcfOnlyMode_ttKeySeparatedFromPlain` (a planted wrong "win" under the zobrist key does
  not reach the VC4 VCF, and one under the VC4 key does not reach the plain VCF but does reach the
  VC4 VCF), `evaluateNode_vcnLeaves_matchAbSignAndMateClass` (AB cross-check, below), and
  `mctsVcn_smokeVsAb_noMateAbRefutes` (smoke run, below; it asserts no proven MCTS win that AB
  refutes).
- Fail-first (`build/vcn03s4/tests_failing_first_stub.txt`, driver `failfirst.sh`). The forwarders
  were stubbed to today's `evaluateNode` and the plain `SimpleVCF::vcf`, and the wrapper took
  `vcfOnly` without passing it on. 10 slice-4 tests failed: `quickWinCheckRuntime`, the 4 VC4
  attacker and defender tests, `facingFive`, both VC5 tests, the TT-separation test, and the
  cross-check. 3 passed before and after, as expected: `vcnBoardFull_returnsDrawLikeAb`,
  `vc3_unchanged` and `vcnOff_unchanged`. They guard the unchanged behaviour. The smoke test did
  not give a real fail-first: on the stub it aborted on the unrelated `ROOT` picker assert (see
  the finding below), and its final cases, after the forced pair, were never run against the stub.
  `..._BlocksFiveWithNonFour_attackerLoses` and the Renju pin were added after the stub run.
  Fail-first for the first was run by disabling only the `vcfdefend` VC4 mirror: the test aborts
  on the value (`build/vcn03s4/failfirst_nonfourblock_mirror_removed.txt`). The Renju pin is by
  inspection: it fails if the wrapper drops its own beta.
- AB cross-check (attacker-to-move leaves; AB = alpha-beta search driven directly at the same
  position and level, depth 8, built-in evaluation; MCTS = `evaluateNode` on that position as a
  non-root node). AB values are relative to its root (ply 0) and MCTS values to the game start
  (`board.ply()`), so sign and class are compared, not plies. The rule-4 defender leaves are
  covered through their parent: AB's `quiet VC4` loss comes from exactly that rule at its
  children.

  | Leaf | Level | AB | MCTS `evaluateNode` | Class |
  |---|---|---|---|---|
  | farVcfAfterForcedPair | VC4 | +29995 | +29971 | win = win |
  | doubleThreeAfterForcedPair | VC4 | -29998 | -29980 | loss = loss |
  | doubleThreeAfterForcedPair | VC3 | +29995 | +29977 | win = win |
  | quiet | VC4 | -29998 | -29996 | loss = loss |
  | openThree | VC4 | +29997 | +29991 | win = win |
  | quiet | VC5 | -29998 | -29998 | loss = loss |
  | openThree + black open four (five on board) | VC5 | +29999 | +29991 | win = win |
  | whiteFour, black must block with a non-four | VC4 | -29998 | -29990 | loss = loss |

- VCN-on smoke run through the searcher (not the protocol; `MctsEngineFixture`, 1 thread, the
  deterministic **stand-in** `HashEvaluator`, not the real network; Freestyle; default `mcts_*`
  config). It shows the root result next to AB's answer at the same position and level. No MCTS
  `+M` is refuted by AB. 200K nodes (`build/vcn03s4/smoke_200k.txt`; the in-suite run uses 20K and
  gives the same classes):

  | Root | Level | AB | MCTS best move, value | MCTS root bound | Time |
  |---|---|---|---|---|---|
  | doubleThreeAfterForcedPair | VC4 | -29998 (loss) | -29998 | [-29980, -29980] proven loss | 32 ms |
  | doubleThreeAfterForcedPair | VC3 | +29995 (win) | (5,5) +29995 | [29977, 29977] proven win | 29 ms |
  | farVcfAfterForcedPair | VC4 | +29995 (win) | (3,6) +29995 | [29971, 29971] proven win | 20 ms |
  | farVcfAfterForcedPair | VC3 | +29995 (win) | (3,6) +29995 | [29971, 30001] best move proven | 140 ms |
  | quiet | VC4 | -29998 (loss) | -29998 | [-29996, 30001] not proven (cut root) | 7 ms |
  | quiet | VC3 | -29996 (loss) | -29996 | [-30001, 30001] not proven | 30 ms |

  Roots that are not proven are information for slice 5, not failures. "Nodes" are root visits,
  and these trees are almost all terminals, so the 7–140 ms timings do not measure the cost of the
  full-board VC4 VCF, which runs with no cap at every VC4 attacker leaf. That cost is not measured
  (slice 5, on harder positions).
- Finding (pre-existing, not slice 4, not fixed): the `ROOT` `MovePicker` never sets `hasPolicy` on
  its forced branches (own five or flex four, opponent five), while `Node::createEdges` asserts
  `hasPolicyScore()`. So an MCTS search whose root has a five or flex four on the board aborts
  with asserts live (`rapfi_tests`, `FORCE_ASSERTION`), with VCN on or off. Release builds are
  unaffected (the policies are the one-hot marks). Observed for the opponent-five root: the smoke
  run's first draft hit it at the forced `doubleThreeAfterForcedBlock` root (VC4, on the stub).
  The own-five and flex-four branches are from code reading (`search/movepick.cpp:227-241`). The smoke roots therefore start after the forced
  pair, and the test asserts that precondition. The `MovePicker` rules are out of scope here; this
  needs its own todo if wanted.
- Known gap (kept for bit-identity): the maxMoves clamp after `quickWinCheck`, in `evaluateNode`
  and in `SimpleVCF::vcfsearch`, stays `mate_step(value, 0) > maxMoves`. That is `board.ply()`
  based and counts passes. AB uses `nonPassMoveCount() + mate_step(value, ply)`. So in VCN mode,
  with a `maxMoves` limit set, a mate within a few plies of the limit can be clamped to a draw
  early, by the number of passes in the line. That is conservative: it never produces a wrong `+M`.
  Without a `maxMoves` limit it has no effect. The instruction's pitfall ("use `nonPassMoveCount`
  consistently") is therefore not applied to the existing clamp.
- Results: `rapfi_tests` 192/192 (177 + 15 new, `tests/vcn_tests.cpp` 38/38 included), asserts
  live (`--check-asserts-live` aborts, `build/vcn03s4/asserts_live.txt`). Bench (gcc 15.2.0,
  Release, AVX512+VNNI, built-in config): Nodes `17621937`, Hash `8c1f72e5`, unchanged
  (`bench_base_gcc.txt`, `bench_s4_gcc.txt`). MCTS fixed-node golden with VCN off (harness from
  `build/vcn03s3/`, 4 Standard positions, 300K nodes, 1 thread, final root block): identical to
  branch HEAD `05f97aa` for `cfg_off` (cut off) and `cfg_on` (0.99 cut), 8/8. The HEAD baseline
  also equals the slice-3 golden 8/8. The full extracts differ only in the time-driven periodic
  prints, as between two baseline runs. Outputs in `build/vcn03s4/`. No self-play: VCN off is
  bit-identical, and VCN on is not reachable from the protocol.
- Not in this slice: the protocol guard and PV printing of PASS (slice 5), measuring the pass
  prior (slice 5), and the TSan races in the lockless TT (§8.4).
