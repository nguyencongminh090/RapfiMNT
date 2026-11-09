# VCN-03 — VCN search mode under the MCTS searcher (research report)

**Status:** option B chosen (2026-11-08, user). Slice 0 (guard) done 2026-11-08, see §9; slices
1–5 not started. Depended on [SEARCH-09](SEARCH-09-mcts-bound-under-edge-cut.md), done 2026-11-09
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
