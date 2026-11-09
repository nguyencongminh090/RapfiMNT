# CMD-01 — Engine stalls without sending a best move under a pipe harness

**Status:** open (recorded 2026-11-09, not started). Seen during PERF-04 phase 2 verification.
**Class:** C provisional. Reclassify once the root cause is known: a race in the search threads or
in the search's start/stop signalling makes it B.

## Problem

The PERF-04 verification harness (`build/perf04/multi4.py`) drives the engine over stdin/stdout
pipes. It sends `START`, `INFO ...` (with `MAX_NODE`, `TIMEOUT_TURN 360000000`), then
`BOARD ... DONE` per step, and waits for a `x,y` line. Sometimes the engine process stops
answering:

- it has printed part of a search's output, but never prints the best move;
- the protocol thread is idle in `anon_pipe_read` (waiting on stdin); 1 or 8 search threads are in
  `futex_wait`; no CPU is used;
- the harness waits forever (runs were killed by `timeout`).

It is **not** caused by PERF-04: it hits the base binary and `build/perf03/phase2/pbrain-B-gcc`
equally. In a stress run of 6 new + 6 `pbrain-B-gcc` concurrent engines, 3 of each stalled
(`build/perf04/phase2/verify/stress/`). It was seen mostly under load (parallel builds, ASan,
12 concurrent engines), and once in an 8-thread base run on an otherwise idle machine, at the first
board. Evidence: `build/perf04/phase2/verify_raw.md`, section "Anomaly". No backtrace or core of a
stalled process was saved.

If the engine itself loses the best move, a GUI or tournament manager that drives it the same way
would time the engine out. That makes this worth a root cause, not a harness workaround.

## Hypotheses (unverified, to test in order)

1. **The search's completion callback never runs.** `think` (`command/gomocup.cpp:99`) starts the
   search with a callback (`:126`) that prints the move via `sendActionAndUpdateBoard` (`:67`) under
   `protocolMutex`. Threads that are all idle and no output fit a lost wake-up or a missed "search
   finished" path in `Search::Engine` (`search/searchthread.cpp`: `startThinking`, `runTask`,
   `waitForIdle`), or an early return (for example `isTerminating()`) that skips the callback.
2. **The callback ran but the move is not delivered.** The move goes to `std::cout` with
   `std::endl` (`gomocup.cpp:71`) without `sync_cout`, while search output goes through `sync_cout`.
   Check for interleaving or a lost line, although `std::endl` flushes.
3. **A harness bug.** `multi4.py` recognises the move as a line of two integers separated by a
   comma. Rule out a move line that is written in another form (for example run together with
   another line) and so is not recognised.

## Scope

1. Reproduce reliably: a stress script (N concurrent engines plus a CPU load, 1T and 8T) that
   detects a stall by timeout and, before it kills the process, saves `gdb -batch -ex "thread apply
   all bt"` (use a build with debug info), `/proc/<pid>/fd` and the tail of the engine's output.
2. Check hypothesis 3 against the saved output first (it is the cheapest to rule out).
3. Find the root cause from the backtraces (hard rule 4). Fix at the layer that produces it.
4. Write the test list here before code. A deterministic regression test if the race can be made
   deterministic (for example by forcing the ordering with a hook); otherwise a stress test with a
   pass criterion, kept out of the default `rapfi_tests` run if it is slow.

## Test list

To be written once the root cause is known.

## Verification

- The stress reproduction gives 0 stalls in a run long enough that the base binary stalls at least
  several times. State both counts.
- `rapfi_tests` pass with asserts live. Run TSan on the stress reproduction if the fix touches
  thread signalling.
- Bench `17621937 / 8c1f72e5` unchanged (the fix should not move search output).
