# docs/

Tracking and design documentation for the Rapfi engine core. [`../CLAUDE.md`](../CLAUDE.md) holds
the hard rules; this folder holds the detail.

| Path | Contents |
|---|---|
| `process/change-process.md` | Risk classes A/B/C, derived-state audit, definition of done. |
| `process/unit-tests.md` | Unit test conventions (AAA, naming, partitions, isolation, FIRST). |
| `process/tracking-files.md` | How `TODO.md`, `instruction.md`, todo/instruction/design/rules, fix-log and audit fit together. |
| `todo/<CODE>-<slug>.md` | Task detail, indexed by [`../TODO.md`](../TODO.md). |
| `instruction/<CODE>-<slug>.md` | Execution guidance, indexed by [`../instruction.md`](../instruction.md). |
| `design/<CODE>-<slug>.md` | Class-A design docs. |
| `rules/<CODE>-<slug>.md` | Rule / command-surface specs with worked examples. |
| `fix-log.md` | Append-only wrong-layer root-cause log. |
| `audit/<date>-<slug>.md` | Non-bug decisions (create on first use). |

`<CODE>` is a short task code (e.g. `EVAL-01`, `VCN-02`); `<slug>` is a short kebab-case name.
`../features/<slug>/` holds pre-implementation discussion for new rule features.

## Current items

Active: none. Done: SEARCH-01, PERF-02, PERF-01, BUILD-01, VCN-02, VCN, EVAL-01, YXANALZ. The authoritative list is [`../TODO.md`](../TODO.md).
