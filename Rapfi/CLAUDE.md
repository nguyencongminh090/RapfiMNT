# Project Rules — Rapfi Engine Core

Scope: this file applies to the whole `Rapfi/` engine core (search, eval, game, pattern).
Game-rule modifications follow the same process below as any other core-engine change.

## Development model: risk-tiered Agile (since 2026-08-19)

**This section replaced a strict V-Model on 2026-08-19. The heavier tiers are kept only for the
kinds of change that have historically produced silent corruption in this codebase; everything
else iterates without an up-front phase gate. See "[Why the model changed](#why-the-model-changed)"
below.**

Work is organised as small vertical slices (one defect or one coherent behaviour per slice), each
landing with its own tests and its own regression check. There is no up-front phase gate for the
whole feature. What survives from the V-Model is **risk-based**: the heavier tiers still apply,
but only to the kinds of change that historically produced silent corruption in this codebase.

### Which process a change gets

Classify by *what the change touches*, not by how big it feels or what task it belongs to.

| Class | What it covers | Required before code |
|---|---|---|
| **A — Representation** | Encoding and derived state: `game/pattern.h` bitkeys and LUTs, `emptyBB`/`onBoardBB`/`candidatesBB`, `boardCellCount`, zobrist/position identity, `DBKey`, anything a compile-time table is keyed on | Requirements note + **design doc** (`docs/design/<CODE>-<slug>.md`), including the derived-state audit below |
| **B — Search / eval behaviour** | Move ordering, pruning and reduction heuristics, eval blending, MCTS/AB policy wiring | A *decision note* in the task doc or commit message: what changed, what it is expected to do, how it will be measured. No design doc |
| **C — Surface and small fixes** | Protocol commands, ergonomics, missing legality guards, plumbing bugs | Nothing. Failing test first, then fix |

When a change spans classes, the highest class wins. A "small fix" that adjusts how a cell is
encoded is class A.

### The derived-state audit (class A only, mandatory)

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

### Definition of done — applies to every class, no exceptions

A slice is not done until all four hold. These are the safety net that makes short iterations
safe; they are not paperwork, and none of them scales with the size of the change.

1. **Unit tests, strict.** Every new or modified public/free function gets at least one dedicated,
   behaviour-named test case — see "Unit test conventions" below, which is unchanged by the move
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
   2026-09-09): **Search Bench Nodes `16733809`, Hash `4621a98a`.**
4. **Tests actually evaluated.** Confirm the test binary was built with asserts live — see
   "Verification without a UI".

### Why the model changed

The V-Model was adopted for a large, fully-knowable port. Ongoing work on an existing engine —
defects found by review, protocol gaps found by the client, heuristics tuned against measurements
— does not fit fixed sequential phases, so the phase gate was dropped for everything except
class-A encoding changes. Class A keeps its design tier because the value there was never the
sequencing; it was being forced to enumerate every consumer of an encoding before touching it.

A separate lesson that no process model would have caught: a test suite built with `-DNDEBUG`
compiles every `assert()` out and then passes while evaluating nothing. Short iterations depend
entirely on automated feedback, so the "tests actually evaluated" item in the definition of done
is checked explicitly rather than trusting a green run by default.

### What still holds from the old model

- `features/<slug>/` discussion stage (`user_story.md`, `diagram/`, `planning.md`) for a **new
  rule feature** — an addition to the game's rules, not a change to existing behaviour. Formalise
  into `docs/todo/` + `TODO.md` and `docs/instruction/` + `instruction.md` before implementation.
- The tracking files and their discipline (next section).
- Root-cause diagnosis (below) — unchanged, and more important under fast iteration, not less.

## Unit test conventions

Adapted from industry-standard practice — Google's *Software Engineering at Google* testing
chapter, Microsoft's .NET unit-testing guidance, and the ISTQB Foundation syllabus — to this
project's actual test style: hand-rolled `assert()`-based `main()`, no GTest/Catch2 (see
"Verification without a UI" for why no framework was introduced).

**These conventions are binding for feature work under the Agile model, not a legacy of the
V-Model.** They are what the "definition of done" above delegates to. Iterating in small slices
removes the up-front phase gates; it does not remove — and in fact concentrates — the requirement
that each slice ships strict, behaviour-named, per-function test cases. "We'll add tests in a
follow-up slice" is not a slice boundary, it is an unfinished slice.

