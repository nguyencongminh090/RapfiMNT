# SEARCH-16 — MCTS: keep the search useful after the graph memory budget is full

**Status:** open (recorded 2026-11-11 from a discussion with the user; no design chosen, no code).
**Class:** B (search behaviour on budget-limited runs; unbounded runs must stay byte-identical).

## Problem

When `graphMemoryUsed` reaches the budget, the graph closes (SEARCH-03, `searcher.graphClosed()`).
From then on, in `search/mcts/search.cpp`:

- `searchNode` expands no leaf; a visit to a leaf is credited with the leaf's stored value;
- `selectChild` offers no unexplored edge (playouts stay in explored nodes);
- `widenNode` adds no cut move back (SEARCH-10);
- `recycleOldNodes` runs only in `setupRootNode`, i.e. between searches, never inside one.

So after the close no network evaluation is added. Playouts only re-weight the existing graph:
values converge to the fixed point of that graph and then stop moving, and a wrong leaf value can
no longer be corrected (this amplified the VCN-04 error).

Measured (VCN-04 item 6, `build/vcn04/logs/vc2_30m_new.log`; Renju, 8 threads, 8 GB, 30 min asked):

| Time | Best move | Win rate | Visits | n/ms |
|---|---|---|---|---|
| 151 s | F9 | 53.78 % | 42M | 284 |
| 181 s | F9 | 53.69 % | 49M | 277 |
| 301 s | F9 | 56.45 % | 1241M | 4124 |
| 901 s | F9 | 56.64 % | 7632M | 8471 |
| 1129 s (end) | F9 | 56.64 % | 8611M | 7630 |

The graph closed between 181 s and 241 s at about 50M visits (about 160 bytes per visit). The value
was stable from about 300 s; the remaining time changed it by 0.2 points.

Reference read 2026-11-11: KataGomo (branches `Gom2026` and
`gom2026-avx2-vnni-b11-b16-b24-cpu-ptq`, identical `cpp/search/`) has no memory budget for the
tree and ignores `INFO MAX_MEMORY`; it limits the search by visits / playouts / time and collects
unreachable nodes only when the root changes. Nothing to reuse for this problem except the idea of
limiting the search instead of the memory. Knowledge lookup (SKILLS_TREE): no matching note.

## Options (none chosen)

1. **Stop when the closed graph has settled.** End the search some time after the close, when root
   values stop moving. Smallest change; saves time, adds no strength per move.
2. **Expansion threshold that rises with memory use.** `mcts_expand_min_visits` (SEARCH-02) is a
   constant; make the required visit count grow with the fill ratio (example: below 50 % as now,
   75 % 4 visits, 90 % 16 visits). No stop-the-world pass, no dangling pointer, parent statistics
   untouched; the graph slows down instead of closing.
3. **Reclaim inside a search.** When the graph closes: stop all threads at a safe point, choose a
   visit threshold from a histogram so that about 30 to 50 % of the memory is freed, mark from the
   root through edges whose child passes, erase unmarked nodes (`eraseNodesIf`, as
   `recycleOldNodes` does), reset edges that point to erased nodes to "unexplored", reopen.
   - Criterion: low visits, not low Q. Never erase proven nodes (terminal / bound) or nodes on the
     root principal variations.
   - Reversible only: an erased branch can be selected and expanded again. No permanent ban of a
     move (a banned defence at a defender node gives a false win, the VCN-04 class of error).
4. **Collapse instead of erase** (variant of 3 for large subtrees): only branches with at least N
   visits whose upper confidence bound is below the lower bound of the best sibling; drop the
   subtree, keep the branch's own node with its value and visits, so the parent's statistics do
   not change.

User's idea (2026-11-11): before deleting a branch, search it up to a visit threshold and delete
it only if its value is still low. Assessment given to the user: the intent (evidence before
deletion) is kept by option 4's "at least N visits" plus the confidence-bound test; forcing extra
visits is not recommended because it needs memory when memory is full, costs N visits for each of
tens of millions of low-visit leaves, and biases the parent's average. Not decided.

Recommendation given: try option 2 first, measure, then decide whether 3 / 4 is needed.

## Open questions (answer before a design)

- `Node::updateStats`: what happens to a parent's value when an edge's visits are reset to 0
  (option 3), and when a collapsed node is expanded again (option 4: is the stored value lost at
  once)? Not read yet.
- Cost of one mark-and-sweep pass at about 50M nodes (not measured; `recycleOldNodes` does the
  same work between moves).
- The edge is 16 bytes with no value field (`search/mcts/node.h`); a design that needs a per-edge
  value summary changes the edge layout (SEARCH-08 / PERF-03 budget math).
- How to keep unbounded runs and runs that never fill the budget byte-identical.

## Decision note (rule 9; fill in when a design is chosen)

- What: to be chosen from the options above.
- Expected: the root value keeps improving after the budget is reached; no loss before it.
- How measured: bench `17621937 / 8c1f72e5` unchanged; golden output on unbounded runs; long
  fixed-time self-play with a small memory budget against the current close behaviour; the VCN-04
  item 6 position as a timeline (value and node rate after the close).

## Test list

Not written: depends on the design. Write it here before any code.
