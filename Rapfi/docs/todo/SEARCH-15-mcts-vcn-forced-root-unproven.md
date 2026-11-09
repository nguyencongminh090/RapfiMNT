# SEARCH-15 — MCTS does not prove P4 VC3 / VC2 at a forced root

**Status:** done 2026-11-09 (investigation only, no production change). Cause found: the Part C runs had a 32 MB MCTS graph budget, the graph closed after about 2.3M visits (VC3) / under 0.3M visits (VC2), and a closed graph cannot finish a proof. With enough memory MCTS proves P4 VC3 (`-M8`) and VC2 (`+M15`..`+M17`) at the unchanged defaults. Not worth a search fix, see Findings. Recorded 2026-11-09 by the user. Follow-up of SEARCH-13 (partly done).

**Class:** B (MCTS search behaviour, VCN mode only). Investigation only; no fix is recommended.

## Problem

- After SEARCH-13 the singular-root stop is skipped under VCN, so MCTS runs the full budget on
  forced roots. P4 VC3 (AB: `-M8`) and P4 VC2 (AB: `+M13`) are still unproven in the slice-5
  Part C grid (5 s / 30 s, 1 / 8 threads): values are ordinary scores (VC3 -522..-789, VC2
  730..778), with no mate bound, after hundreds of millions to billions of nodes.
- Data: `Rapfi/build/search13/partc/results_base.tsv`, `results_after.tsv` (not in git; see the
  Results section of `SEARCH-13-mcts-vcn-singular-root-stop.md`).

## Suspects (unverified)

- pass-edge prior (VCN-03 slices 2-4);
- `Node` bound propagation (proven win/loss not reaching the root);
- edge cut at 0.99 hiding the needed defence/attack move.

## Scope

1. Reproduce on P4 VC3 with a fixed node budget; dump root edges (visits, value, bound, prior).
2. Find which suspect (or another) stops the proof; write the finding with evidence.
3. Decide a fix as a separate step (decision note), then test list before code.

## Out of scope

- Any change before the cause is shown. AB remains the VCN searcher (VCN-03 conclusion), so this
  is low priority.

## Verification

- Cause identified with data. Any later fix: bench `17621937 / 8c1f72e5`, MCTS golden with VCN off
  identical, Part C zero wrong.

## Findings (2026-11-09)

All runs: 1 thread, real network (`build/search13/cfg_vcn`), HEAD `443204a`, release build `build/pbrain-rapfi` (objects
of the same build for the diagnostic), P4 = `P4-counterFourBlockWithoutFour`. Tools and raw outputs are in
`Rapfi/build/search15/` (not in git): `diag15.cpp` + `build.sh` (throw-away tree dumper, links the unmodified
engine objects; never in the repo tree), `run_vcn_mem.py` (Part C runner plus `PC_MAX_MEMORY`), `ab_L3.out`,
`ab_L2.out`, `d3_*.txt`, `d2_*.txt`, `protocol_runs.tsv`.

### 1. AB reference line (`ab_L3.out`, `ab_L2.out`, 20 s, 1 thread)

Black is the attacker (side to move), white the defender. Root: white has a four on x = 12, black's only move is the block M10.

| Level | AB result | AB nodes | PV (levels: the defender pass raises the level by one) |
|---|---|---|---|
| `YXVCN 3` | `-M8` at depth 5 | 4052 at first `-M8`, 35K at 11 ms | M10 (B, VC3) C8 (W, VC3) H8 (B, VC3, split four) G8 (W, forced block) E9 (B, VC3) Pass (W) then black at VC4 has no VCF: black loses |
| `YXVCN 2` | `+M25` first at depth 11 (25K nodes, 50 ms), `+M13` from depth 28 (1.1M nodes, 1.3 s), 3.2M nodes at 3.2 s | M10 (B, VC2) C8 (W) E9 (B) C7 (W) G7 (B) D10 (W) G9 (B) Pass (W): VC3 attacker wins |

