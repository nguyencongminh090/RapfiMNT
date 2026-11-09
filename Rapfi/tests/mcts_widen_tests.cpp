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

// SEARCH-10 tests (docs/todo/SEARCH-10-mcts-readd-cut-moves.md): when every kept reply of a cut
// node is a proven win for the opponent, the node reports "widen needed" and its edge array is
// replaced by one holding every generated move, so the node can be proven lost again.
//
// Set RAPFI_TEST_ISOLATE=1 to run each test of this suite in a forked child, so that one failing
// assert reports [FAIL] and the suite continues (used to show the tests fail before the change).

#include "testutil.h"

#include "../search/mcts/node.h"
#include "../search/mcts/searcher.h"

#include <atomic>
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

/// A regenerated full list in another order: moves 1 and 2 swap places, 11 and 12 too. Used to
/// check that kept edges are matched by move, not by index.
const Pos   ShuffledMoves[NumMoves]  = {Pos(2, 1), Pos(1, 1), Pos(3, 1), Pos(4, 1),
                                        Pos(5, 1), Pos(6, 1), Pos(7, 1), Pos(8, 1),
                                        Pos(9, 1), Pos(10, 1), Pos(12, 1), Pos(11, 1)};
const float ShuffledPolicy[NumMoves] = {0.50f, 0.20f, 0.10f, 0.05f, 0.04f, 0.03f,
                                        0.02f, 0.02f, 0.015f, 0.01f, 0.008f, 0.007f};

const EdgeLimits CutLimits {0.9f, 8};
const EdgeLimits NoCutLimits {};  // massKeep 1: keep every move

/// Builds an expanded, non-terminal parent node with the fixture move list.
std::unique_ptr<Node> makeParent(const EdgeLimits &limits, std::atomic<size_t> *counter = nullptr)
{
    auto parent = std::make_unique<Node>(HashKey(100), 0);
    parent->setNonTerminal(0.0f, 0.1f);
    parent->createEdgesFrom(FixtureMoves, FixturePolicy, NumMoves, limits, counter);
    return parent;
}

/// The quantized policy an edge stores for p.
float quantized(float p)
{
    return Edge(Pos(0, 0), p).getP();
}

/// Index of move in a move list, or -1.
int indexOf(const Pos *moves, uint32_t n, Pos move)
{
    for (uint32_t i = 0; i < n; i++)
        if (moves[i] == move)
            return int(i);
    return -1;
}

/// Children owned by the test; a fresh child per edge.
struct Children
{
    std::vector<std::unique_ptr<Node>> nodes;

    /// Attaches one child to edges [from, numEdges) of parent and gives edge i (i + 1) visits.
    /// valueOf(i) returns the terminal value of child i, or VALUE_NONE for a non-terminal child.
    template <typename F>
    void attach(Node &parent, F valueOf, uint32_t from = 0)
    {
        EdgeArray &edges = *parent.getEdges();
        for (uint32_t i = from; i < edges.numEdges; i++) {
            nodes.push_back(std::make_unique<Node>(HashKey(200 + i), 0));
            Node &child = *nodes.back();
            Value v     = valueOf(i);
            if (v == VALUE_NONE)
                child.setNonTerminal(0.0f, 0.2f);
            else
                child.setTerminal(v);
            edges[i].setChild(&child);
            edges[i].addVisits(i + 1);
        }
    }
};

// ----------------------------------------------------------------------------------------------
// Node::updateStats (widen trigger)

void updateStats_edgesCutAllKeptChildrenProvenWinForOpponent_reportsWidenNeeded()
{
    // Arrange: every kept reply is a proven win for the opponent; 4 low-policy moves were cut.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });

    // Act
    bool widen = parent->updateStats();

    // Assert: only a cut move can still save the node; the SEARCH-09 fold keeps it unproven.
    assert(widen);
    assert(parent->getBound().upper == VALUE_INFINITE);
}