- **Structure every test case as Arrange–Act–Assert (AAA).** Build the fixture/state, perform the
  one action under test, then assert. Don't interleave — a test that asserts mid-setup or takes a
  second action after its first assertion is testing two things at once, which reintroduces the
  "which one broke?" ambiguity a good test name is supposed to eliminate.
- **Name test functions for the behavior, not just the function under test.** `test_move()`
  says what's being exercised but not what's expected. Prefer
  `<function>_<scenario>_<expectedBehavior>` (e.g. `test_checkForbiddenPoint_doubleThreeOnEmptyCell_true`,
  `test_move_recursivePass_doesNotExceedCellCount`) — this is Google's "name the behavior, not
  the method" guidance combined with Microsoft's `MethodName_StateUnderTest_ExpectedBehavior`
  convention. A function with several distinct behaviors gets *multiple* named test functions,
  not one bundled test with several unrelated asserts — this is what makes the "at least one
  dedicated test case per function" rule in the definition of done concrete: it's a minimum of
  one, not a cap of one.
- **Use equivalence partitioning + boundary value analysis (ISTQB) to decide which cases to
  write, not intuition.** Partition the input space, then test one representative per partition
  plus the boundary between partitions — defects cluster at boundaries. The rolling bitkey-window
  in `move()` / `getKeyAt()` is a good example: the risk is an off-by-one at one specific line
  offset, not "patterns are broken in general". For a spatial query over the board grid, the
  partitions are: the cell itself, the 8 one-step neighbours, cells two-or-more steps away, and
  the board-edge/corner case where the neighbour computation could read outside the playable area
  (the coordinate padding makes that read safe but still wrong for the query).
- **Test isolation.** Each test function constructs (or fully resets via `newGame()`) its own
  `Board`; never rely on state a previous test function left behind. A
  hand-rolled `main()` that runs test functions in sequence against one shared global `Board` is
  exactly the kind of order-dependent setup that produces "fails only when run after test X"
  bugs — avoid it even without a framework enforcing isolation.
- **FIRST.** Tests should be **F**ast (no real waiting — self-play and `bench` regression runs are
  a separate step of the definition of done, not part of a unit test), **I**ndependent (see
  isolation above), **R**epeatable (deterministic — seed any `PRNG` explicitly, don't call
  `PRNG::nondeterministic()` in a unit test), **S**elf-validating
  (the test reports pass/fail itself via `assert()`/exit code, no manual log-reading), **T**horough
  (covers the equivalence partitions above, not just the happy path).

References: [Software Engineering at Google — Unit Testing](https://abseil.io/resources/swe-book/html/ch12.html),
[Google Testing Blog — Writing Descriptive Test Names](https://testing.googleblog.com/2014/10/testing-on-toilet-writing-descriptive.html),
[Microsoft Learn — Best practices for writing unit tests](https://learn.microsoft.com/en-us/dotnet/core/testing/unit-testing-best-practices),
[ISTQB — Boundary Value Analysis](https://istqb.org/wp-content/uploads/2025/10/Boundary-Value-Analysis-white-paper.pdf).

## Tracking files

- `TODO.md` — index of what to do, one line per item, links to `docs/todo/<CODE>-<slug>.md`.
- `docs/todo/<CODE>-<slug>.md` — task detail.
- `instruction.md` — index of execution guidance, links to `docs/instruction/<CODE>-<slug>.md`.
- `docs/instruction/<CODE>-<slug>.md` — approach, pitfalls, "don't touch this" boundaries for that
  same item.
- `docs/fix-log.md` — append-only log of bugs where an early fix touched the layer where the
  symptom was *visible* and the bug resurfaced from the layer that actually *produces* the value.

Before implementing any `TODO.md` task, read the matching `instruction.md` entry if one exists —
missing entry is fine, not every task has one.

## New requirements/tasks mid-conversation: stack, don't perform directly

A new requirement raised mid-session gets recorded (`TODO.md` + `docs/todo` line, `instruction.md`
+ `docs/instruction` line), not implemented immediately — unless explicitly requested now ("do
this now", "implement this").

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

## Current scope

- Supported rules are **Freestyle, Standard, and Renju** on a rectangular board. There is no
  board-modifier rule (no in-board walls, no teleportation); the `WALL` color in `core/types.h`
  is only the padded coordinate boundary described in `core/pos.h`.
- Any new game rule is a **new rule feature**: it goes through the `features/<slug>/` discussion
  stage and (for representation changes) a class-A design doc before implementation.
