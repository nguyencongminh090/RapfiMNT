/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// EVAL-01 unit tests: the "+128 input-bias" VNNI path for the signed-input int8
// affine kernel (simd::linear<..., InputBiased128=true>) and the matching
// value_corner_down bias fold in the mix9svq weight loader.
//
// Reference: scalar int32 ref[o] = bias[o] + Sum_i int32(input_i8[i]) * int32(weight_i8[o][i])
// over the ORIGINAL row-major weight layout. The biased kernel, fed input ^ 0x80
// and a bias with -128*rowsum(w) folded in, must reproduce ref lane-for-lane and
// must equal the pre-existing unbiased signed-input kernel.
//
// This target is compiled with -UNDEBUG regardless of build type, so every
// assert() below is actually evaluated (see CLAUDE.md "Verification without a UI").

#include "testutil.h"

#include "../eval/simd/activation.h"
#include "../eval/simd/linear.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

// Free helper defined in eval/mix9svqnnue.cpp (external linkage). Subtracts
// 128*rowsum(weight) from each output bias, using the row-major weight layout;
// a no-op unless USE_VNNI is defined.
namespace Evaluation::mix9svq {
void foldValueCornerDownInputBias(const int8_t *weight, int32_t *bias, int outSize, int inSize);
}

namespace {

using namespace Evaluation;

// value_corner_down dimensions in mix9svq: OutSize == InSize == ValueDim.
constexpr int N = 64;

constexpr int Align = simd::NativeAlignment;

// ---------------------------------------------------------------------------
// Scalar helpers (the "known good" reference).

// ref[o] = bias[o] + sum_i in[i] * w[o*InSize + i], row-major weight, int64 math.
template <int OutSize, int InSize>
void scalarRef(int64_t out[OutSize],
               const int8_t input[InSize],
               const int8_t weightRowMajor[OutSize * InSize],
               const int32_t bias[OutSize])
{
    for (int o = 0; o < OutSize; o++) {
        int64_t acc = bias[o];
        for (int i = 0; i < InSize; i++)
            acc += int64_t(input[i]) * int64_t(weightRowMajor[o * InSize + i]);
        out[o] = acc;
    }
}

// biasFolded[o] = bias[o] - 128 * sum_i w[o*InSize + i]  (row-major weight)
template <int OutSize, int InSize>
void foldBiasScalar(int32_t biasFolded[OutSize],
                    const int8_t weightRowMajor[OutSize * InSize],
                    const int32_t bias[OutSize])
{
    for (int o = 0; o < OutSize; o++) {
        int32_t rowsum = 0;
        for (int i = 0; i < InSize; i++)
            rowsum += int32_t(weightRowMajor[o * InSize + i]);
        biasFolded[o] = bias[o] - 128 * rowsum;
    }
}

// The bias the mix9svq weight loader would hand to the InputBiased128 kernel on
// THIS build: folded by -128*rowsum on a VNNI build, unmodified otherwise (the
// flag and the loader fold are both gated on USE_VNNI, so they never disagree).
template <int OutSize, int InSize>
void loaderBias(int32_t out[OutSize],
                const int8_t weightRowMajor[OutSize * InSize],
                const int32_t bias[OutSize])
{
#if defined(USE_VNNI)
    foldBiasScalar<OutSize, InSize>(out, weightRowMajor, bias);
#else
    std::memcpy(out, bias, sizeof(int32_t) * OutSize);
#endif
}

// ---------------------------------------------------------------------------
// Fixture: aligned buffers for one OutSize=InSize=N affine layer.

struct AffineFixture
{
    alignas(Align) std::array<int8_t, N * N> weightRowMajor {};
    alignas(Align) std::array<int8_t, N * N> weightScrambled {};
    alignas(Align) std::array<int32_t, N> bias {};
    alignas(Align) std::array<int8_t, N> input {};

    // Scramble a fresh copy of the row-major weight into the layout the kernel
    // consumes at inference time (exactly what the weight loader does).
    void scramble()
    {
        weightScrambled = weightRowMajor;
        simd::preprocessLinear<N, N>(weightScrambled.data());
    }