void updateStats_edgesCutOneKeptChildUnknown_noWidenNeeded()
{
    // Arrange: kept reply 5 is open, the other kept replies are proven wins for the opponent.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t i) { return i == 5 ? VALUE_NONE : mate_in(3); });

    // Act
    bool widen = parent->updateStats();

    // Assert: a kept reply may still save the node, so there is nothing to widen for.
    assert(!widen);
}

void updateStats_noCutAllChildrenProvenWinForOpponent_noWidenNeededProvenLoss()
{
    // Arrange: every reply loses, nothing was cut.
    auto     parent = makeParent(NoCutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });

    // Act
    bool widen = parent->updateStats();

    // Assert: the full list already proves the loss.
    assert(!widen);
    assert(parent->getBound().upper == mated_in(3));
    assert(parent->getBound().isTerminal());
}

void updateStats_edgesCutUnvisitedKeptEdge_noWidenNeeded()
{
    // Arrange: kept replies 0..5 are proven wins for the opponent, 6 and 7 are unvisited.
    auto       parent = makeParent(CutLimits);
    Children   children;
    EdgeArray &edges = *parent->getEdges();
    for (uint32_t i = 0; i < 6; i++) {
        children.nodes.push_back(std::make_unique<Node>(HashKey(200 + i), 0));
        children.nodes.back()->setTerminal(mate_in(3));
        edges[i].setChild(children.nodes.back().get());
        edges[i].addVisits(1);
    }

    // Act
    bool widen = parent->updateStats();

    // Assert: an unexplored kept reply is still open.
    assert(!widen);
}

// ----------------------------------------------------------------------------------------------
// Node::replaceEdgesWithFull

void replaceEdgesWithFull_keptEdgesFirst_visitsAndChildrenCopied()
{
    // Arrange: a cut parent with visited kept edges; the regenerated list comes in another order.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    EdgeArray        *old = parent->getEdges();
    RetiredEdgeArrays retired;

    // Act
    bool ok =
        parent->replaceEdgesWithFull(old, ShuffledMoves, ShuffledPolicy, NumMoves, nullptr, retired);

    // Assert: the kept edges keep their old order, visits and children.
    assert(ok);
    EdgeArray &edges = *parent->getEdges();
    assert(&edges != old);
    for (uint32_t i = 0; i < NumKept; i++) {
        assert(edges[i].getMove() == FixtureMoves[i]);
        assert(edges[i].getVisits() == i + 1);
        assert(edges[i].getChild() == children.nodes[i].get());
    }
}

void replaceEdgesWithFull_cutMovesAppendedUnvisitedInPolicyOrder()
{
    // Arrange
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    EdgeArray        *old = parent->getEdges();
    RetiredEdgeArrays retired;

    // Act
    bool ok =
        parent->replaceEdgesWithFull(old, ShuffledMoves, ShuffledPolicy, NumMoves, nullptr, retired);

    // Assert: the 4 cut moves follow in the regenerated (descending policy) order, unvisited.
    assert(ok);
    EdgeArray &edges       = *parent->getEdges();
    const Pos  appended[4] = {Pos(9, 1), Pos(10, 1), Pos(12, 1), Pos(11, 1)};
    for (uint32_t k = 0; k < 4; k++) {
        const Edge &e = edges[NumKept + k];
        assert(e.getMove() == appended[k]);
        assert(e.getVisits() == 0);
        assert(e.getChild() == nullptr);
    }
}

void replaceEdgesWithFull_success_edgesCutFalseAndPoliciesFromFullList()
{
    // Arrange
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    EdgeArray        *old = parent->getEdges();
    RetiredEdgeArrays retired;

    // Act
    bool ok =
        parent->replaceEdgesWithFull(old, ShuffledMoves, ShuffledPolicy, NumMoves, nullptr, retired);

    // Assert: every move has an edge, and every policy (kept edges too) comes from the full list,
    // matched by move, without the cut renormalisation.
    assert(ok);
    const EdgeArray &edges = *parent->getEdges();
    assert(edges.numEdges == NumMoves);
    assert(edges.numMoves == NumMoves);
    assert(!edges.edgesCut());
    for (uint32_t i = 0; i < NumMoves; i++) {
        int j = indexOf(ShuffledMoves, NumMoves, edges[i].getMove());
        assert(j >= 0);
        assert(edges[i].getP() == quantized(ShuffledPolicy[j]));
    }
    assert(edges[0].getP() == quantized(0.20f));  // move (1,1): 0.5 renormalised before
}

