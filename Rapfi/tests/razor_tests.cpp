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

// SEARCH-01 tests (docs/todo/SEARCH-01-oppo4-explicit-threat-node.md): razorThreatNode must keep
// the razoring decision that search() made when eval was left at VALUE_NONE at nodes facing a
// flex-four threat. The hand-worked boundaries depend on the tuned Razor* tables, so re-tuning them
// must update these cases on purpose.

#include "testutil.h"

#include "../core/types.h"
#include "../search/ab/parameter.h"
#include "../search/ab/searcher.h"

#include <cassert>
#include <cstdio>

using Search::AB::razorMargin;
using Search::AB::razorThreatNode;

namespace {

/// The expression search() used before SEARCH-01, frozen, with eval left at the sentinel.
template <Rule R>
bool baseRazorDecision(Depth depth, Value alpha)
{
    Value eval = VALUE_NONE;
    return eval + razorMargin<R>(depth) < alpha;
}

template <Rule R>
void assertNoDropUpToThreshold()
{
    const Depth depths[] = {2.6f, 3.0f, 8.0f, 30.0f};
    const Value alphas[] = {Value(-30001), Value(-6000), Value(0), Value(2765)};

    for (Depth depth : depths)
        for (Value alpha : alphas)
            assert(!razorThreatNode<R>(depth, alpha));
}

template <Rule R>
void assertDropsAboveThreshold()
{
    const Depth depths[] = {2.6f, 3.0f, 8.0f, 30.0f};
    const Value alphas[] = {Value(2766), Value(29500)};

    for (Depth depth : depths)
        for (Value alpha : alphas)
            assert(razorThreatNode<R>(depth, alpha));
}

template <Rule R>
void assertMatchesBaseSweep()
{
    for (int tenths = 0; tenths <= 300; tenths++) {
        const Depth depth = tenths * 0.1f;
        for (int a = -30002; a <= 30001; a++)
            assert(razorThreatNode<R>(depth, Value(a)) == baseRazorDecision<R>(depth, Value(a)));
    }
}

void razorThreatNode_standardBelowRazorDepth_dropsForEveryReachableAlpha()
{
    // Arrange
    const Depth depths[] = {0.5f, 1.0f, 2.0f, 2.59f};
    const Value alphas[] = {Value(-30001),
                            Value(-29500),
                            Value(-6000),
                            Value(0),
                            Value(6000),
                            Value(29500),
                            Value(30001)};

    // Act and assert
    for (Depth depth : depths)
        for (Value alpha : alphas)
            assert(razorThreatNode<STANDARD>(depth, alpha));
}

void razorThreatNode_standardBelowRazorDepth_alphaEqualToSentinel_doesNotDrop()
{
    // Arrange
    const Depth depth = 2.0f;

    // Act
    const bool atSentinel    = razorThreatNode<STANDARD>(depth, Value(-30002));
    const bool aboveSentinel = razorThreatNode<STANDARD>(depth, Value(-30001));

    // Assert
    assert(!atSentinel);
    assert(aboveSentinel);
}

void razorThreatNode_atOrAboveRazorDepth_noDropUpToThreshold()
{
    assertNoDropUpToThreshold<FREESTYLE>();
    assertNoDropUpToThreshold<STANDARD>();
    assertNoDropUpToThreshold<RENJU>();
}

void razorThreatNode_atOrAboveRazorDepth_dropsAboveThreshold()
{
    assertDropsAboveThreshold<FREESTYLE>();
    assertDropsAboveThreshold<STANDARD>();
    assertDropsAboveThreshold<RENJU>();
}

void razorThreatNode_freestyleShallow_noDropAtVeryShortMatedAlpha()
{
    // Margin at d = 1 is int(0.17 + 15) + 11 = 26; at d = 2.5, int(1.0625 + 37.5) + 11 = 49.
    assert(!razorThreatNode<FREESTYLE>(1.0f, Value(-29976)));
    assert(razorThreatNode<FREESTYLE>(1.0f, Value(-29975)));
    assert(!razorThreatNode<FREESTYLE>(2.5f, Value(-29953)));
    assert(razorThreatNode<FREESTYLE>(2.5f, Value(-29952)));
}

void razorThreatNode_renjuShallow_noDropAtVeryShortMatedAlpha()
{
    // Margin at d = 1 is int(0.17 + 8) + 9 = 17; at d = 2.5, int(1.0625 + 20) + 9 = 30.
    assert(!razorThreatNode<RENJU>(1.0f, Value(-29985)));
    assert(razorThreatNode<RENJU>(1.0f, Value(-29984)));
    assert(!razorThreatNode<RENJU>(2.5f, Value(-29972)));
    assert(razorThreatNode<RENJU>(2.5f, Value(-29971)));
}

void razorThreatNode_allRules_matchesBaseExpressionWithSentinelEval()
{
    assertMatchesBaseSweep<FREESTYLE>();
    assertMatchesBaseSweep<STANDARD>();
    assertMatchesBaseSweep<RENJU>();
}

}  // namespace

void runRazorTests()
{
    std::printf("\n== SEARCH-01 razorThreatNode ==\n");
    RUN_TEST(razorThreatNode_standardBelowRazorDepth_dropsForEveryReachableAlpha);
    RUN_TEST(razorThreatNode_standardBelowRazorDepth_alphaEqualToSentinel_doesNotDrop);
    RUN_TEST(razorThreatNode_atOrAboveRazorDepth_noDropUpToThreshold);
    RUN_TEST(razorThreatNode_atOrAboveRazorDepth_dropsAboveThreshold);
    RUN_TEST(razorThreatNode_freestyleShallow_noDropAtVeryShortMatedAlpha);
    RUN_TEST(razorThreatNode_renjuShallow_noDropAtVeryShortMatedAlpha);
    RUN_TEST(razorThreatNode_allRules_matchesBaseExpressionWithSentinelEval);
}
