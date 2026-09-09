# docs/

Tracking and design documentation for the Rapfi engine core. See [`../CLAUDE.md`](../CLAUDE.md)
for the process these directories serve.

| Path | Contents |
|---|---|
| `todo/<CODE>-<slug>.md` | Task detail: scope steps, the function -> test-case list written up front, verification criteria. Indexed by [`../TODO.md`](../TODO.md). |
| `instruction/<CODE>-<slug>.md` | Execution guidance for a task: approach, pitfalls, "do not touch" boundaries. Indexed by [`../instruction.md`](../instruction.md). |
| `design/<CODE>-<slug>.md` | Class-A design doc (representation / derived-state changes), including the mandatory derived-state audit. Required before code for class-A changes only. |
| `rules/<CODE>-<slug>.md` | Specification of a game rule or command surface, with worked board examples used as targeted position tests. |
| `fix-log.md` | Append-only root-cause log (see CLAUDE.md, "Root-cause diagnosis"). |

`<CODE>` is a short task code (e.g. `EVAL-03`, `TEST-01`); `<slug>` is a short kebab-case name.