void replaceEdgesWithFull_oldPointerAlreadyReplaced_returnsFalseNewArrayFreed()
{
    // Arrange: a first replacement succeeds.
    std::atomic<size_t> counter {0};
    auto                parent = makeParent(CutLimits, &counter);
    Children            children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    EdgeArray        *old = parent->getEdges();
    RetiredEdgeArrays retired;
    bool              first =
        parent->replaceEdgesWithFull(old, FixtureMoves, FixturePolicy, NumMoves, &counter, retired);
    EdgeArray *current      = parent->getEdges();
    size_t     counterAfter = counter.load();

    // Act: a second thread that read the old pointer tries again.
    bool second =
        parent->replaceEdgesWithFull(old, FixtureMoves, FixturePolicy, NumMoves, &counter, retired);

    // Assert: the loser changes nothing (its temp array is freed; ASan checks the leak).
    assert(first);
    assert(!second);
    assert(parent->getEdges() == current);
    assert(counter.load() == counterAfter);
    assert(retired.count() == 1);
}

void replaceEdgesWithFull_success_oldArrayRetiredMemoryAccounted()
{
    // Arrange: the counter holds the old array's footprint, as createEdgesFrom left it.
    std::atomic<size_t> counter {0};
    auto                parent = makeParent(CutLimits, &counter);
    Children            children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    assert(counter.load() == edgeArrayFootprint(NumKept));
    EdgeArray        *old = parent->getEdges();
    RetiredEdgeArrays retired;

    // Act
    bool ok =
        parent->replaceEdgesWithFull(old, FixtureMoves, FixturePolicy, NumMoves, &counter, retired);

    // Assert: the new array is added, the old one stays counted while it is retired.
    assert(ok);
    assert(counter.load() == edgeArrayFootprint(NumKept) + edgeArrayFootprint(NumMoves));
    assert(retired.count() == 1);
    assert(retired.bytes() == edgeArrayFootprint(NumKept));
}

void replaceEdgesWithFull_keptMoveMissingFromFullList_returnsFalseNothingChanged()
{
    // Arrange: the "full" list lacks kept move (3,1).
    std::atomic<size_t> counter {0};
    auto                parent = makeParent(CutLimits, &counter);
    Children            children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    EdgeArray *old            = parent->getEdges();
    Pos        moves[NumMoves - 1];
    float      policy[NumMoves - 1];
    for (uint32_t i = 0, k = 0; i < NumMoves; i++)
        if (FixtureMoves[i] != Pos(3, 1)) {
            moves[k]  = FixtureMoves[i];
            policy[k] = FixturePolicy[i];
            k++;
        }
    size_t            counterBefore = counter.load();
    RetiredEdgeArrays retired;

    // Act
    bool ok = parent->replaceEdgesWithFull(old, moves, policy, NumMoves - 1, &counter, retired);

    // Assert: the widen is aborted and the old array stays current.
    assert(!ok);
    assert(parent->getEdges() == old);
    assert(counter.load() == counterBefore);
    assert(retired.count() == 0);
}

void updateStats_afterReplaceAllRepliesProvenWinForOpponent_provenLoss()
{
    // Arrange: widen a node whose kept replies all lose, then the cut replies are found to lose.
    auto     parent = makeParent(CutLimits);
    Children children;
    children.attach(*parent, [](uint32_t) { return mate_in(3); });
    bool              triggered = parent->updateStats();
    RetiredEdgeArrays retired;
    bool              ok = parent->replaceEdgesWithFull(parent->getEdges(),
                                           FixtureMoves,
                                           FixturePolicy,
                                           NumMoves,
                                           nullptr,
                                           retired);
    assert(triggered && ok);
    children.attach(*parent, [](uint32_t) { return mate_in(5); }, NumKept);

    // Act
    bool widen = parent->updateStats();

    // Assert: every reply loses, so the node is a proven loss (the proof SEARCH-10 enables).
    assert(!widen);
    ValueBound b = parent->getBound();
    assert(b.upper == mated_in(5));
    assert(b.lower == mated_in(5));
    assert(b.isTerminal());
}

