# CMD-01 — Engine stalls without sending a best move under a pipe harness

**Status:** in progress (recorded 2026-11-09; resumed 2026-11-09 with reduced scope, see below). Seen during PERF-04 phase 2 verification.
**Progress (paused 2026-11-09):** branch `cmd-01-stall` (from `fe37c22`), still active, no code
changed. Done: read the evidence. No reproduction has been run yet (0 stress runs, so no stall
count and no power-state record of our own; the PERF-04 run recorded PL1 24 W). The RelWithDebInfo
build in `build/cmd01/rwdi` (gcc, AVX512/VNNI, `BUILD_TESTS=ON`) was configured but stopped
before it finished. No backtraces or stress script saved yet.
Findings so far:
- The `.out` files in `build/perf04/phase2/verify/stress/` are **not** evidence of where the
  engine stopped. `multi4.py` writes them through a buffered Python file that is lost on
  `timeout` kill (bg_6/new_6 are 0 bytes; bg_1 `.out` shows 2 boards answered while its unbuffered
  `.stderr` shows `BOARD n=8` sent). "Printed part of a search's output" is a harness artifact.
- Leading hypothesis (new, not yet proven): **a race in the completion callback in
  `command/gomocup.cpp` drops the next command.** `think`'s callback prints the move (flushed)
  *before* `thinking = false`. `runProtocol` checks `thinking` without the lock and drops any
  non-STOP command while it is true (`std::cin.ignore` rest of line). A fast client that answers
  the move at once with `BOARD` can have it dropped; the following `x,y,c` / `DONE` lines are then
  parsed as unknown commands, and both sides wait on each other forever. This matches the
  signature: protocol thread in pipe read, search threads idle, no CPU. Load widens the window
  between the print and the store, because the pipe write wakes the reader.
Next step: finish the build (`ninja -C build/cmd01/rwdi pbrain-rapfi rapfi_tests`). Write a
stress script from `multi4.py` that writes the log unbuffered (flush every line), sets a timeout
per board and saves gdb `thread apply all bt`, `/proc/<pid>/fd` and the output tail. Run it with
6 to 12 engines at 1T/8T plus CPU load, and record PL1. Confirm in a stalled log that the move
line was received and the next `BOARD` got no reply (look for `ERROR Unknown command: DONE`-style
lines). Then write the test list and fix the ordering at the protocol layer.

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

## Resumed 2026-11-09: reduced scope, root cause, class

**Deviation from the instruction (hard rule 6), approved by the user 2026-11-09:** the 4 to 6 hour
stress reproduction with gdb backtraces (instruction step 1) is replaced by (a) a static trace of
the root cause, (b) a deterministic regression test that forces the ordering and fails on HEAD,
and (c) a short targeted stress run (minutes per binary) of base vs fixed under the same load,
both searchers, 1T and 8T, with PL1 recorded. No gdb backtraces of a stalled process are taken.

**Root cause (static trace, confirmed in code):** `command/gomocup.cpp`, `think`'s completion
callback. It holds `protocolMutex`, calls `sendActionAndUpdateBoard`, which prints the move with
`std::endl` (flushed to the pipe), and only then sets `thinking = false`. `runProtocol` checks
`thinking` without the lock and drops any non-STOP command (plus the rest of its line) while it is
true. A client that answers the move at once with `BOARD` can have `BOARD` dropped in the window
between the flush and the store; the following `x,y,c` and `DONE` lines then become unknown
commands, the engine waits on stdin and the client waits for a move. This is hypothesis 1/2 of
neither kind: the callback runs and the move is delivered; the *next* command is lost. Load widens
the window (the pipe write wakes the reader before the callback thread stores the flag). It is
generic to every searcher (AB, MCTS), to trivial best moves (opening DB, `tryTrivialBestmove`)
and to every `think` caller (`BEGIN`, `TURN`, `BOARD`, `YXNBEST`, `YXBALANCE*`, `SWAP2BOARD`).
`build/perf04/multi4.py` sends `BOARD` right after reading the move line, which is exactly this
pattern (hypothesis 3, a harness bug, is ruled out: the harness behaves as a fast GUI may).

**Class: C** (protocol layer, `command/gomocup.cpp`). No search-thread signalling
(`startThinking`, `runTask`, `waitForIdle`) changes, search output is unchanged.

**Fix (root-cause layer):** in the callback, clear `thinking` *before* the move is printed, still
inside `protocolMutex`. The flag is the engine's "result published" state; it must change before
the externally visible effect that lets the client send its next command. A protocol thread that
sees `false` early then blocks on `protocolMutex` until the callback (board update, time
accounting, ponder startup) is complete, so the ordering documented in the callback and in the
STOP handler is kept. Rejected alternative: re-check `thinking` under the lock in `runProtocol`.
It also closes the window, but it fixes the reader instead of the writer that publishes out of
order, and adds a mutex acquisition to the drop path. Knowledge lookup (hard rule 10):
`SKILLS_TREE/tools/find -k knowledge "publish completion signal after state update race
condition flag ordering concurrency"` returned no matching note (only PostgreSQL locking and GoF
State); the choice rests on the trace above.

**Second suspect, `Swap2PutTwo` (checked, different defect, follow-up CMD-02):** the
`Swap2PutTwo` branch of `sendActionAndUpdateBoard` starts a second (`BALANCE_TWO`) search from
inside the callback on the main search thread (`waitForIdle` skips the calling thread, so it does
not deadlock). `thinking` is then cleared while that second search runs, and its own callback
prints the two moves without `protocolMutex` and without any `thinking` coverage. That is the
flag being cleared too *early* for a nested search (commands are accepted and can stop it or
reallocate `board` mid-search), not the print-before-clear ordering fixed here, and it cannot cause
the BOARD-after-move stall. Recorded as CMD-02, not fixed here (hard rule 5).

## Test list

Written before code. File `tests/gomocup_tests.cpp`, driver `runGomocupTests()`. Seam: the test
installs a `std::streambuf` on `std::cout`; its `sync()` (called by the move's `std::endl` on the
main search thread, inside `protocolMutex`) is the deterministic interleaving point. No engine hook,
no sleeps, no timeouts; positions are immediate fives so `tryTrivialBestmove` answers without a
search (no PRNG involved).

| Function | Test | Partition |
|---|---|---|
| `think` (callback) | `think_moveFlushedToClient_thinkingAlreadyCleared` | flag state at the moment the move line is flushed: must be `false` |
| `runProtocol` | `runProtocol_boardSentRightAfterMove_isProcessed` | a `BOARD ... DONE` that arrives after the move is flushed and before the callback returns: must be processed (second move printed, no `Unknown command`) |
| `runProtocol` | `runProtocol_commandWhileThinking_isDropped` | boundary on the other side: a command read while `thinking` is true (before the result is published) is still dropped with the rest of its line (existing behaviour kept) |

The `runProtocol_boardSentRightAfterMove_isProcessed` test forces the HEAD interleaving: in
`sync()`, if `thinking` is still true, the stream blocks until the test thread's `runProtocol()`
has returned (on HEAD that is the lock-free drop path, so it cannot deadlock); if `thinking` is
already false it returns at once and the test thread's `runProtocol()` waits on the mutex.

## Verification

- The stress reproduction gives 0 stalls in a run long enough that the base binary stalls at least
  several times. State both counts.
- `rapfi_tests` pass with asserts live. Run TSan on the stress reproduction if the fix touches
  thread signalling.
- Bench `17621937 / 8c1f72e5` unchanged (the fix should not move search output).