So at VC3 the proof is: after the block, white must first stop the open three (C8 or G8), then every black move has to be refuted,
mostly by the defender pass (black at VC4 without a VCF loses). Black nodes have 211-213 legal moves (the config uses
`default_candidate_range = full_board`). They are the AND nodes of the VC3 proof; every one of those moves needs an edge, a visit and a proven child.

### 2. Walk in the MCTS tree with the default config (budget 1M / 10M / 100M visits, `d3_*.txt`)

The tree at HEAD has no bug on the line:

- The pass edge exists and is not starved: white node after M10 has `Pass` P = 0.25 (the config prior), it is visited and proven (the
  pass gives black an immediate VCF: terminal `[29985,29985]`). At the white node after E9, `Pass` has 932 visits at 1M and is a proven loss for black, bound `[-29982,-29982]` terminal.
- Bounds propagate: at ply 12 (black, after C8) 66 of 68 kept children carry `lower >= 29982`; at ply 14 (after C8 H8 G8) 191 of 204 visited children are proven.
  Terminal leaves (VC4 no VCF) are found (`evaluateNode` step 5).
- The key / level is right: every node on the line is found with `mctsNodeKey` (level 2 = VC3 before the pass, 3 after).
- The edge cut (0.99 mass) is present (`edges=68/213, cut=1` at ply 12) but is not what blocks: the node still has two unproven kept children.

First node on the line where the tree lacks what a proof needs, 100M visits: **ply 14, black to move after M10 C8 H8 G8**
(an AND node, all moves must be refuted): `edges 211/211`, 204 visited, 191 proven, **13 visited but unproven, 7 never visited**
(first unvisited edge P = 2e-5), node visits 7.2M, and the unproven children are leaves that are not expanded (their edge counts stay
at 34/211, 192/209 ... with 33, 187 unvisited edges). The same shape is at 10M (206 visited, 200 proven, 6 unproven) and 100M. Between
10M and 100M the node gained 7M visits and not one new edge or node. That is the signature of a graph that no longer accepts nodes.

The selection arithmetic at that node says the next edge should have been taken (`nextSel` -0.60 against the best explored -0.98
at 100M), so selection is not the blocker either: `selectChild(..., graphClosed)` offers no unexplored edge and `searchNode` does not expand a leaf when
`searcher.graphClosed()`.

### 3. Cause

The harness gives MCTS a **32 MB graph budget**. `default_tt_size_kb = 32768` in `cfg_vcn` is also the MCTS memory limit
(`config.cpp:213`, `setMemoryLimit`), and `run_vcn.py` sends no `INFO MAX_MEMORY`. Graph limit 30720 KB, closes at 85 %
(26112 KB, `GraphSoftLimitPercent`):

| Search | graph closes at | nodes in graph | then |
|---|---|---|---|
| VC3 | about 2.3M visits (2.2M: 70,397 nodes; 2.4M: 73,414, 26111 KB) | 73,414 | frozen up to 100M+ visits |
| VC2 | before 0.3M visits | 52,699 (26112 KB) | frozen |

After closing, no node and no edge array is added, so an AND node with unvisited moves cannot be proven, and the search spends the rest
of the budget revisiting the same nodes (21-24K visits/ms on one thread, which is why "93M-2144M nodes" never helped). The PV of the Part C
log (`M10 G8 C8 B8 J8 L12 J10 K12`) is the same as the frozen-graph dump. The SEARCH-13 expectation (proof after the stop is gone) was wrong
for this reason, not because of the stop removal. The three suspects of the todo are refuted: pass prior (works, defaults), bound
propagation (works), edge cut (present in the run that proves).

### 4. Controlled run: one change, the memory limit (`INFO MAX_MEMORY`, same 10M visits, defaults unchanged)

VC3, `d3_10M_*mb.txt` (graph in KB):

