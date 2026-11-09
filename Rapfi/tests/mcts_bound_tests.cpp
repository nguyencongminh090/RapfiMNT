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

// SEARCH-09 tests (docs/todo/SEARCH-09-mcts-bound-under-edge-cut.md): a node whose edge list was
// cut by the policy-mass rule (SEARCH-02/05) must not become a proven loss (or a capped draw) from
// its kept children alone; the cut moves count as one unexplored child with bound (-inf, +inf).
//
// Set RAPFI_TEST_ISOLATE=1 to run each test of this suite in a forked child, so that one failing
// assert reports [FAIL] and the suite continues (used to show the tests fail before the change).

#include "testutil.h"

#include "../search/mcts/node.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace Search::MCTS;

namespace {

// ----------------------------------------------------------------------------------------------
// Fixture: a parent with 12 generated moves sorted by descending policy (sum 1).
// With CutLimits the cumulative mass reaches 0.9 at move 6, the 8-edge floor wins: 8 kept, 4 cut.

constexpr uint32_t NumMoves = 12;
constexpr uint32_t NumKept  = 8;

const Pos   FixtureMoves[NumMoves]  = {Pos(1, 1), Pos(2, 1), Pos(3, 1), Pos(4, 1),
                                       Pos(5, 1), Pos(6, 1), Pos(7, 1), Pos(8, 1),
                                       Pos(9, 1), Pos(10, 1), Pos(11, 1), Pos(12, 1)};
const float FixturePolicy[NumMoves] = {0.50f, 0.20f, 0.10f, 0.05f, 0.04f, 0.03f,
                                       0.02f, 0.02f, 0.01f, 0.01f, 0.01f, 0.01f};

const EdgeLimits CutLimits {0.9f, 8};
const EdgeLimits NoCutLimits {};  // massKeep 1: keep every move

/// Builds an expanded, non-terminal parent node with the fixture move list.
std::unique_ptr<Node> makeParent(const EdgeLimits &limits)
{
    auto parent = std::make_unique<Node>(HashKey(100), 0);
    parent->setNonTerminal(0.0f, 0.1f);
    parent->createEdgesFrom(FixtureMoves, FixturePolicy, NumMoves, limits);
    return parent;
}

/// Children owned by the test; a fresh child per edge, all visited once.
struct Children
{
    std::vector<std::unique_ptr<Node>> nodes;

