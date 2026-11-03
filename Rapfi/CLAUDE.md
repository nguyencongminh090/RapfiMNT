# Project Rules — Rapfi Engine Core

Scope: the whole `Rapfi/` engine core (search, eval, game, pattern, command). This is a derivative
of Rapfi (RapfiMNT) supporting **Freestyle, Standard and Renju** on a rectangular board, plus the
VCN attack-search mode (`INFO VCTHREAD`, `YXVCN`) and `YXANALZ`. It has **no WALL, Portal, torus or
Caro rule**: the `WALL` color in `core/types.h` is only the padded coordinate boundary
(`core/pos.h`). Do not import process language or code from sibling forks that have them.

This file is loaded every session, so it holds only **hard rules and pointers**. Detail lives in
`docs/process/` and is read on demand.

<lang>Language in conversation with user: Vietnamese (default)</lang>

Code, identifiers, commit messages and docs stay in English; only the conversation with the user is Vietnamese.

## Build and test

- Configure with a preset from `CMakePresets.json` (e.g. `x64-clang-Native`), build with Ninja. Engine binary: `pbrain-rapfi`.
- Unit tests: configure with `-DBUILD_TESTS=ON`; target `rapfi_tests` (sources in `tests/`:
  `generateRootMoves`, `analyzeMoves`, `simd_linear`, `vcn`), run via `ctest` or `./rapfi_tests`.
  `-UNDEBUG` is forced on this target so asserts are live; never do that to the `rapfi` target.
  A new test file must be added to `TEST_DRIVER_SOURCES` in `CMakeLists.txt`.
- `FORCE_ASSERTION=ON` keeps asserts in the engine for debug runs; not for benchmarking.
- Regression: `echo BENCH | ./pbrain-rapfi` (gcc 15.2.0, Release, AVX512+VNNI) must stay **Nodes
  `17621937`, Hash `8c1f72e5`** (built-in config, upstream 0.44.1; was `16733809` / `4621a98a`
  before the 2026-11-03 upstream sync) unless the change is meant to move it, in which case record why and
  the new value in the todo file. VCN scripts: `tests/vcn_{measure,selfplay,sweep}.sh`.
- Index: `.codegraph/` is a local CodeGraph index; use `codegraph_explore` before grep/Read; do not commit it.

## Hard rules

1. **Classify first** (A representation / B search-eval behaviour / C surface and small fixes) —
   highest class wins. Class A needs a design doc with the derived-state audit before code. See
   [`docs/process/change-process.md`](docs/process/change-process.md).
2. **Definition of done, every class**: strict per-function tests (written test list in
   `docs/todo/<CODE>-*.md` *before* code), bench regression check, and tests confirmed to run with
   asserts live. "Tests in a follow-up slice" is an unfinished slice.
3. **Tests**: Arrange-Act-Assert, named `<function>_<scenario>_<expectedBehavior>`, each test builds
   its own `Board`, deterministic (seed any `PRNG`), failing test first. See
   [`docs/process/unit-tests.md`](docs/process/unit-tests.md).
4. **Root cause, not symptom**: fix at the layer that produces the wrong value (e.g. bitkey
   encoding in `game/pattern.h`, not `movepick.cpp` ordering). A recurring symptom means the root
   cause is not found yet. A wrong-layer first fix gets a `docs/fix-log.md` entry.
5. **Don't implement work you were not asked to do now.** Record a mid-conversation requirement as
   `TODO.md` + `docs/todo/` (+ `instruction.md` + `docs/instruction/`); implement only on "do this
   now" / "implement this".
6. **Read the matching `instruction.md` entry before implementing a `TODO.md` item**; note any
   deviation. A new game rule goes through `features/<slug>/` discussion first.
7. **Tracking files are append-only** (`fix-log.md`, `docs/audit/`): correct with a new entry. Keep
   `TODO.md` / `instruction.md` index lines paired with their detail files and move finished items
   to `## Done` with date and bench/test result. See [`docs/process/tracking-files.md`](docs/process/tracking-files.md).
8. **Class A is never "done" on unit tests alone**: add targeted position tests and a self-play
   regression (bitkey/pattern bugs are invisible to casual play).
9. **Measure, don't opine** for class-B changes: decision note (what / expected / how measured) in
   the todo file, then numbers (bench, self-play, acceptance positions).
10. **Use knowledge, not memory, for design opinions**: before recommending architecture, search/eval
    or process options, look up the matching note with
    `SKILLS_TREE/tools/find -k knowledge "<need>"` (delegate multi-query lookups to an Agent) and
    name the note used; if none matches, say so.
11. Commits: conventional style (`fix:`, `feat:`, `search:`, `docs:`, `test:`).

## Where the detail lives

| Topic | Location |
|---|---|
| Risk classes, derived-state audit, definition of done, verification | `docs/process/change-process.md` |
| Unit test conventions | `docs/process/unit-tests.md` |
| Tracking files, status markers, fix-log, audit, features/ | `docs/process/tracking-files.md` |
| Doc map and current items | `docs/README.md`, `TODO.md`, `instruction.md` |
| Layout | `core/` `game/` `search/` `eval/` `command/` `database/` `tuning/` `tests/` `external/` |
