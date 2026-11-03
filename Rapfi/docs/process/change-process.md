# Change process: risk classes and definition of done

Detail for [`../../CLAUDE.md`](../../CLAUDE.md). Risk-tiered Agile since 2026-08-19 (replaced a
strict V-Model). Work is small vertical slices; only the kinds of change that historically
produced silent corruption keep a heavy tier.

## Which process a change gets

Classify by *what the change touches*, not by how big it feels or what task it belongs to.

| Class | What it covers | Required before code |
|---|---|---|
| **A — Representation** | Encoding and derived state: `game/pattern.h` bitkeys and LUTs, `emptyBB`/`onBoardBB`/`candidatesBB`, `boardCellCount`, zobrist/position identity, `DBKey`, anything a compile-time table is keyed on | Requirements note + **design doc** (`docs/design/<CODE>-<slug>.md`), including the derived-state audit below |
| **B — Search / eval behaviour** | Move ordering, pruning and reduction heuristics, eval blending, MCTS/AB policy wiring | A *decision note* in the task doc or commit message: what changed, what it is expected to do, how it will be measured. No design doc |
| **C — Surface and small fixes** | Protocol commands, ergonomics, missing legality guards, plumbing bugs | Nothing. Failing test first, then fix |

When a change spans classes, the highest class wins. A "small fix" that adjusts how a cell is
encoded is class A.

## The derived-state audit (class A only, mandatory)

A design that concludes "zero new branches" from a partial reading of the affected structures is
exactly how this codebase has produced silent corruption before — abort-or-alias in pattern
lookup, a guard that could never fire, TT/database key collisions. A class-A design doc must
therefore answer, in writing, for the state it changes:

1. Who reads its **cardinality**? (`cellCount`, `boardSize * boardSize`, `movesLeft`, anything
   sizing an array)
2. Who reads it as **position identity**? (zobrist, `DBKey`, symmetry reduction, TT, database)
3. Which **compile-time tables** are keyed on it, and does every key still map to a valid entry?
4. Which **derived bitboards or caches** must be rebuilt, and where is that rebuild anchored?

"Reviewed by inspection" is not an answer to these; name the call sites.

## Definition of done — applies to every class, no exceptions

A slice is not done until all four hold. These are the safety net that makes short iterations
safe; they are not paperwork, and none of them scales with the size of the change.

1. **Unit tests, strict.** Every new or modified public/free function gets at least one dedicated,
   behaviour-named test case — see [unit-tests.md](unit-tests.md), which is unchanged by the move
   to Agile and is not negotiable for feature work. Faster iteration means *more* pressure on
   these tests, not less: they are now the only phase gate left. A feature slice whose tests are
   a single smoke test is not done.
2. **Test cases enumerated up front.** The function → test-case list still goes in
   `docs/todo/<CODE>-*.md` *before* the code is written, for every class. This is the one piece of
   up-front specification the model kept, because writing it afterwards reliably produces tests
   that describe whatever the code happens to do.
3. **Regression check.** `bench` bit-identical when the feature is not active, or an explicit,
   recorded reason why the change is expected to move it. Record this project's own baseline
   (`rapfi bench` node count + hash) here the first time it is measured. A class-B change that
   *should* move it needs a measurement, not an opinion.

   Baseline (gcc 15.2.0, Release, AVX512+VNNI, `echo BENCH | ./pbrain-rapfi`, first measured
   2026-11-03, after syncing upstream 0.44.1): **Search Bench Nodes `17621937`, Hash `8c1f72e5`.**
   (Previous baseline `16733809` / `4621a98a`, 2026-09-09, applies to older todo records.)
4. **Tests actually evaluated.** Confirm the test binary was built with asserts live — see
   "Verification without a UI".

## Why the model changed

The V-Model was adopted for a large, fully-knowable port. Ongoing work on an existing engine —
defects found by review, protocol gaps found by the client, heuristics tuned against measurements
— does not fit fixed sequential phases, so the phase gate was dropped for everything except
class-A encoding changes. Class A keeps its design tier because the value there was never the
sequencing; it was being forced to enumerate every consumer of an encoding before touching it.

A separate lesson that no process model would have caught: a test suite built with `-DNDEBUG`
compiles every `assert()` out and then passes while evaluating nothing. Short iterations depend
entirely on automated feedback, so the "tests actually evaluated" item in the definition of done
is checked explicitly rather than trusting a green run by default.

## What still holds from the old model

- `features/<slug>/` discussion stage (`user_story.md`, `diagram/`, `planning.md`) for a **new
  rule feature** — an addition to the game's rules, not a change to existing behaviour. Formalise
  into `docs/todo/` + `TODO.md` and `docs/instruction/` + `instruction.md` before implementation.
- The tracking files and their discipline (next section).
- Root-cause diagnosis (below) — unchanged, and more important under fast iteration, not less.

## Root-cause diagnosis

When a bug's symptom recurs, or a fix only touches the layer where the bug is *observed* (e.g.
`movepick.cpp` ordering) without touching the layer that *produces* the value (e.g. bitkey
encoding in `setBitKey()` / `flipBitKey()`), treat that as a signal the root cause isn't found —
trace the value back to its origin before writing the fix. A mistake in how a cell's two-bit code
is written into `bitKey0..3` corrupts pattern matching only under specific line-window alignments
— undetectable by casual play-testing, only by a targeted bitkey unit test.

## Verification without a UI

Rapfi core has no browser/UI to eyeball correctness against. The acceptance check for a
core-engine change is:

0. **Confirm the tests actually run.** Test suites are `assert()`-based, and a `Release` build
   (`-DNDEBUG`) compiles every assert out — the binary then prints its `[PASS]` lines
   unconditionally and exits 0 while evaluating nothing. This bites both the test bodies and the
   engine's own invariants (`checkP4`, `isValidLineKey`). Any test target must strip `NDEBUG`
   (force `-UNDEBUG` / `/UNDEBUG`) regardless of `CMAKE_BUILD_TYPE`; do this explicitly for each
   new test target and confirm it in the task doc. Never strip it from the `rapfi` engine target
   itself.
1. **Self-play regression** — run N games before/after the change on a fixed set of
   positions/openings, diff outcomes.
2. **Targeted position tests** — hand-construct board positions that exercise the behaviour under
   change (forbidden points, pattern recognition, win detection), assert the engine's search/eval
   output matches the expected result.
3. **Board dump inspection** — `Board::trace()` ASCII output for a manual sanity-check of stone
   and pattern placement.

Never call a class-A change "done" on unit tests alone — unit tests catch plumbing bugs, not
hot-path (bitkey/pattern) correctness bugs.