    /// Attaches one child per edge of parent. valueOf(i) returns the terminal value of child i,
    /// or VALUE_NONE for a non-terminal child with a neutral evaluation.
    template <typename F>
    void attachAll(Node &parent, F valueOf)
    {
        EdgeArray &edges = *parent.getEdges();
        for (uint32_t i = 0; i < edges.numEdges; i++) {
            nodes.push_back(std::make_unique<Node>(HashKey(200 + i), 0));
            Node &child = *nodes.back();
            Value v     = valueOf(i);
            if (v == VALUE_NONE)
                child.setNonTerminal(0.0f, 0.2f);
            else
                child.setTerminal(v);
            edges[i].setChild(&child);
            edges[i].addVisits(1);
        }
    }
};

// ----------------------------------------------------------------------------------------------
// Node::updateStats

void updateStats_allKeptChildrenProvenWinForOpponentEdgesCut_noProvenLoss()
{
    // Arrange: every kept reply is a proven win for the opponent; 4 low-policy moves were cut.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attachAll(*parent, [](uint32_t) { return mate_in(3); });

    // Act
    parent->updateStats();

    // Assert: a cut move may still save the node, so the upper bound stays open.
    ValueBound b = parent->getBound();
    assert(b.upper == VALUE_INFINITE);
    assert(b.lower == mated_in(3));
    assert(!b.isTerminal());
}

void updateStats_allChildrenProvenWinForOpponentNoCut_provenLoss()
{
    // Arrange: same replies, nothing cut.
    auto     parent = makeParent(NoCutLimits);
    Children children;
    children.attachAll(*parent, [](uint32_t) { return mate_in(3); });

    // Act
    parent->updateStats();

    // Assert: every reply loses, so the node is a proven loss (behaviour before SEARCH-09).
    ValueBound b = parent->getBound();
    assert(b.upper == mated_in(3));
    assert(b.lower == mated_in(3));
    assert(b.isTerminal());
}

void updateStats_oneChildProvenLossForOpponentEdgesCut_provenWinKept()
{
    // Arrange: kept reply 2 is a proven loss for the opponent; the others are open.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attachAll(*parent, [](uint32_t i) { return i == 2 ? mated_in(2) : VALUE_NONE; });

    // Act
    parent->updateStats();

    // Assert: one winning reply is a proof whether or not siblings were cut.
    ValueBound b = parent->getBound();
    assert(b.lower == mate_in(2));
    assert(b.upper == VALUE_INFINITE);
}

void updateStats_edgesCut_keptChildProvenLoss_parentWinLowerUnchanged()
{
    // Arrange: kept reply 0 is a proven loss for the opponent, all other kept replies proven wins.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attachAll(*parent, [](uint32_t i) { return i == 0 ? mated_in(2) : mate_in(3); });

    // Act
    parent->updateStats();

    // Assert: the cut fold leaves the proof direction (lower) alone and only opens upper.
    ValueBound b = parent->getBound();
    assert(b.lower == mate_in(2));
    assert(b.upper == VALUE_INFINITE);
}

void updateStats_edgesCut_allKeptChildrenDraw_upperNotCapped()
{
    // Arrange: every kept reply is a proven draw.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attachAll(*parent, [](uint32_t) { return VALUE_DRAW; });

    // Act
    parent->updateStats();

    // Assert: a cut move may win, so the node is not capped at a draw.
    ValueBound b = parent->getBound();
    assert(b.lower == VALUE_DRAW);
    assert(b.upper == VALUE_INFINITE);
}

// ----------------------------------------------------------------------------------------------
// Node::createEdgesFrom

void createEdgesFrom_cutApplied_setsCutFlag()
{
    // Arrange
    Node parent(HashKey(300), 0);
    parent.setNonTerminal(0.0f, 0.1f);

    // Act
    parent.createEdgesFrom(FixtureMoves, FixturePolicy, NumMoves, CutLimits);

    // Assert
    const EdgeArray &edges = *parent.getEdges();
    assert(edges.numEdges == NumKept);
    assert(edges.numMoves == NumMoves);
    assert(edges.edgesCut());
}

void createEdgesFrom_noCut_cutFlagClear()
{
    // Arrange
    Node parent(HashKey(301), 0);
    parent.setNonTerminal(0.0f, 0.1f);

    // Act
    parent.createEdgesFrom(FixtureMoves, FixturePolicy, NumMoves, NoCutLimits);

    // Assert
    const EdgeArray &edges = *parent.getEdges();
    assert(edges.numEdges == NumMoves);
    assert(edges.numMoves == NumMoves);
    assert(!edges.edgesCut());
}

void createEdgesFrom_minEdgesFloorCoversAllMoves_cutFlagClear()
{
    // Arrange: massKeep < 1 but only minEdges (8) moves exist, so the floor keeps all of them.
    Node        parent(HashKey(302), 0);
    const float policy[NumKept] = {0.6f, 0.2f, 0.1f, 0.04f, 0.03f, 0.01f, 0.01f, 0.01f};
    parent.setNonTerminal(0.0f, 0.1f);

    // Act
    parent.createEdgesFrom(FixtureMoves, policy, NumKept, EdgeLimits {0.5f, 8});

    // Assert
    const EdgeArray &edges = *parent.getEdges();
    assert(edges.numEdges == NumKept);
    assert(edges.numMoves == NumKept);
    assert(!edges.edgesCut());
}

// ----------------------------------------------------------------------------------------------
// Layout

void edgeArray_sizeUnchanged()
{
    // Arrange / Act
    constexpr size_t edgeArraySize = sizeof(EdgeArray);
    constexpr size_t nodeSize      = sizeof(Node);

    // Assert: the cut count lives in EdgeArray's padding; Node keeps its SEARCH-08 size.
    assert(edgeArraySize == 8);
    if (sizeof(void *) == 8)
        assert(nodeSize == 56);
}

// ----------------------------------------------------------------------------------------------

bool isolateMode()
{
    const char *env = std::getenv("RAPFI_TEST_ISOLATE");
    return env && env[0] == '1';
}

#if defined(__unix__) || defined(__APPLE__)
    #define RUN_BOUND_TEST(fn)                                                  \
        do {                                                                    \
            if (!isolateMode()) {                                               \
                RUN_TEST(fn);                                                   \
                break;                                                          \
            }                                                                   \
            ++testsRun;                                                         \
            std::fflush(stdout);                                                \
            pid_t pid = fork();                                                 \
            if (pid == 0) {                                                     \
                fn();                                                           \
                std::fflush(stdout);                                            \
                _exit(0);                                                       \
            }                                                                   \
            int status = 0;                                                     \
            waitpid(pid, &status, 0);                                           \
            bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;            \
            if (ok)                                                             \
                ++testsPassed;                                                  \
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", #fn);                \
            std::fflush(stdout);                                                \
        } while (0)
#else
    #define RUN_BOUND_TEST(fn) RUN_TEST(fn)
#endif

}  // namespace

void runMctsBoundTests()
{
    RUN_BOUND_TEST(updateStats_allKeptChildrenProvenWinForOpponentEdgesCut_noProvenLoss);
    RUN_BOUND_TEST(updateStats_allChildrenProvenWinForOpponentNoCut_provenLoss);
    RUN_BOUND_TEST(updateStats_oneChildProvenLossForOpponentEdgesCut_provenWinKept);
    RUN_BOUND_TEST(updateStats_edgesCut_keptChildProvenLoss_parentWinLowerUnchanged);
    RUN_BOUND_TEST(updateStats_edgesCut_allKeptChildrenDraw_upperNotCapped);
    RUN_BOUND_TEST(createEdgesFrom_cutApplied_setsCutFlag);
    RUN_BOUND_TEST(createEdgesFrom_noCut_cutFlagClear);
    RUN_BOUND_TEST(createEdgesFrom_minEdgesFloorCoversAllMoves_cutFlagClear);
    RUN_BOUND_TEST(edgeArray_sizeUnchanged);
}
