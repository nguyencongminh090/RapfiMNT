# EVAL-01 — VNNI signed-input affine via +128 input-bias trick

**Status:** done 2026-09-09 — bench bit-identical (Nodes 16733809 / Hash 4621a98a),
12 new unit tests (26/26 total), asserts-live confirmed, non-VNNI build compiles with the
flag as a no-op.

**Class:** B (eval SIMD implementation). No persistent encoding change, no compile-time table,
no position identity. Decision note below stands in for a design doc. `bench` must stay
**bit-identical** — this is a pure instruction-count optimization, not a behaviour change.

**Scope guard:** `eval/mix10nnue.*` is **out of scope** for this repo. The change must be
opt-in and wired for **`mix9svqnnue` only**. See `docs/instruction/EVAL-01-vnni-i8i8-input-bias.md`.

## Goal

Remove the "double `vpdpbusd`" cost from the one signed-input int8 affine layer on the mix9
value-eval hot path: `starBlock`'s `value_corner_down` (`eval/mix9svqnnue.cpp:237`,
`simd::linear<ValueDim, ValueDim, /*SignedInput=*/true>`), called 13× per value eval
(`mix9svqnnue.cpp:581-591` + `:619-628`).

### Why the current path is expensive

`vpdpbusd` computes `u8 × i8 → i32`. The input to `value_corner_down` (`dotsum` from
`simd::dot2`, `activation.h:217`) is **signed** and genuinely negative-capable (`up2` is
produced by `crelu<..., NoReLU=true>`, `mix9svqnnue.cpp:230`). So `dot4_i8i8_accum`
(`eval/simd/vec.h:1017-1041`) splits the sign bit and runs `vpdpbusd` twice plus
`vpand`/`vpandn`/`vpsubd`/`vpaddd` — ~6 µop for a step that should be 1.

### The trick

For a signed input byte `s ∈ [-128,127]`, `u = s ^ 0x80 == s + 128 ∈ [0,255]` (exact in both
the `s≥0` and `s<0` cases). Then

```
vpdpbusd(acc, u, w) = Σ (s_i + 128)·w_i = Σ s_i·w_i + 128·Σ w_i
⟹  Σ s_i·w_i = vpdpbusd_result − 128·Σ_row(w)
```

Fold `−128·Σ_row(w)` into `bias[row]` at weight-load time. Runtime becomes **1 `vpxor`
(amortized once per broadcast chunk) + 1 `vpdpbusd`**, i.e. identical cost to the existing
unsigned `dot4_u7i8_accum` path.

## Decision note (what / expected / how measured)

- **What:** add an opt-in "input pre-biased by +128" mode to the int8 `Affine`/`linear` kernel,
  used only by `mix9svqnnue`'s `value_corner_down`. `Mix9svqWeightLoader::preprocess` folds the
  `-128·Σrow(w)` correction into that layer's bias. Both the kernel XOR path and the bias fold
  are gated on `#if defined(USE_VNNI)` so a non-VNNI build keeps the current unbiased path with
  the unmodified bias.
- **Expected:** identical int32 accumulator output (hence identical `crelu` output, identical
  eval, identical search tree). On a VNNI build the `value_corner_down` inner loop drops
  `vpand`+`vpandn`+one `vpdpbusd`+`vpsubd`+`vpaddd` per output batch, keeping `vpxor`+`vpdpbusd`.
- **Measured:** `rapfi bench` node count + hash **bit-identical** to the pre-change baseline
  (record baseline below on first measurement). SIMD unit test: biased-path `linear` output ==
  scalar reference `Σ s·w + bias` for randomized and boundary inputs. `perf stat` /
  `objdump -d | grep -c vpdpbusd` for the instruction-count delta.
- **Baseline (gcc 15 `/usr/bin/c++`, Release, AVX512+VNNI, `BENCH` command, 2026-09-09):**
  Search Bench Nodes `16733809`, Hash `4621a98a`.
- **Non-goal:** `vpdpbssd` (AVX-VNNI-INT8). Separate, later, CPU-gated; this task makes it
  unnecessary on the common case.

## Settled design points

