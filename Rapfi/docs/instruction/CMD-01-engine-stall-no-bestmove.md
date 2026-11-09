# CMD-01 — execution guidance

Task: [`../todo/CMD-01-engine-stall-no-bestmove.md`](../todo/CMD-01-engine-stall-no-bestmove.md).
Background: `build/perf04/phase2/verify_raw.md` section "Anomaly"; harness `build/perf04/multi4.py`.

## Approach

1. **Reproduce before reading code for a fix.** Build a RelWithDebInfo engine (asserts optional)
   and a stress script based on `multi4.py`:
   - run 6–12 concurrent engines, 1T and 8T, with a CPU load alongside;
   - detect a stall with a per-board timeout;
   - before killing a stalled process, save all-thread backtraces, `/proc/<pid>/fd`, and the
     engine's stdout/stderr tail.
   Record the stall rate of the HEAD binary; it is the "before" number.
2. **Rule out the harness** (todo hypothesis 3) from the saved stdout of a stalled run.
3. **Read the backtraces.** Identify which thread should have printed the move and why it did not:
   - the completion callback in `command/gomocup.cpp` (`think`, `sendActionAndUpdateBoard`);
   - `protocolMutex`, `thinking`;
   - the start/stop/idle signalling in `search/searchthread.cpp` (`startThinking`, `runTask`,
     `waitForIdle`) and `external/thread-pool` usage.
4. Write the test list in the todo, then the test (failing first), then the fix.

## Pitfalls

- **Do not fix the harness alone** if the engine is at fault. A timeout-and-retry in the harness
  hides a real protocol bug.
- **Do not add sleeps or retries in the engine** to make the race rarer. Fix the ordering.
- **The protocol thread and the main search thread share `board` and `options`** under
  `protocolMutex` (see the comment at `gomocup.cpp:127`). Any change there must keep that ordering
  (ponder startup before a later stop or board reallocation).
- **Load matters.** The stall shows mostly under load. A clean run on an idle machine proves
  nothing; compare stall counts against the base binary under the same load.
- **CPU throttling** (PL1 24 W) changes timing. Record the power state with each stress run.

## Don't touch

- Search algorithms, eval, MCTS node table (PERF-03/04), defaults.
- The AB/MCTS search output: bench must stay `17621937 / 8c1f72e5`.
- The user's uncommitted files in the git root.

## Verification before done

As in the todo "Verification" section. In addition:
- If the class turns out to be B (search-thread race), record the decision note and numbers
  (hard rule 9).
- If a first fix was at the wrong layer, add a `docs/fix-log.md` entry.
