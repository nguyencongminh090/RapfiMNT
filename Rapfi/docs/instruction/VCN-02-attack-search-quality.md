# VCN-02 — execution guidance

Task: [`../todo/VCN-02-attack-search-quality.md`](../todo/VCN-02-attack-search-quality.md).

## What VCN is for (read first)

VCN maximises the attacker's chances: it searches for every way to attack and win. Its value
measures the **attack chance**, not the position:
- an attack exists → high value, `+M` when proven;
- no attack exists → strongly negative, even if an ordinary search calls the position balanced;
- `-M` / negative means "no win found within the depth/time", not a proof the defender wins.

Its main use is complex positions where an ordinary search or the network misses an attack.
Those cannot be hand-written as tests; judge changes on a position set with a fixed time per
position (see the todo's measurement section and `tests/vcn_measure.sh`).

## Approach

1. Read the VCN-01 docs ([todo](../todo/VCN-search-mode.md), [instruction](VCN-search-mode.md))
   and check that the VCN-01 changes are present/committed.
2. One slice per problem, in the order in the todo. Each slice: decision note in the todo doc,
   tests listed before code, `bench` identical, the position table re-run and the changed rows
   recorded.
3. Hand-made tests for clear-cut cases (forced blocks, VCF, counter-fours). For quality claims,
   measure on a position set (self-play or real games) at a fixed time per position.

## Pitfalls

- **Keep every change gated on `options.vcnMode.enabled()`** (or the node's VCN state). The VCN-off
  path must stay bit-identical; check with `bench` after each slice.
- **Do not turn off the ordinary pruning inside VCN** (user decision 2026-10-06: ~10× cost for
  ~10% more wins on the self-play set). Prefer ordering (item 6) to get attacks searched first.
- **Do not restrict move width** (attacker or defender) by pattern filters: a quiet move that
  creates a VCF/VCT is a real attack, and dropped defender moves give false wins. Order instead.
- **`QVCF` locality is intentional for ordinary search.** It is widened only for the VC4 attacker
  (`ExtraArgs<QVCF>::fullBoardVCF`). Do not widen ordinary quiescence.
- **`quickWinCheck` in VCF-only mode** (`vcfOnly`) must not count a double three: the VCN defender
  answers it with a pass. Other callers keep the default.
- **Never prune or reduce the defender's PASS**, and never generate PASS against a five
  (covered by `tests/vcn_tests.cpp`).
- `Pos::PASS == -1` must never index pattern/history tables (see the VCN-01 instruction).
- **Measurement runner:** send `END` only after the engine printed its move; it reads stdin while
  thinking and an early `END` cuts the search (nondeterministic node counts).
- **Evaluator:** this tree has no config and `Networks/` is empty, so engine runs use the
  classical evaluator. Values from a network build differ; say which one a number came from.
- Output format is final (user decision): do not add "win / winning line" output. MCTS is out of
  scope.