| Point | Decision |
|---|---|
| Which layer | `value_corner_down` only. It is the sole `SignedInput=true` `linear` call in `mix9svqnnue.cpp`. `value_corner_up1`/`up2` are already `SignedInput=false` (u7). |
| Kernel signal | New `bool InputBiased128 = false` template parameter on `simd::linear` / `Affine<int8_t>::forward`. Caller (`starBlock`) passes `true` for `value_corner_down`. Only meaningful together with `SignedInput`. |
| Non-VNNI build | `#if !defined(USE_VNNI)`: `InputBiased128` is ignored, the existing `dot4_i8i8_accum` path runs, and `preprocess` does **not** fold the bias. Kernel and loader gate on the **same** macro so they never disagree. |
| Bias fold location | `Mix9svqWeightLoader::preprocess(StarBlockWeight&)` (`mix9svqnnue.cpp:201-206`). Compute `rowsum` from `value_corner_down.weight` **before** `simd::preprocessLinear` scrambles it, then `bias[row] -= 128 * rowsum[row]` — under `#if defined(USE_VNNI)`. |
| XOR granularity | In the `ChunkSize=4` `Affine` specialization (`linear.h:49-103`) the input is broadcast once per chunk `i` as `in0`; XOR `in0` with a `set1_epi8(0x80)` constant there, once per chunk, before the `j` loop. Then call `dot4_u7i8_accum(acc[j], in0_xored, w0[j])`. |
| Overflow | `value_corner_down` InSize = `ValueDim = 64`. `max|Σ u·w| = 255·128·64 ≈ 2.09M`, ~1000× margin under `INT32_MAX`. Add `static_assert(255LL*128*InSize < INT32_MAX)` in the biased path. `bias` is `int32_t` (`mixcommon.h:77`); `|128·Σrow| ≤ 128·8192 ≈ 1.05M`. |
| Weight file format | Unchanged. Only the in-memory bias differs, and only on a VNNI build — same pattern as `preprocessLinear`'s existing build-dependent weight scramble. |
| mix10 | Not touched. `linearBlock<true>` in `mix10nnue.cpp` keeps calling the unchanged `SignedInput=true` path (`InputBiased128` defaults false). |

## Scope steps

1. **`eval/simd/vec.h`** — no new intrinsic needed; the biased path reuses `dot4_u7i8_accum`.
   (Optional: a thin `dot4_i8i8_biased_accum` wrapper that XORs then calls `dot4_u7i8_accum`, if
   it reads cleaner than XOR-in-`forward`.)
2. **`eval/simd/linear.h`**
   - Add `bool InputBiased128 = false` to `linear<>` and thread it into
     `Affine<int8_t, ...>::forward`.
   - In the `ChunkSize=4` specialization: when `InputBiased128 && defined(USE_VNNI)`, XOR the
     broadcast `in0` with `set1_epi8(0x80)` once per chunk and use `dot4_u7i8_accum`; else
     unchanged.
   - `static_assert(!InputBiased128 || (255LL*128*InSize < INT32_MAX))`.
   - `static_assert` that `InputBiased128` implies `SignedInput` (the flag is only defined for
     the signed case).
3. **`eval/mix9svqnnue.cpp`**
   - `starBlock`: change the `value_corner_down` call to
     `simd::linear<OutSize, OutSize, /*SignedInput=*/true, /*Bias=*/true, false, false,
     /*...*/, /*InputBiased128=*/true>(...)` (match the existing template arg order — verify).
   - `Mix9svqWeightLoader::preprocess(StarBlockWeight&)`: `#if defined(USE_VNNI)` — before
     `simd::preprocessLinear<OutSize, OutSize>(b.value_corner_down.weight)`, compute
     `int32_t rowsum[OutSize]` from the raw weight and apply `b.value_corner_down.bias[r] -=
     128 * rowsum[r]`.
4. **`CMakeLists.txt`** — add `tests/simd_linear_tests.cpp` to `TEST_DRIVER_SOURCES`.
5. **`tests/simd_linear_tests.cpp`** + register in `tests/testmain.cpp` — see test list below.

## Function → test-case list (write before code)

Reference: a scalar `int32 ref[o] = bias[o] + Σ_i int32(input_i8[i]) * int32(weight_i8[o][i])`.

`Evaluation::simd::linear<..., SignedInput=true, InputBiased128=true>` (new path):

- `linear_biasedSignedInput_randomData_matchesScalarReference` — 1000 randomized
  `(input ∈ [-128,127], weight ∈ [-128,127], bias)` cases, `OutSize=InSize=64`; assert every
  lane equals `ref`.
- `linear_biasedSignedInput_inputAllMinus128_matchesScalarReference` — boundary: `s = -128`
  (the case `s ^ 0x80` must yield `0`, not `256`).
- `linear_biasedSignedInput_inputAll127_matchesScalarReference` — boundary: `s = 127`
  (`s ^ 0x80` = `255`).
- `linear_biasedSignedInput_inputAllZero_returnsBiasExactly`.
- `linear_biasedSignedInput_weightExtremes_matchesScalarReference` — weights all `-128`, then
  all `127`, with input at max magnitude (overflow-headroom probe; compare against int64 ref).
