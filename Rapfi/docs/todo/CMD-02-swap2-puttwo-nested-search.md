# CMD-02 — Swap2 "put two" nested search runs outside `thinking` and `protocolMutex`

**Status:** closed 2026-11-09 (user decision: option 1, close as not reachable). The `Swap2PutTwo`
branch is unreachable in this tree, see "Finding" below. No code changed; the latent defects stay
documented here for whoever makes the engine produce `Swap2PutTwo` (that would be a new class B
item, and this fix must come first).
Earlier status: open, blocked on a user decision (2026-11-09).
(Recorded 2026-11-09 during CMD-01.)
**Class:** C provisional (protocol layer, `command/gomocup.cpp`). Reclassify if the fix needs a
change in the search start/stop signalling (B).

## Problem

Found by static reading while fixing CMD-01; not reproduced.

`sendActionAndUpdateBoard` (`command/gomocup.cpp`), branch `ActionType::Swap2PutTwo`, runs inside
`think`'s completion callback on the main search thread, under `protocolMutex`. It sets
`options.swapable = false`, `options.balanceMode = BALANCE_TWO` and calls
`Search::Engine.startThinking` for a second search from inside the callback (this does not
deadlock: `SearchEngine::waitForIdle` skips the calling thread). Then:

- the callback returns and `thinking` is false while the second search runs, so `runProtocol`
  accepts any command during it (for example `INFO`, `BOARD`, `START`): they call `stopThinking`
  and can modify `options` or reallocate `board` while that search is running;
- the second search's own callback prints the two moves without `protocolMutex` and reads the
  global `board` (`board->size()`) without it, racing with a protocol command that reallocates it;
- the time-accounting and ponder-start tail of `think`'s callback runs for the first search only.

This is a different defect from CMD-01 (CMD-01: the flag was cleared *after* the move was printed;
here the flag and lock do not cover the nested search at all). It cannot produce the CMD-01
"BOARD after move" stall, because the client is still waiting for the two moves.

## Scope

1. Reproduce or show by a deterministic test (e.g. a command sent during the nested search is
   processed instead of dropped).
2. Decide the fix layer: keep `thinking` set across the nested search (clear it in the nested
   callback, under `protocolMutex`, before printing, as CMD-01 does), or run the second search as
   a separate `think` step.

## Test list

To be written before code.

## Verification

- `rapfi_tests` pass with asserts live; the new test fails first.
- Bench `17621937 / 8c1f72e5` unchanged.

## Finding (2026-11-09): the branch is unreachable, nothing produces `Swap2PutTwo`

Started on branch `cmd-02-swap2-nested` (from `955fa8a`). Before writing the test list, traced
which code can set `Search::Engine.ctx.resultAction` (the value `think`'s callback passes to
`sendActionAndUpdateBoard`). The only writers are:

| Writer | Location | Values |
|---|---|---|
| `SearchContext::reset` | `search/searchengine.cpp:195` | `Move` |
| `Opening::probeOpening` (via `tryTrivialBestmove`) | `search/opening.cpp:176-192`, called at `search/searchengine.cpp:390` | `Swap` or `Move` |
| `Opening::decideAction` (via `finalizeResult` when `options.swapable`) | `search/opening.cpp:194-213`, called at `search/searchengine.cpp:447` | `Swap` or `Move` (SWAP2: `Swap` at ply 3 or 5 if `bestValue < 0`, else `Move`) |
| `SearchEngine::finalizeResult` | `search/searchengine.cpp:449,451` | `Move2` (BALANCE_TWO) or `Move` |
| MCTS searcher | `search/mcts/search.cpp:1378` | `Move` |

Evidence that nothing else does:
- `git grep -n -e PutTwo -e 'static_cast<ActionType>' -e 'ActionType(' HEAD`: only the enum
  definition (`core/types.h:285`), the consumer branch (`command/gomocup.cpp:84`) and docs; no
  int-to-enum construction of `ActionType` anywhere.
- `git grep -n PutTwo 4538227` (initial RapfiMNT import): the same two code lines, so the gap is
  inherited, not a regression of a later change. `5fee9c0` mentions `Swap2PutTwo` only in the
  CMD-01 todo text.

So no protocol input (`SWAP2BOARD`, `BOARD` with `INFO RULE` SWAP2, opening database) can reach
the nested `BALANCE_TWO` search, and the race described above cannot occur in the shipped engine.
It is latent: it becomes real the moment a producer of `Swap2PutTwo` is added.

Consequence for this item: every verification tier asked for (a test that fails on HEAD through
`think`'s callback, a Swap2 protocol exercise, TSan on that path) needs the branch to be reachable.
A test that calls `sendActionAndUpdateBoard(ActionType::Swap2PutTwo, ...)` directly from the test
thread would bypass `think`'s callback and `protocolMutex`, i.e. the very interleaving under test,
and its "failing first" log would be evidence about a path that does not exist. Changing the scope
that way is a deviation from the instruction (hard rule 6) and needs the user's approval, as the
CMD-01 reduced scope did. Therefore no test list, test or fix was written.

Second latent issue in the same branch (static reading, same reachability caveat): the branch
returns without printing anything when `Search::Engine.isTerminating()` is true. A search that
ends by its time or node limit sets `terminate` through `SearchContext::checkExit` /
`ABSearcher` (`engine->stopThinking()`), and nothing clears it until the next `startThinking`,
so a put-two decision after a normally timed search would print no answer at all.

### Options for the user

1. **Close CMD-02 as not reachable.** No code change; the branch stays as dead code.
2. **Remove the dead branch, or make it explicitly unreachable** (e.g. `assert` plus a comment
   pointing here). Small class C change, needs approval (it was not asked for).
3. **Support the put-two choice** (make `decideAction`, or an opening database entry, return
   `Swap2PutTwo` for balanced ply-3 SWAP2 positions). That changes Swap2 play: class B and a new
   feature (hard rule 5: record as its own TODO item first). CMD-02's fix (keep `thinking` set
   across the nested search and clear it, under `protocolMutex`, in the nested callback before the
   print; move the time accounting and ponder tail to the nested callback; handle the
   `isTerminating` early return) would then be a prerequisite of that item and could be tested
   end to end through `SWAP2BOARD`.
