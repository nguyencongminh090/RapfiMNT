# Tracking files

Detail for [`../../CLAUDE.md`](../../CLAUDE.md). Read an index first; open only the matched detail file.

| File | Role |
|---|---|
| `TODO.md` | Index of work, one line per item, grouped `## Active` / `## Done`. Links to `docs/todo/<CODE>-<slug>.md`. |
| `docs/todo/<CODE>-<slug>.md` | Task detail: scope, function -> test-case list (written before code), verification criteria. |
| `instruction.md` | Index of execution guidance. Links to `docs/instruction/<CODE>-<slug>.md`. |
| `docs/instruction/<CODE>-<slug>.md` | Approach, pitfalls, "do not touch" boundaries, verification before done. |
| `docs/design/<CODE>-<slug>.md` | Class-A design doc with the derived-state audit. |
| `docs/rules/<CODE>-<slug>.md` | Rule or command-surface spec with worked examples used as position tests. |
| `docs/fix-log.md` | Append-only root-cause log (wrong-layer fixes only). |
| `docs/audit/<date>-<slug>.md` | Decisions that are not bugs (tooling, measurement method, rejected options). Create on first use. |

## Rules

- **Append-only**: `fix-log.md` and `docs/audit/` are corrected with a new entry, never rewritten.
- **Index and detail stay paired**: every `TODO.md` line has a `docs/todo/` file; add the matching
  `instruction.md` line when execution guidance exists (missing is fine).
- **Read first**: before implementing a `TODO.md` item, read its `instruction.md` entry and note any
  deviation in the todo file.
- **Status line**: each todo file starts with `**Status:**` (open / in progress / done + date) and
  `**Class:**` (A/B/C). When an item is done, move its index lines to `## Done` with the date and the
  bench/test result, in both `TODO.md` and `instruction.md`.
- **Stack, don't perform**: a requirement raised mid-conversation is recorded as a todo (+ instruction)
  line, not implemented, unless the user says to do it now.
- **New rule feature** (an addition to the game's rules): discuss under `features/<slug>/`
  (`user_story.md`, `diagram/`, `planning.md`), resolve open questions with the user, then formalise
  into todo + instruction before code.
- **Fix-log entry** is required when a first fix touched the layer where the symptom was visible and
  the bug came back from the layer that produces the value. Entry format: date, symptom, first
  (wrong-layer) fix, producing layer, corrected fix, the check that would have caught it earlier.