    // Run the NEW biased path: input is signed, bias already has -128*rowsum folded in.
    void runBiased(int32_t out[N], const int32_t biasFolded[N])
    {
        alignas(Align) std::array<int32_t, N> b {};
        std::memcpy(b.data(), biasFolded, sizeof(b));
        simd::linear<N,
                     N,
                     /*SignedInput=*/true,
                     /*Bias=*/true,
                     /*PreReLU=*/false,
                     /*PostReLU=*/false,
                     /*InputBiased128=*/true>(out, input.data(), weightScrambled.data(), b.data());
    }

    // Run the pre-existing unbiased signed-input path with the UNMODIFIED bias.
    void runUnbiased(int32_t out[N])
    {
        simd::linear<N, N, /*SignedInput=*/true>(out,
                                                 input.data(),
                                                 weightScrambled.data(),
                                                 bias.data());
    }
};

// Deterministic fill (seeded mt19937, no nondeterministic sources).
void fillRandom(AffineFixture &f, uint32_t seed)
{
    std::mt19937                           rng(seed);
    std::uniform_int_distribution<int>     b8(-128, 127);
    std::uniform_int_distribution<int32_t> b32(-100000, 100000);
    for (auto &w : f.weightRowMajor)
        w = int8_t(b8(rng));
    for (auto &x : f.input)
        x = int8_t(b8(rng));
    for (auto &v : f.bias)
        v = b32(rng);
}

void assertMatchesScalarRef(AffineFixture &f)
{
    alignas(Align) int32_t kbias[N];
    loaderBias<N, N>(kbias, f.weightRowMajor.data(), f.bias.data());

    int64_t ref[N];
    scalarRef<N, N>(ref, f.input.data(), f.weightRowMajor.data(), f.bias.data());

    alignas(Align) int32_t got[N];
    f.runBiased(got, kbias);

    for (int o = 0; o < N; o++)
        assert(int64_t(got[o]) == ref[o] && "biased kernel lane != scalar reference");
}

// ===========================================================================
// simd::linear<..., SignedInput=true, InputBiased128=true>

void linear_biasedSignedInput_randomData_matchesScalarReference()
{
    // Arrange / Act / Assert: 1000 randomized (input, weight, bias) cases.
    for (uint32_t trial = 0; trial < 1000; trial++) {
        AffineFixture f;
        fillRandom(f, 0xE1A0u + trial);
        f.scramble();
        assertMatchesScalarRef(f);
    }
}

void linear_biasedSignedInput_inputAllMinus128_matchesScalarReference()
{
    // Arrange: boundary s = -128. (s ^ 0x80) must be 0, not 256.
    AffineFixture f;
    fillRandom(f, 0x1234u);
    f.input.fill(int8_t(-128));
    f.scramble();

    // Act / Assert
    assertMatchesScalarRef(f);
}

void linear_biasedSignedInput_inputAll127_matchesScalarReference()
{
    // Arrange: boundary s = 127. (s ^ 0x80) == 255.
    AffineFixture f;
    fillRandom(f, 0x5678u);
    f.input.fill(int8_t(127));
    f.scramble();

    // Act / Assert
    assertMatchesScalarRef(f);
}

void linear_biasedSignedInput_inputAllZero_returnsBiasExactly()
{
    // Arrange: zero input -> output is exactly the (unmodified) bias.
    AffineFixture f;
    fillRandom(f, 0x9ABCu);
    f.input.fill(0);
    f.scramble();

    alignas(Align) int32_t kbias[N];
    loaderBias<N, N>(kbias, f.weightRowMajor.data(), f.bias.data());

    // Act
    alignas(Align) int32_t got[N];
    f.runBiased(got, kbias);

    // Assert
    for (int o = 0; o < N; o++)
        assert(got[o] == f.bias[o] && "zero input must yield the original bias");
}

void linear_biasedSignedInput_weightExtremes_matchesScalarReference()
{
    // Arrange: overflow-headroom probe. Weights all -128 then all 127, input at
    // max magnitude. Compared against the int64 scalar reference.
    for (int weightVal : {-128, 127}) {
        for (int inputVal : {-128, 127}) {
            AffineFixture f;
            fillRandom(f, 0xF00Du);
            for (auto &w : f.weightRowMajor)
                w = int8_t(weightVal);
            f.input.fill(int8_t(inputVal));
            f.bias.fill(123456);
            f.scramble();

            // Act / Assert
            assertMatchesScalarRef(f);
        }
    }
}

void linear_biasedSignedInput_equalsUnbiasedSignedInputPath()
{
    // Arrange: the biased kernel + folded bias must be bit-identical to the OLD
    // SignedInput=true, InputBiased128=false kernel + unmodified bias. Both are
    // compiled into this binary; the flag is toggled explicitly, not via a macro.
    for (uint32_t trial = 0; trial < 256; trial++) {
        AffineFixture f;
        fillRandom(f, 0x2200u + trial);
        f.scramble();

        alignas(Align) int32_t kbias[N];
        loaderBias<N, N>(kbias, f.weightRowMajor.data(), f.bias.data());

        // Act
        alignas(Align) int32_t biased[N];
        alignas(Align) int32_t unbiased[N];
        f.runBiased(biased, kbias);
        f.runUnbiased(unbiased);

        // Assert
        for (int o = 0; o < N; o++)
            assert(biased[o] == unbiased[o] && "biased path != unbiased signed-input path");
    }
}

void linear_biasedSignedInput_noVnniBuild_pathIsUnbiasedAndStillCorrect()
{
#if !defined(USE_VNNI)
    // On a non-VNNI build InputBiased128 is a no-op: the kernel runs the ordinary
    // signed-input path and the bias must NOT be folded. Feeding the unmodified
    // bias must still match the scalar reference.
    AffineFixture f;
    fillRandom(f, 0x0F0Fu);
    f.scramble();

    int64_t ref[N];
    scalarRef<N, N>(ref, f.input.data(), f.weightRowMajor.data(), f.bias.data());

    alignas(Align) int32_t got[N];
    f.runBiased(got, f.bias.data());  // unmodified bias

    for (int o = 0; o < N; o++)
        assert(int64_t(got[o]) == ref[o] && "no-VNNI biased flag must be a correct no-op");
#else
    // VNNI build: this scenario is not reachable; the biased-vs-unbiased and
    // scalar-reference equivalence is covered by the other cases above.
#endif
}

// ===========================================================================
// Mix9svqWeightLoader value_corner_down bias fold

void preprocess_valueCornerDown_biasFoldedByMinus128TimesRowSum()
{
    // Arrange: a small layer with known weights and bias.
    constexpr int O = 8, I = 8;
    int8_t        weight[O * I];
    int32_t       bias[O], biasOrig[O];
    for (int r = 0; r < O; r++) {
        bias[r] = biasOrig[r] = 1000 * (r + 1);
        for (int c = 0; c < I; c++)
            weight[r * I + c] = int8_t((r - c) * 3 - 5);  // spans negative and positive
    }
    int32_t rowsum[O] = {};
    for (int r = 0; r < O; r++)
        for (int c = 0; c < I; c++)
            rowsum[r] += weight[r * I + c];

    // Act
    mix9svq::foldValueCornerDownInputBias(weight, bias, O, I);

    // Assert
#if defined(USE_VNNI)
    for (int r = 0; r < O; r++)
        assert(bias[r] == biasOrig[r] - 128 * rowsum[r] && "bias fold != -128*rowsum");
#else
    for (int r = 0; r < O; r++)
        assert(bias[r] == biasOrig[r] && "non-VNNI build must not fold the bias");
#endif
}

void preprocess_valueCornerDown_weightScrambleStillApplied()
{
    // Arrange: two identical row-major weight copies.
    AffineFixture f;
    fillRandom(f, 0x7777u);
    alignas(Align) std::array<int8_t, N * N> weightFoldThenScramble = f.weightRowMajor;
    alignas(Align) std::array<int8_t, N * N> weightScrambleOnly     = f.weightRowMajor;
    alignas(Align) std::array<int32_t, N> bias                      = f.bias;

    // Act: fold (touches bias only) then scramble, vs. scramble alone.
    mix9svq::foldValueCornerDownInputBias(weightFoldThenScramble.data(), bias.data(), N, N);
    simd::preprocessLinear<N, N>(weightFoldThenScramble.data());
    simd::preprocessLinear<N, N>(weightScrambleOnly.data());

    // Assert: the fold must not disturb the weight permutation.
    assert(weightFoldThenScramble == weightScrambleOnly
           && "bias fold must not change the scrambled weight layout");
}

void preprocess_valueCornerUp1Up2_biasUnchanged()
{
    // Arrange: mirror the loader's preprocess(StarBlockWeight&) sequence: up1/up2
    // get preprocessLinear on their weight only; value_corner_down additionally
    // gets the bias fold. Only value_corner_down.bias may change.
    struct MiniStarBlock
    {
        alignas(Align) std::array<int8_t, (2 * N) * N> up1w;
        alignas(Align) std::array<int32_t, 2 * N> up1b;
        alignas(Align) std::array<int8_t, (2 * N) * N> up2w;
        alignas(Align) std::array<int32_t, 2 * N> up2b;
        alignas(Align) std::array<int8_t, N * N> downw;
        alignas(Align) std::array<int32_t, N> downb;
    };
    auto        b   = std::make_unique<MiniStarBlock>();
    std::mt19937 rng(0x4242u);
    std::uniform_int_distribution<int> d8(-128, 127);
    for (auto &x : b->up1w)
        x = int8_t(d8(rng));
    for (auto &x : b->up2w)
        x = int8_t(d8(rng));
    for (auto &x : b->downw)
        x = int8_t(d8(rng));
    for (int i = 0; i < 2 * N; i++)
        b->up1b[i] = b->up2b[i] = 100 * i + 7;
    for (int i = 0; i < N; i++)
        b->downb[i] = 100 * i + 7;
    auto up1bOrig  = b->up1b;
    auto up2bOrig  = b->up2b;
    auto downbOrig = b->downb;

    // Act: exactly the loader's sequence.
    simd::preprocessLinear<2 * N, N>(b->up1w.data());
    simd::preprocessLinear<2 * N, N>(b->up2w.data());
    mix9svq::foldValueCornerDownInputBias(b->downw.data(), b->downb.data(), N, N);
    simd::preprocessLinear<N, N>(b->downw.data());

    // Assert
    assert(b->up1b == up1bOrig && "value_corner_up1 bias must be untouched");
    assert(b->up2b == up2bOrig && "value_corner_up2 bias must be untouched");
#if defined(USE_VNNI)
    bool anyChanged = false;
    for (int i = 0; i < N; i++)
        anyChanged |= (b->downb[i] != downbOrig[i]);
    assert(anyChanged && "value_corner_down bias must be folded on a VNNI build");
#else
    assert(b->downb == downbOrig && "non-VNNI build must not fold any bias");
#endif
}

void preprocess_noVnniBuild_biasUnchanged()
{
#if !defined(USE_VNNI)
    int8_t  weight[N * N];
    int32_t bias[N], biasOrig[N];
    std::mt19937 rng(0x9090u);
    std::uniform_int_distribution<int> d8(-128, 127);
    for (auto &w : weight)
        w = int8_t(d8(rng));
    for (int i = 0; i < N; i++)
        bias[i] = biasOrig[i] = 12345 - 77 * i;

    mix9svq::foldValueCornerDownInputBias(weight, bias, N, N);

    for (int i = 0; i < N; i++)
        assert(bias[i] == biasOrig[i] && "non-VNNI build must leave the bias as-is");
#endif
}

// ===========================================================================
// starBlock-level equivalence (value_corner_down inside the mix9svq value head)

// A stand-in for mix9svq::starBlock() built from the public simd primitives:
// linear(up1) -> crelu -> linear(up2) -> crelu(NoReLU) -> dot2 -> linear(down) -> crelu.
// `useBiasTrick` selects the biased value_corner_down kernel + folded bias.
void starBlockLike(int8_t        output[N],
                   const int8_t  input[N],
                   const int8_t  up1w[(2 * N) * N],
                   const int32_t up1b[2 * N],
                   const int8_t  up2w[(2 * N) * N],
                   const int32_t up2b[2 * N],
                   const int8_t  downwScrambled[N * N],
                   const int32_t downb[N],
                   bool          useBiasTrick)
{
    alignas(Align) int32_t upi32[2 * N];
    alignas(Align) int8_t  up1[2 * N], up2[2 * N];
    simd::linear<2 * N, N>(upi32, input, up1w, up1b);
    simd::crelu<2 * N, 128>(up1, upi32);
    simd::linear<2 * N, N>(upi32, input, up2w, up2b);
    simd::crelu<2 * N, 128, true>(up2, upi32);

    alignas(Align) int8_t dotsum[N];
    simd::dot2<N, 128>(dotsum, up1, up2);

    alignas(Align) int32_t outi32[N];
    if (useBiasTrick)
        simd::linear<N, N, true, true, false, false, /*InputBiased128=*/true>(outi32,
                                                                              dotsum,
                                                                              downwScrambled,
                                                                              downb);
    else
        simd::linear<N, N, true>(outi32, dotsum, downwScrambled, downb);
    simd::crelu<N, 128>(output, outi32);
}

void starBlock_producesSameOutputWithAndWithoutBiasTrick()
{
    // Arrange: one set of randomized weights; value_corner_down weight in the
    // scrambled inference layout; two bias variants (folded vs original).
    std::mt19937 rng(0xB10Cu);
    std::uniform_int_distribution<int> d8(-128, 127);
    std::uniform_int_distribution<int32_t> d32(-50000, 50000);

    struct Buf
    {
        alignas(Align) std::array<int8_t, (2 * N) * N> up1w, up2w;
        alignas(Align) std::array<int32_t, 2 * N> up1b, up2b;
        alignas(Align) std::array<int8_t, N * N> downwRowMajor, downwScrambled;
        alignas(Align) std::array<int32_t, N> downb, downbFolded;
        alignas(Align) std::array<int8_t, N> input;
    };
    auto buf = std::make_unique<Buf>();
    for (auto &x : buf->up1w)
        x = int8_t(d8(rng));
    for (auto &x : buf->up2w)
        x = int8_t(d8(rng));
    for (auto &x : buf->downwRowMajor)
        x = int8_t(d8(rng));
    for (auto &x : buf->input)
        x = int8_t(d8(rng));
    for (int i = 0; i < 2 * N; i++) {
        buf->up1b[i] = d32(rng);
        buf->up2b[i] = d32(rng);
    }
    for (int i = 0; i < N; i++)
        buf->downb[i] = d32(rng);

    loaderBias<N, N>(buf->downbFolded.data(), buf->downwRowMajor.data(), buf->downb.data());

    buf->downwScrambled = buf->downwRowMajor;
    simd::preprocessLinear<N, N>(buf->downwScrambled.data());
    // up1/up2 weights also consumed in scrambled layout.
    simd::preprocessLinear<2 * N, N>(buf->up1w.data());
    simd::preprocessLinear<2 * N, N>(buf->up2w.data());

    // Act
    alignas(Align) int8_t outTrick[N], outPlain[N];
    starBlockLike(outTrick,
                  buf->input.data(),
                  buf->up1w.data(),
                  buf->up1b.data(),
                  buf->up2w.data(),
                  buf->up2b.data(),
                  buf->downwScrambled.data(),
                  buf->downbFolded.data(),
                  /*useBiasTrick=*/true);
    starBlockLike(outPlain,
                  buf->input.data(),
                  buf->up1w.data(),
                  buf->up1b.data(),
                  buf->up2w.data(),
                  buf->up2b.data(),
                  buf->downwScrambled.data(),
                  buf->downb.data(),
                  /*useBiasTrick=*/false);

    // Assert
    for (int i = 0; i < N; i++)
        assert(outTrick[i] == outPlain[i] && "starBlock output differs with the +128 bias trick");
}

}  // namespace

void runSimdLinearTests()
{
    std::printf("-- simd::linear (EVAL-01 +128 input-bias) --\n");
    RUN_TEST(linear_biasedSignedInput_randomData_matchesScalarReference);
    RUN_TEST(linear_biasedSignedInput_inputAllMinus128_matchesScalarReference);
    RUN_TEST(linear_biasedSignedInput_inputAll127_matchesScalarReference);
    RUN_TEST(linear_biasedSignedInput_inputAllZero_returnsBiasExactly);
    RUN_TEST(linear_biasedSignedInput_weightExtremes_matchesScalarReference);
    RUN_TEST(linear_biasedSignedInput_equalsUnbiasedSignedInputPath);
    RUN_TEST(linear_biasedSignedInput_noVnniBuild_pathIsUnbiasedAndStillCorrect);
    RUN_TEST(preprocess_valueCornerDown_biasFoldedByMinus128TimesRowSum);
    RUN_TEST(preprocess_valueCornerDown_weightScrambleStillApplied);
    RUN_TEST(preprocess_valueCornerUp1Up2_biasUnchanged);
    RUN_TEST(preprocess_noVnniBuild_biasUnchanged);
    RUN_TEST(starBlock_producesSameOutputWithAndWithoutBiasTrick);
}