| Memory | graph nodes / KB | result |
|---|---|---|
| 32 MB (Part C) | 73,414 / 26111 | -565 (unproven) |
| 40 MB | 91,155 / 32640 | -558 |
| 48 MB | 110,639 / 39164 | -550 |
| 64 MB | 146,451 / 52219 | **-M8** |
| 96 / 128 / 256 / 512 MB | 229K / 313K / 609K / 609K nodes | **-M8** (first `-M8` at 4.4M visits with 4 GB, 2.0 s) |

VC3 4 GB, budgets 1M / 2M / 4M / 6M / 8M visits: `-469`, `-474`, `-488`, `-M8`, `-M8` (graph 254K nodes, 86 MB at 6M). Pass prior 0.25 and edge
mass 0.99 unchanged and the proof exists, so neither of them blocks.

VC2, `d2_10M_*mb.txt`, same 10M visits:

| Memory | graph nodes / KB | result |
|---|---|---|
| 64 / 128 / 256 / 320 MB | 106K..578K nodes | 705 / 697 / 681 / 675 (unproven, graph closed or at its limit) |
| 384 MB | 703,271 / 327255 | **+M17** |
| 448 / 512 MB | 831K / 960K nodes | +M17 |
| 1024 MB | 1,638,531 / 735917 | **+M15** (first `+M` at 2.6M visits as `+M17`, 17 s) |

Through the real protocol (`protocol_runs.tsv`, 20 s, 1 thread, `build/pbrain-rapfi`): VC3 32 MB `-531` (390M nodes), 1 GiB `-M8` (proof printed
at 2.4 s, 15M nodes); VC2 32 MB `770` (127M), 1 GiB `+M15` (proof at 10.2 s, 5.2M nodes).

### 5. Too deep or too narrow for visit-driven search?

No. With memory, MCTS needs about 4.4M-6M visits (VC3, AB 35K nodes) and about 2.6M visits for the first VC2 proof (AB: first `+M25` at 25K nodes, shortest `+M13` at 1.1M nodes), so MCTS needs
about 100x AB's nodes for a first proof at VC2 and about 100x at VC3. Its mate distance is longer than AB's (`+M15`/`+M17` against AB `+M13`), because MCTS proofs are not distance-minimal. The proof needs more
than a quarter of a million graph nodes (VC3) and about 0.5-1.6M nodes (VC2), which is the memory cost.

### 6. Decision: not worth a search change

- The root cause is a budget setting of the test harness plus the SEARCH-03 closed-graph design (a full graph is allowed to
  keep playing in the explored part). A GUI normally sends `INFO MAX_MEMORY`, and `MAX_MEMORY 0` means 350 MB (`gomocup.cpp`): VC3 proves, VC2 is borderline
  (needs about 330-380 MB at 10M visits).
- AB stays the VCN searcher (VCN-03), so a change that makes MCTS prove faster or in less memory has little value. A proof-number style
  searcher was already rejected there.
- Considered and not recommended (no todo created): print a one-time message when the graph closes (it would have shown this in
  seconds; class C, small, optional, only if the user wants it); stop the search or recycle old nodes when the graph is closed under VCN.
- SEARCH-13 "Remaining" is answered by this item. Part C numbers for P4 VC3/VC2 should be quoted as "32 MB graph limit"; re-measure the grid
  with `INFO MAX_MEMORY` >= 1 GiB if the full table is ever needed. Not done here.

### Verification

- `git diff --stat -- search eval game command tests` is empty (investigation only), so no bench, golden or unit tests are needed.
  Only docs (`TODO.md`, `instruction.md`, this file, a pointer in SEARCH-13) changed.
- The diagnostic (`diag15.cpp`) lives only under `build/search15/`; no diagnostic code was added to `tests/` or the repo.
- Caveat: 1-thread runs, one run each; node counts are deterministic for one thread, times are not.
