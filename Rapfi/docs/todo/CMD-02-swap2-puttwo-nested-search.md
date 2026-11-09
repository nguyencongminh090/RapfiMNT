# CMD-02 — Swap2 "put two" nested search runs outside `thinking` and `protocolMutex`

**Status:** open (recorded 2026-11-09 during CMD-01, not started).
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