// ----------------------------------------------------------------------------------------------
// MCTSSearcher::freeRetiredEdgeArrays

void freeRetiredEdgeArrays_releasesAllAndSubtractsFootprint()
{
    // Arrange: two widened nodes whose old arrays are retired in the searcher.
    MCTSSearcher        searcher;
    std::atomic<size_t> &used = searcher.graphMemoryUsed;
    auto                 a    = makeParent(CutLimits, &used);
    auto                 b    = makeParent(CutLimits, &used);
    Children             children;
    children.attach(*a, [](uint32_t) { return mate_in(3); });
    children.attach(*b, [](uint32_t) { return mate_in(3); });
    bool okA = a->replaceEdgesWithFull(a->getEdges(),
                                       FixtureMoves,
                                       FixturePolicy,
                                       NumMoves,
                                       &used,
                                       searcher.retiredEdgeArrays);
    bool okB = b->replaceEdgesWithFull(b->getEdges(),
                                       FixtureMoves,
                                       FixturePolicy,
                                       NumMoves,
                                       &used,
                                       searcher.retiredEdgeArrays);
    assert(okA && okB);
    assert(used.load() == 2 * (edgeArrayFootprint(NumKept) + edgeArrayFootprint(NumMoves)));

    // Act
    searcher.freeRetiredEdgeArrays();

    // Assert: only the two current arrays stay counted.
    assert(searcher.retiredEdgeArrays.count() == 0);
    assert(searcher.retiredEdgeArrays.bytes() == 0);
    assert(used.load() == 2 * edgeArrayFootprint(NumMoves));
}

// ----------------------------------------------------------------------------------------------

bool isolateMode()
{
    const char *env = std::getenv("RAPFI_TEST_ISOLATE");
    return env && env[0] == '1';
}

#if defined(__unix__) || defined(__APPLE__)
    #define RUN_WIDEN_TEST(fn)                                                  \
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
    #define RUN_WIDEN_TEST(fn) RUN_TEST(fn)
#endif

}  // namespace

void runMctsWidenTests()
{
    RUN_WIDEN_TEST(updateStats_edgesCutAllKeptChildrenProvenWinForOpponent_reportsWidenNeeded);
    RUN_WIDEN_TEST(updateStats_edgesCutOneKeptChildUnknown_noWidenNeeded);
    RUN_WIDEN_TEST(updateStats_noCutAllChildrenProvenWinForOpponent_noWidenNeededProvenLoss);
    RUN_WIDEN_TEST(updateStats_edgesCutUnvisitedKeptEdge_noWidenNeeded);
    RUN_WIDEN_TEST(replaceEdgesWithFull_keptEdgesFirst_visitsAndChildrenCopied);
    RUN_WIDEN_TEST(replaceEdgesWithFull_cutMovesAppendedUnvisitedInPolicyOrder);
    RUN_WIDEN_TEST(replaceEdgesWithFull_success_edgesCutFalseAndPoliciesFromFullList);
    RUN_WIDEN_TEST(replaceEdgesWithFull_oldPointerAlreadyReplaced_returnsFalseNewArrayFreed);
    RUN_WIDEN_TEST(replaceEdgesWithFull_success_oldArrayRetiredMemoryAccounted);
    RUN_WIDEN_TEST(replaceEdgesWithFull_keptMoveMissingFromFullList_returnsFalseNothingChanged);
    RUN_WIDEN_TEST(updateStats_afterReplaceAllRepliesProvenWinForOpponent_provenLoss);
    RUN_WIDEN_TEST(freeRetiredEdgeArrays_releasesAllAndSubtractsFootprint);
}
