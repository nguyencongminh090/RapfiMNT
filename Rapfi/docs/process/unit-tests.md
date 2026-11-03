# Unit test conventions

Detail for [`../../CLAUDE.md`](../../CLAUDE.md). Tests live in `Rapfi/tests/`; see CLAUDE.md "Build and test".

Adapted from industry-standard practice — Google's *Software Engineering at Google* testing
chapter, Microsoft's .NET unit-testing guidance, and the ISTQB Foundation syllabus — to this
project's actual test style: hand-rolled `assert()`-based `main()`, no GTest/Catch2 (see
[change-process.md](change-process.md) "Verification without a UI").

**These conventions are binding for feature work under the Agile model, not a legacy of the
V-Model.** They are what the "definition of done" in [change-process.md](change-process.md) delegates to. Iterating in small slices
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
