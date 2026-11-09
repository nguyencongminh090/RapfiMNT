# CMD-02 — execution guidance

Task: [`../todo/CMD-02-swap2-puttwo-nested-search.md`](../todo/CMD-02-swap2-puttwo-nested-search.md).
Background: CMD-01 (`docs/todo/CMD-01-engine-stall-no-bestmove.md`), which fixed the order of
`thinking = false` and the move print in `think`'s callback.

## Approach

1. Write a deterministic test first, in `tests/gomocup_tests.cpp` (the CMD-01 `ClientSink`
   streambuf seam can be reused to hold or observe the nested callback).
2. Keep the CMD-01 invariant: `thinking` is cleared under `protocolMutex` *before* a result is
   printed, and never while a search whose result the client waits for is still running.

## Pitfalls

- Same as CMD-01: no sleeps or retries in the engine; keep the ponder-startup ordering
  documented in `think`'s callback and the STOP handler.
- The nested `startThinking` runs on the main search thread; anything that waits for that thread
  from inside the callback deadlocks.

## Don't touch

- Search algorithms, eval, MCTS node table, defaults; bench must stay `17621937 / 8c1f72e5`.