- `linear_biasedSignedInput_equalsUnbiasedSignedInputPath` — build a second reference by
  calling the **old** `SignedInput=true, InputBiased128=false` kernel with the **unmodified**
  bias; assert identical. (Both kernels compiled in the test binary regardless of `USE_VNNI` —
  the test forces `InputBiased128` on/off explicitly, not via the macro.)
- `linear_biasedSignedInput_noVnniBuild_pathIsUnbiasedAndStillCorrect` — `#ifndef USE_VNNI`
  guard: assert the kernel still matches the scalar reference (flag is a no-op).

`Mix9svqWeightLoader::preprocess(StarBlockWeight&)` bias fold (small fixture, VNNI build only):

- `preprocess_valueCornerDown_biasFoldedByMinus128TimesRowSum` — construct a
  `StarBlockWeight<8,8>` with known weights/bias, capture `rowsum` before, call `preprocess`,
  assert `bias[r] == original_bias[r] - 128 * rowsum[r]` for each row.
- `preprocess_valueCornerDown_weightScrambleStillApplied` — the weight array is still permuted
  exactly as `preprocessLinear` alone would (fold must not disturb the scramble).
- `preprocess_valueCornerUp1Up2_biasUnchanged` — only `value_corner_down` is folded.
- `preprocess_noVnniBuild_biasUnchanged` — `#ifndef USE_VNNI`: `preprocess` leaves all biases
  as-is.

`starBlock` (via a mix9 `Accumulator` fixture or direct call with synthetic weights):

- `starBlock_producesSameOutputWithAndWithoutBiasTrick` — two `StarBlockWeight` copies, one run
  through the biased kernel + folded bias, one through the old kernel + original bias; identical
  `int8` output for randomized `input[InSize]`.

## Verification

- Tests built with asserts live (`-UNDEBUG` on `rapfi_tests`; confirm via
  `rapfi_tests --check-asserts-live` as EVAL/YXANALZ does).
- `rapfi bench` **bit-identical** vs baseline (record Nodes + Hash above first). If the hash
  moves, the fold or the XOR is wrong — do not proceed.
- `objdump -d build/.../pbrain-rapfi | grep -c vpdpbusd` before/after (expect fewer, or same
  count with the `vpand`/`vpandn` gone — check `perf annotate` on
  `mix9svq::...::evaluateValue`).
- Optional: `perf stat -e instructions,uops_issued.any bench` before/after.

### Verification results (2026-09-09, gcc 15.2.0, Release, AVX512+VNNI)

- **Asserts live:** `./build/rapfi_tests --check-asserts-live` trips the sentinel assert and
  aborts (exit 134, "asserts are live" printed). `rapfi_tests` target forces `-UNDEBUG`.
- **Unit tests:** `./build/rapfi_tests` → `26/26 tests passed` (12 new EVAL-01 cases:
  7 `linear_biasedSignedInput_*`, 4 `preprocess_*`, 1 `starBlock_*`). Same suite on a
  non-VNNI build (`USE_VNNI=OFF`, AVX512) also `26/26` — the `#ifndef USE_VNNI` cases and the
  flag-is-a-no-op paths pass there too.
- **Bench bit-identical:** baseline `Nodes 16733809 / Hash 4621a98a` → post-change
  `Nodes 16733809 / Hash 4621a98a`. Identical. (NPS is within run-to-run noise on this host,
  ~750k–870k both before and after — no reliable NPS delta measurable here.)
- **Instruction-count delta** (`objdump -d`, `Accumulator::evaluateValue` — `starBlock` inlines
  13× into it):
  | insn | before | after | note |
  |---|---:|---:|---|
  | `vpdpbusd` | 384 | 1112 | gcc fully unrolls the now-single-`vpdpbusd` `value_corner_down` loop that it previously kept rolled; per-element `vpdpbusd` for that layer goes 2→1 |
  | `vpsubd` | 52 | 0 | i8i8 sign-split subtract — gone |
  | `vpaddd` | 58 | 6 | i8i8 sign-split add — gone |
  | `vpandd` | 13 | 0 | i8i8 MSB mask — gone |
  | `vpandnd` | 13 | 0 | i8i8 low-7 mask — gone |
  | `vpxord` | 64 | 210 | the `s ^ 0x80` (once per broadcast chunk, now unrolled) |
  Whole-binary `vpdpbusd` 1188 → 1916 (same unroll artifact). The `vpand*/vpsubd/vpaddd` +
  second `vpdpbusd` are confirmed gone from `value_corner_down`; static `vpdpbusd` rises only
  because the lighter loop is now fully unrolled by gcc.
- **Non-VNNI build:** `cmake -DUSE_VNNI=OFF -DUSE_AVX512=ON` builds `pbrain-rapfi` and
  `rapfi_tests` clean; `grep -c vpdpbusd` on that binary = 0; `InputBiased128` and the loader
  fold are both compiled out.
