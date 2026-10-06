# VCN-02 — execution guidance

Task: [`../todo/VCN-02-attack-search-quality.md`](../todo/VCN-02-attack-search-quality.md).

## Approach

1. Read the VCN-01 docs ([todo](../todo/VCN-search-mode.md), [instruction](VCN-search-mode.md))
   and check that the VCN-01 changes are present/committed.
2. Build the measurement position set and record the baseline before touching search.
3. One slice per problem, in the ranked order. Item 1 (VC4 locality) first: failing test →
   fix → measure.
4. Each slice: decision note in the todo doc, tests listed before code, `bench` identical, the
   node/result table updated.

## Pitfalls

- **Keep every change gated on `options.vcnMode.enabled()`** (or the node's VCN state). The VCN-off
  path must stay bit-identical; check with `bench` after each slice.
- **`QVCF` locality is intentional for ordinary search.** Change it only for the VCN `vcfOnly`
  case (e.g. a new `ExtraArgs<QVCF>` flag, like `forceAllowB4InVCF`). Do not widen ordinary
  quiescence.
- **`GenType::VC2` / `VCT` filters** are untested (dead code until now) and need explicit
  `generate<...>` instantiations in `movegen.cpp`. Test their boundaries before relying on them.
- **Never prune or reduce the defender's PASS**, and never generate PASS against a five
  (existing invariants, covered by `tests/vcn_tests.cpp`).
- **Counter-attacks must survive any width restriction:** the attacker must still get the
  defence stages; the defender must still get counter-fours (and counter-threes where relevant).
- `Pos::PASS == -1` must never index pattern/history tables (see the VCN-01 instruction).
- Output format is final (user decision): do not add "win / winning line" output.
- MCTS: out of scope.
