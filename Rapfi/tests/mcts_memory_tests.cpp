/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2024  Rapfi developers
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

// SEARCH-02 tests (docs/todo/SEARCH-02-mcts-memory-growth.md): edge-count selection, expansion
// delay predicate and Node::createEdgesFrom.

#include "testutil.h"

#include "../search/mcts/node.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Search::MCTS;

namespace {

const float kPolicy[] = {0.5f, 0.25f, 0.125f, 0.0625f, 0.0625f};

void selectEdgeCount_massOne_keepsAllEdges()
{
    assert(selectEdgeCount(kPolicy, 5, 1.0f, 1) == 5);
    assert(selectEdgeCount(kPolicy, 5, 1.5f, 1) == 5);
}

void selectEdgeCount_massReachedMidList_cutsAfterTheEdgeThatReachesMass()
{
    // cumulative: .5 .75 .875 .9375 1.0
    assert(selectEdgeCount(kPolicy, 5, 0.75f, 1) == 2);
    assert(selectEdgeCount(kPolicy, 5, 0.8f, 1) == 3);
    assert(selectEdgeCount(kPolicy, 5, 0.9f, 1) == 4);
}

void selectEdgeCount_massReachedBeforeMinEdges_keepsMinEdges()
{
    assert(selectEdgeCount(kPolicy, 5, 0.5f, 4) == 4);
}

void selectEdgeCount_fewerMovesThanMinEdges_keepsAllMoves()
{
    assert(selectEdgeCount(kPolicy, 3, 0.1f, 8) == 3);
}

void selectEdgeCount_zeroMoves_returnsZero()
{
    assert(selectEdgeCount(kPolicy, 0, 0.5f, 8) == 0);
}

void selectEdgeCount_minEdgesZero_keepsAtLeastOneEdge()
{
    assert(selectEdgeCount(kPolicy, 5, 0.1f, 0) == 1);
}

void selectEdgeCount_massNonPositive_keepsOnlyMinEdges()
{
    assert(selectEdgeCount(kPolicy, 5, 0.0f, 2) == 2);
    assert(selectEdgeCount(kPolicy, 5, -1.0f, 2) == 2);
}

void readyToExpand_defaultMinVisits_trueFromFirstVisit()
{
    assert(readyToExpand(1, 1));
}

void readyToExpand_belowMinVisits_false()
{
    assert(!readyToExpand(1, 3));
    assert(!readyToExpand(2, 3));
}

void readyToExpand_atMinVisits_true()
{
    assert(readyToExpand(3, 3));
    assert(readyToExpand(10, 3));
}

void readyToExpand_nonPositiveMinVisits_alwaysTrue()
{
    assert(readyToExpand(0, 0));
    assert(readyToExpand(1, -5));
}

std::vector<Pos> makeMoves(int n)
{
    std::vector<Pos> m;
    for (int i = 0; i < n; i++)
        m.push_back(Pos(i, 0));
    return m;
}

void createEdgesFrom_noLimits_copiesAllMovesAndPolicies()
{
    Node             node(HashKey(1), 0);
    std::vector<Pos> moves = makeMoves(5);
    std::atomic<size_t> counter {0};

    bool mated = node.createEdgesFrom(moves.data(), kPolicy, 5, EdgeLimits {}, &counter);

    assert(!mated);
    assert(!node.isLeaf());
    EdgeArray *edges = node.getEdges();
    assert(edges->numEdges == 5);
    for (uint32_t i = 0; i < 5; i++) {
        assert((*edges)[i].getMove() == moves[i]);
        assert(std::fabs((*edges)[i].getP() - kPolicy[i]) < 1e-3f);
    }
    assert(counter.load() == edgeArrayFootprint(5));
}

void createEdgesFrom_massCut_keepsPrefixAndRenormalisesToOne()
{
    Node             node(HashKey(2), 0);
    std::vector<Pos> moves = makeMoves(5);

    node.createEdgesFrom(moves.data(), kPolicy, 5, EdgeLimits {0.75f, 1}, nullptr);

    EdgeArray *edges = node.getEdges();
    assert(edges->numEdges == 2);
    float sum = 0.0f;
    for (uint32_t i = 0; i < 2; i++) {
        assert((*edges)[i].getMove() == moves[i]);
        sum += (*edges)[i].getP();
    }
    assert(std::fabs(sum - 1.0f) < 2e-3f);
    // 0.5 / 0.75 and 0.25 / 0.75
    assert(std::fabs((*edges)[0].getP() - 2.0f / 3.0f) < 2e-3f);
}

void createEdgesFrom_massCut_countsFootprintOfKeptEdgesOnly()
{
    Node                node(HashKey(3), 0);
    std::vector<Pos>    moves = makeMoves(5);
    std::atomic<size_t> counter {0};

    node.createEdgesFrom(moves.data(), kPolicy, 5, EdgeLimits {0.75f, 1}, &counter);

    assert(counter.load() == edgeArrayFootprint(2));
}

void createEdgesFrom_emptyList_returnsTrueAndLeavesNodeLeaf()
{
    Node                node(HashKey(4), 0);
    std::atomic<size_t> counter {0};

    bool mated = node.createEdgesFrom(nullptr, nullptr, 0, EdgeLimits {}, &counter);

    assert(mated);
    assert(node.isLeaf());
    assert(counter.load() == 0);
}

void createEdgesFrom_secondCall_keepsFirstArrayAndAddsNoFootprint()
{
    Node                node(HashKey(5), 0);
    std::vector<Pos>    moves = makeMoves(5);
    std::atomic<size_t> counter {0};

    node.createEdgesFrom(moves.data(), kPolicy, 5, EdgeLimits {}, &counter);
    EdgeArray *first = node.getEdges();
    node.createEdgesFrom(moves.data(), kPolicy, 3, EdgeLimits {}, &counter);

    assert(node.getEdges() == first);
    assert(first->numEdges == 5);
    assert(counter.load() == edgeArrayFootprint(5));
}

}  // namespace

void runMctsMemoryTests()
{
    std::printf("\n== SEARCH-02 MCTS memory ==\n");
    RUN_TEST(selectEdgeCount_massOne_keepsAllEdges);
    RUN_TEST(selectEdgeCount_massReachedMidList_cutsAfterTheEdgeThatReachesMass);
    RUN_TEST(selectEdgeCount_massReachedBeforeMinEdges_keepsMinEdges);
    RUN_TEST(selectEdgeCount_fewerMovesThanMinEdges_keepsAllMoves);
    RUN_TEST(selectEdgeCount_zeroMoves_returnsZero);
    RUN_TEST(selectEdgeCount_minEdgesZero_keepsAtLeastOneEdge);
    RUN_TEST(selectEdgeCount_massNonPositive_keepsOnlyMinEdges);
    RUN_TEST(readyToExpand_defaultMinVisits_trueFromFirstVisit);
    RUN_TEST(readyToExpand_belowMinVisits_false);
    RUN_TEST(readyToExpand_atMinVisits_true);
    RUN_TEST(readyToExpand_nonPositiveMinVisits_alwaysTrue);
    RUN_TEST(createEdgesFrom_noLimits_copiesAllMovesAndPolicies);
    RUN_TEST(createEdgesFrom_massCut_keepsPrefixAndRenormalisesToOne);
    RUN_TEST(createEdgesFrom_massCut_countsFootprintOfKeptEdgesOnly);
    RUN_TEST(createEdgesFrom_emptyList_returnsTrueAndLeavesNodeLeaf);
    RUN_TEST(createEdgesFrom_secondCall_keepsFirstArrayAndAddsNoFootprint);
}
