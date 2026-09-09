# EVAL-01 — execution guidance

Task: `docs/todo/EVAL-01-vnni-i8i8-input-bias.md`.

## Approach (recommended order)

1. **Baseline first.** Build `pbrain-rapfi` Release with AVX512+VNNI, run `BENCH`, write the
   Search Bench Nodes + Hash into the task doc's "Baseline" line and into
   `CLAUDE.md` §"Definition of done" item 3 if that slot is still empty.
2. **Scalar reference + failing test.** Add `tests/simd_linear_tests.cpp` with the scalar
   `Σ s·w + bias` reference and the `linear_biasedSignedInput_*` cases. They won't compile until
   the `InputBiased128` template arg exists — add the parameter as a no-op first (ignored,
   `static_assert` only), get the "equals unbiased path" test green, then implement the XOR.
3. **Kernel.** Implement the biased path in the `ChunkSize=4` `Affine<int8_t>` specialization
   only (`linear.h:49-103`). XOR `in0` once per chunk `i`, call `dot4_u7i8_accum`.
4. **Loader.** `Mix9svqWeightLoader::preprocess(StarBlockWeight&)` — fold the bias **before**
   `preprocessLinear` scrambles the weight, under `#if defined(USE_VNNI)`.
5. **Wire `starBlock`.** Flip the `value_corner_down` call to pass `InputBiased128=true`.
6. **Verify:** bench bit-identical, unit tests, `objdump` instruction count.

## Pitfalls (this codebase has produced silent eval corruption before)

- **Bias fold ordering.** `simd::preprocessLinear<OutSize,OutSize>(b.value_corner_down.weight)`
  permutes the weight array in place. `rowsum[r] = Σ_c weight[r*InSize + c]` **must** be summed
  from the original row-major layout, i.e. *before* that call. Summing after the scramble gives
  a per-row sum over the wrong elements → wrong bias → hash moves. The test
  `preprocess_valueCornerDown_biasFoldedByMinus128TimesRowSum` exists to catch exactly this.
- **Macro agreement.** The kernel XOR path and the loader bias fold are **both** `#if
  defined(USE_VNNI)`. If one is guarded and the other is not, a build computes `Σs·w` one way
  and applies a bias corrected for the other way → silently wrong eval that still "looks sane".
  Never gate them on different conditions.
- **`s ^ 0x80` vs `s + 128`.** They are equal *as the unsigned byte value* for every signed
  input, including `s = -128` (`0x80 ^ 0x80 = 0x00`) and `s = 127` (`0x7F ^ 0x80 = 0xFF`). Use
  XOR, not a saturating add. The two boundary tests pin this.
- **`dot4_u7i8_accum` non-VNNI fallback saturates.** Its `#else` branch is `maddubs`+`madd`,
  and `maddubs` on `u8 × i8` with `u ∈ [128,255]` overflows the int16 intermediate. That is why
  the biased path is VNNI-only. Do **not** route the biased (0..255) input through the non-VNNI
  `dot4_u7i8_accum`.
- **Template arg position.** `simd::linear` has several bool params
  (`SignedInput, Bias, PreReLU, PostReLU`) then `Alignment, Inst, AccType, InputType`. Add
  `InputBiased128` in a place that does not silently shift an existing positional call. Grep
  every `simd::linear<` call site and confirm none breaks; prefer appending after the existing
  bools so default-valued tail params still work.
- **Other `SignedInput=true` callers.** In this repo only `mix9svqnnue.cpp:237`. `mix10nnue.cpp`
  has many (`linearBlock<true>`) — they must keep `InputBiased128=false` (the default) and be
  left byte-for-byte unchanged. If a refactor makes it tempting to "just apply it everywhere",
  stop: mix10 is out of scope and its weight loader would also need the fold.
- **`Divisor`/shift interaction.** `value_corner_down` uses `linear` (int32 accumulate) then a
  separate `crelu<OutSize, 128>` (`mix9svqnnue.cpp:241`). The bias fold changes the int32
  accumulator by exactly `-128·Σw + 128·Σw = 0` net — it must not touch the `crelu` divisor or
  the `dot2` divisor. Only `value_corner_down.bias` changes.

## Do not touch

- `eval/mix10nnue.*` — out of scope. Zero diff.
- `dot4_i8i8_accum` existing behaviour — the non-VNNI and the non-`InputBiased128` paths stay
  exactly as they are. Add a parallel path, don't rewrite the current one.
- `simd::preprocessLinear` itself — the weight scramble is shared with many layers. Do the bias
  fold in the mix9 loader, not inside `preprocessLinear`.
- `-ffp-contract=off` and any float-head code — unrelated; this task is integer-only.
- The weight file format / `weightloader.h` stream parsing — the on-disk bytes are unchanged.

## Definition of done (all four, per CLAUDE.md)

1. Unit tests: every new/modified function has ≥1 behaviour-named case (list in the task doc).
2. Test-case list was written before the code (it is, in the task doc).
3. `rapfi bench` **bit-identical** to baseline — this change is a pure optimization. Record the
   before/after Nodes+Hash in the task doc's "Verification results".
4. Confirm `rapfi_tests` built with asserts live (`--check-asserts-live`).

If the bench hash is not bit-identical, this is a bug, not an expected behaviour shift — do not
record a "reason it moved", fix it.
