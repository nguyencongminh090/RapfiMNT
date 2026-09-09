# TODO

Index of tracked work. Each line links to a detail file under `docs/todo/`. Pair every entry with
a matching `instruction.md` line when execution guidance exists. See
[CLAUDE.md](CLAUDE.md) for the process this file is part of.

## Active

## Done

- [EVAL-01 — VNNI signed-input affine via +128 input-bias trick](docs/todo/EVAL-01-vnni-i8i8-input-bias.md) — feed `input ^ 0x80` to one `vpdpbusd` for mix9 `starBlock`'s `value_corner_down` and fold `-128·Σrow(w)` into the bias at load time (VNNI builds only). Class B, opt-in, mix9-only. Done 2026-09-09 — bench bit-identical (16733809 / 4621a98a), 26/26 unit tests (12 new); sign-split `vpand/vpandn/vpsubd/vpaddd` + 2nd `vpdpbusd` removed from `value_corner_down`.
- [YXANALZ — user-supplied root candidates](docs/todo/YXANALZ-user-root-candidates.md) — new `YXANALZ x,y ... DONE` command: analyze an explicit user allow-list of depth-0 root moves instead of engine-generated candidates. Class C. Done 2026-09-09 — bench bit-identical (16733809 / 4621a98a), 14/14 unit tests.
