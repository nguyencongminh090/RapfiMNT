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

// SEARCH-08 tests (docs/todo/SEARCH-08-mcts-64bit-visits.md): 64-bit MCTS visit counters with
// a 16-byte Edge (visit word = 48-bit visits + 16-bit policy, child word = 48-bit pointer +
// 16-bit move) and a 56-byte Node (visit word = 48-bit visits + 16-bit terminal value).
//
// Set RAPFI_TEST_ISOLATE=1 to run each test of this suite in a forked child, so that one failing
// assert reports [FAIL] and the suite continues (used to show the tests fail before the change).

#include "testutil.h"

#include "../game/board.h"
#include "../eval/evaluator.h"
#include "../search/mcts/node.h"
#include "../search/mcts/searcher.h"
#include "../search/mcts/searchtesting.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace Search::MCTS;

namespace {

constexpr uint64_t TwoPow32 = uint64_t(1) << 32;
constexpr uint64_t TwoPow48 = uint64_t(1) << 48;

// ----------------------------------------------------------------------------------------------
// Reference policy quantization, copied from the HEAD (pre-SEARCH-08) Edge::setP / Edge::getP.

uint16_t refQuantizePolicy(float p)
{
    constexpr int32_t roundings = (1 << 11) - (3 << 28);
    int32_t           bits;
    std::memcpy(&bits, &p, sizeof(float));
    bits += roundings;
    return (bits < 0) ? 0 : static_cast<uint16_t>(bits >> 12);
}

float refDecodePolicy(uint16_t policy)
{
    uint32_t bits = (static_cast<uint32_t>(policy) << 12) | (3 << 28);
    float    p;
    std::memcpy(&p, &bits, sizeof(uint32_t));
    return p;
}

bool bitEqual(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// ----------------------------------------------------------------------------------------------
// Edge

void Edge_size_is16Bytes()
{
    // Arrange / Act
    constexpr size_t size = sizeof(Edge);

    // Assert: edgeArrayFootprint, EdgeArray alignment and the graph budget assume 16 bytes.
    assert(size == 16);
    assert(edgeArrayFootprint(10) == sizeof(EdgeArray) + 160 + EdgeArrayAllocOverhead);
}

void Edge_getP_roundTripUnchangedForAllQuantizedPolicies()
{
    // Arrange: every quantized policy that decodes into [0, 1], plus a dense float sweep.
    std::vector<float> inputs;
    for (uint32_t q = 0; q <= 0xFFFF; q++) {
        float p = refDecodePolicy(static_cast<uint16_t>(q));
        if (p <= 1.0f)
            inputs.push_back(p);
    }
    for (int i = 0; i <= 100000; i++)
        inputs.push_back(i / 100000.0f);

    for (float p : inputs) {
        // Act
        Edge edge(Pos(3, 4), p);

        // Assert: same quantization as before the change, bit for bit.
        assert(bitEqual(edge.getP(), refDecodePolicy(refQuantizePolicy(p))));
    }
}

void Edge_addVisits_acrossTwoPow32_countsExactly()
{
    // Arrange
    Edge edge(Pos(7, 7), 0.25f);

    // Act
    edge.addVisits(3000000000ull);
    edge.addVisits(3000000000ull);
    edge.addVisits(7);

    // Assert
    assert(edge.getVisits() == 6000000007ull);
    assert(edge.getMove() == Pos(7, 7));
    assert(bitEqual(edge.getP(), refDecodePolicy(refQuantizePolicy(0.25f))));
}

void Edge_addVisits_returnsVisitsBeforeTheAdd()
{
    // Arrange
    Edge edge(Pos(1, 2), 0.5f);
    edge.addVisits(TwoPow32 + 3);

    // Act
    uint64_t before = edge.addVisits(10);

    // Assert: the policy bits are masked off the returned word.
    assert(before == TwoPow32 + 3);
    assert(edge.getVisits() == TwoPow32 + 13);
}

void Edge_addVisits_nearTwoPow48_policyBitsIntact()
{
    // Arrange: a policy whose quantized value has high and low bits set.
    const float policy = 0.999f;
    Edge        edge(Pos::PASS, policy);
    const float pBefore = edge.getP();

    // Act: fill the visit field to its maximum (2^48 - 1), far beyond the search guard (2^46).
    edge.addVisits(SearchVisitGuard);
    edge.addVisits(TwoPow48 - 1 - SearchVisitGuard);

    // Assert: no carry into the policy bits, move unchanged.
    assert(edge.getVisits() == TwoPow48 - 1);
    assert(bitEqual(edge.getP(), pBefore));
    assert(edge.getMove() == Pos::PASS);
    assert(SearchVisitGuard < TwoPow48 / 2);
}

void Edge_setChild_preservesMoveAndReturnsPointer()
{
    // Arrange: a negative move (PASS) exercises the sign extension of the move bits.
    Node        child(HashKey(42), 0);
    const Pos   moves[] = {Pos::PASS, Pos(0, 0), Pos(14, 14), Pos(21, 21)};
    const float policy  = 0.37f;

    for (Pos move : moves) {
        Edge edge(move, policy);
        edge.addVisits(TwoPow32 + 1);
        const float pBefore = edge.getP();

        // Act
        edge.setChild(&child);

        // Assert
        assert(edge.getChild() == &child);
        assert(edge.getMove() == move);
        assert(bitEqual(edge.getP(), pBefore));
        assert(edge.getVisits() == TwoPow32 + 1);
    }
}

void Edge_getChild_initiallyNull_andVisitsZero()
{
    // Arrange / Act
    Edge edge(Pos::PASS, 1.0f);

    // Assert
    assert(edge.getChild() == nullptr);
    assert(edge.getVisits() == 0);
    assert(edge.getMove() == Pos::PASS);
}

void Edge_concurrentAddVisits_totalIsExact()
{
    // Arrange
    Edge                     edge(Pos(5, 6), 0.125f);
    const float              pBefore    = edge.getP();
    constexpr int            NumThreads = 8;
    constexpr int            NumAdds    = 20000;
    constexpr uint64_t       BigDelta   = 600000000ull;  // 8 * 6e8 = 4.8e9 > 2^32
    std::vector<std::thread> threads;

    // Act
    for (int t = 0; t < NumThreads; t++)
        threads.emplace_back([&edge, t] {
            edge.addVisits(BigDelta);
            for (int i = 0; i < NumAdds; i++)
                edge.addVisits(1 + (i + t) % 3);
        });
    for (auto &th : threads)
        th.join();

    // Assert
    uint64_t expected = NumThreads * BigDelta;
    for (int t = 0; t < NumThreads; t++)
        for (int i = 0; i < NumAdds; i++)
            expected += 1 + (i + t) % 3;
    assert(expected > TwoPow32);
    assert(edge.getVisits() == expected);
    assert(bitEqual(edge.getP(), pBefore));
    assert(edge.getMove() == Pos(5, 6));
}

// ----------------------------------------------------------------------------------------------
// Node

void Node_size_is56BytesOn64BitTargets()
{
    // Arrange / Act
    constexpr size_t size = sizeof(Node);

    // Assert: nodes live in std::set nodes; a larger Node moves them to a bigger malloc chunk.
    if (sizeof(void *) == 8)
        assert(size == 56);
}

void Node_incrementVisits_acrossTwoPow32_getVisitsIs64Bit()
{
    // Arrange
    Node node(HashKey(1), 0);
    node.setNonTerminal(0.1f, 0.2f);

    // Act
    node.incrementVisits(3000000000ull);
    node.incrementVisits(3000000000ull);

    // Assert
    assert(node.getVisits() == 6000000001ull);
    assert(!node.isTerminal());
}

void Node_finishVisit_acrossTwoPow32_nVirtualReturnsToZero()
{
    // Arrange
    Node node(HashKey(2), 0);
    node.setNonTerminal(0.0f, 0.0f);
    node.incrementVisits(TwoPow32 - 10);
    node.beginVisit(20);

    // Act
    node.finishVisit(20, 20);

    // Assert
    assert(node.getVisits() == TwoPow32 + 11);
    assert(node.getVirtualVisits() == 0);
}

void Node_setTerminal_freshNode_visitsOneAndTerminal()
{
    // Arrange
    Node node(HashKey(3), 0);

    // Act
    node.setTerminal(mated_in(4));

    // Assert
    assert(node.getVisits() == 1);
    assert(node.isTerminal());
    assert(node.getQ() == -1.0f);
    assert(node.getBound().isTerminal());
}

void Node_isTerminal_visitsAboveTwoPow32_staysFalseUntilMarked()
{
    // Arrange
    Node node(HashKey(4), 0);
    node.setNonTerminal(0.3f, 0.1f);
    node.incrementVisits(5000000000ull);

    // Act
    bool terminalBefore = node.isTerminal();
    bool won            = node.tryMarkTerminal(mate_in(3));

    // Assert: no carry into the terminal tag; the transition keeps the visit count.
    assert(!terminalBefore);
    assert(won);
    assert(node.isTerminal());
    assert(node.getVisits() == 5000000001ull);
    assert(node.getQ() == 1.0f);
    assert(!node.tryMarkTerminal(mated_in(5)));
}

void Node_tryMarkTerminal_concurrentWithIncrementVisits_visitsPreserved()
{
    // Arrange
    Node node(HashKey(5), 0);
    node.setNonTerminal(0.0f, 0.5f);
    constexpr int     NumIncrements = 200000;
    std::atomic<bool> started {false};
    std::atomic<int>  wins {0};

    // Act: one thread keeps adding visits while two threads race for the terminal transition.
    std::thread adder([&] {
        for (int i = 0; i < NumIncrements; i++) {
            node.incrementVisits(1);
            if (i == 100)
                started.store(true);
        }
    });
    std::vector<std::thread> markers;
    for (int t = 0; t < 2; t++)
        markers.emplace_back([&] {
            while (!started.load())
                ;
            if (node.tryMarkTerminal(mated_in(6)))
                wins.fetch_add(1);
        });
    adder.join();
    for (auto &th : markers)
        th.join();

    // Assert
    assert(wins.load() == 1);
    assert(node.isTerminal());
    assert(node.getVisits() == 1ull + NumIncrements);
}

void updateStats_edgeVisitsAboveTwoPow32_qIsWeightedAverage()
{
    // Arrange: parent with two explored edges, weights 5e9 and 1e9 (sum above 2^32).
    Node parent(HashKey(10), 0);
    parent.setNonTerminal(0.2f, 0.1f);
    const Pos   moves[]  = {Pos(1, 1), Pos(2, 2), Pos(3, 3)};
    const float policy[] = {0.5f, 0.3f, 0.2f};
    parent.createEdgesFrom(moves, policy, 3, EdgeLimits {});
    Node c1(HashKey(11), 0), c2(HashKey(12), 0);
    c1.setNonTerminal(0.5f, 0.2f);
    c2.setNonTerminal(-0.3f, 0.4f);
    EdgeArray &edges = *parent.getEdges();
    edges[0].setChild(&c1);
    edges[0].addVisits(2500000000ull);
    edges[0].addVisits(2500000000ull);
    edges[1].setChild(&c2);
    edges[1].addVisits(1000000000ull);

    // Act
    parent.updateStats();

    // Assert (child utility is flipped to the parent's side)
    double n = 1.0 + 5e9 + 1e9;
    double q = (0.2 + 5e9 * -0.5 + 1e9 * 0.3) / n;
    double d = (0.1 + 5e9 * 0.2 + 1e9 * 0.4) / n;
    assert(std::fabs(parent.getQ() - q) < 1e-4);
    assert(std::fabs(parent.getD() - d) < 1e-4);
    assert(parent.getQ() >= -1.0f && parent.getQ() <= 1.0f);
}

// ----------------------------------------------------------------------------------------------
// Selection helpers

void readyToExpand_visitsAboveTwoPow32_true()
{
    // Arrange / Act
    bool ready = readyToExpand(TwoPow32 + 1, 3);

    // Assert
    assert(ready);
}

void cpuctExplorationFactor_visitsAboveTwoPow32_monotonicAndFinite()
{
    // Arrange
    const uint64_t visits[] = {TwoPow32 - 1, TwoPow32, TwoPow32 + (1 << 20), uint64_t(1) << 40,
                               SearchVisitGuard};

    // Act
    std::vector<float> factors;
    for (uint64_t v : visits)
        factors.push_back(Testing::cpuctExplorationFactor(v));

    // Assert
    for (size_t i = 0; i < factors.size(); i++) {
        assert(std::isfinite(factors[i]));
        assert(factors[i] > 0);
        if (i > 0)
            assert(factors[i] >= factors[i - 1]);
    }
    assert(factors.back() > factors.front());
}

void puctSelectionValue_childVisitsAboveTwoPow32_noWrap()
{
    // Arrange: 2^32 child visits would wrap to zero in a 32-bit counter (U would explode).
    const float cpuct = Testing::cpuctExplorationFactor(TwoPow32 * 4);

    // Act
    float noVirtual   = Testing::puctSelectionValue(0.1f, 0, 0, 0.5f, TwoPow32, 0, cpuct);
    float withVirtual = Testing::puctSelectionValue(0.1f, 0, 0, 0.5f, TwoPow32, 8, cpuct);
    float small       = Testing::puctSelectionValue(0.1f, 0, 0, 0.5f, 100, 0, cpuct);

    // Assert
    assert(std::fabs(noVirtual - 0.1f) < 1e-3f);
    assert(std::fabs(withVirtual - 0.1f) < 1e-3f);
    assert(small > noVirtual);
}

// ----------------------------------------------------------------------------------------------
// searchNode through the engine (single thread, tree reuse on the same position)

/// Deterministic stand-in evaluator (MCTS refuses to search without one, and the test binary
/// runs on the built-in config, which loads no network). Value is a fixed function of the
/// position hash, policy is a fixed function of the cell index.
class HashEvaluator : public Evaluation::Evaluator
{
public:
    HashEvaluator(int boardSize, Rule rule) : Evaluator(boardSize, rule) {}
    void initEmptyBoard() override {}

    Evaluation::ValueType evaluateValue(const Board &board, Evaluation::AccLevel) override
    {
        HashKey h    = board.zobristKey();
        float   win  = float(h & 0xff) / 256.0f;
        float   loss = float((h >> 8) & 0xff) / 256.0f;
        return Evaluation::ValueType(win, loss, -1.0f);
    }

    void evaluatePolicy(const Board &board,
                        Evaluation::PolicyBuffer &policyBuffer,
                        Evaluation::AccLevel) override
    {
        for (int i = 0; i < boardSize * boardSize; i++)
            if (policyBuffer.getComputeFlag(i))
                policyBuffer(i) = float((i * 37) % 11) * 0.1f;
    }
};

struct MctsEngineFixture
{
    size_t savedThreads;

    MctsEngineFixture()
    {
        Search::Engine.waitForIdle();
        savedThreads = Search::Engine.size();
        Search::Engine.setNumThreads(1);
        Search::Engine.setupEvaluator([](int boardSize, Rule rule, Numa::NumaNodeId) {
            return std::make_unique<HashEvaluator>(boardSize, rule);
        });
        Search::Engine.setupSearcher(Search::createSearcher("mcts"));
        Search::Engine.clear(true);
    }

    ~MctsEngineFixture()
    {
        Search::Engine.waitForIdle();
        Search::Engine.setupSearcher(Search::createSearcher(""));
        Search::Engine.setupEvaluator(nullptr);  // the built-in config has no evaluator
        Search::Engine.setNumThreads(savedThreads);
        Search::Engine.clear(true);
    }

    MCTSSearcher &searcher() { return static_cast<MCTSSearcher &>(*Search::Engine.searcher()); }
};

std::unique_ptr<Board> makeSearchBoard()
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 7), Pos(8, 8), Pos(7, 8), Pos(6, 9)})
        board->move(FREESTYLE, p);
    return board;
}

void runMctsSearch(const Board &board, uint64_t maxNodes, std::vector<Pos> whitelist = {})
{
    Search::SearchOptions options;
    options.rule                = {FREESTYLE, GameRule::FREEOPEN};
    options.disableOpeningQuery = true;
    options.maxNodes            = maxNodes;
    options.rootMovesWhitelist  = whitelist;
    if (!whitelist.empty())
        options.multiPV = static_cast<uint16_t>(whitelist.size());

    ScopedCoutCapture cap;
    Search::Engine.startThinking(board, options);
    Search::Engine.waitForIdle();
}

Edge &mostVisitedEdge(Node &node)
{
    EdgeArray &edges = *node.getEdges();
    uint32_t   best  = 0;
    for (uint32_t i = 1; i < edges.numEdges; i++)
        if (edges[i].getVisits() > edges[best].getVisits())
            best = i;
    return edges[best];
}

void searchNode_rootPrimedAboveTwoPow32_visitsKeepGrowing()
{
    // Arrange: a small search, then prime the root and its best edge/child to 2^32 - 1000.
    MctsEngineFixture fx;
    auto              board = makeSearchBoard();
    runMctsSearch(*board, 500);
    Node *root = fx.searcher().graph.root;
    assert(root);
    Edge &best  = mostVisitedEdge(*root);
    Node *child = best.getChild();
    assert(child);
    const uint64_t prime = TwoPow32 - 1000;
    best.addVisits(prime);
    child->incrementVisits(prime);
    root->incrementVisits(prime);
    const uint64_t rootBefore = root->getVisits();
    const uint64_t edgeBefore = best.getVisits();

    // Act: same position, so the root is reused.
    runMctsSearch(*board, 5000);

    // Assert
    assert(fx.searcher().graph.root == root);
    assert(root->getVisits() > TwoPow32);
    assert(root->getVisits() >= rootBefore + 4000);
    assert(best.getVisits() >= edgeBefore);
    assert(root->getQ() >= -1.0f && root->getQ() <= 1.0f);
    assert(root->getD() >= 0.0f && root->getD() <= 1.0f);
    for (const auto &rm : Search::Engine.main()->rootMoves)
        assert(rm.winRate >= 0.0f && rm.winRate <= 1.0f);
}

void searchNode_transpositionRule_edgeAboveNodeStillDescends()
{
    // Arrange: prime one root edge and its child node above 2^32 with edge >= node visits, and
    // restrict the root to that move so every playout goes through the edge.
    MctsEngineFixture fx;
    auto              board = makeSearchBoard();
    runMctsSearch(*board, 500);
    Node *root = fx.searcher().graph.root;
    Edge &best  = mostVisitedEdge(*root);
    Node *child = best.getChild();
    assert(child);
    const uint64_t prime = TwoPow32 + 5;
    best.addVisits(prime + 3);
    child->incrementVisits(prime);
    root->incrementVisits(prime + 3);
    const uint64_t childBefore = child->getVisits();
    const uint64_t edgeBefore  = best.getVisits();
    assert(edgeBefore >= childBefore);

    // Act
    runMctsSearch(*board, 100, {best.getMove()});

    // Assert: the playouts descended into the child (its own visits grew), not only the edge.
    assert(fx.searcher().graph.root == root);
    assert(edgeBefore > TwoPow32);
    assert(child->getVisits() > childBefore);
    assert(best.getVisits() > edgeBefore);
}

// ----------------------------------------------------------------------------------------------

bool isolateMode()
{
    const char *env = std::getenv("RAPFI_TEST_ISOLATE");
    return env && env[0] == '1';
}

#if defined(__unix__) || defined(__APPLE__)
    #define RUN_VISITS_TEST(fn)                                                 \
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
    #define RUN_VISITS_TEST(fn) RUN_TEST(fn)
#endif

// Engine-driven tests cannot run in a forked child (the engine's worker threads do not survive
// fork), so they always run in-process. RAPFI_TEST_SKIP (a substring of the test name) skips one of them when
// the failure of another must not hide its result.
bool skipped(const char *name)
{
    const char *env = std::getenv("RAPFI_TEST_SKIP");
    return env && env[0] && std::strstr(name, env) != nullptr;
}

#define RUN_ENGINE_TEST(fn)                                  \
    do {                                                     \
        if (skipped(#fn)) {                                  \
            std::printf("[SKIP] %s\n", #fn);                 \
            break;                                           \
        }                                                    \
        RUN_TEST(fn);                                        \
    } while (0)

}  // namespace

void runMctsVisitsTests()
{
    RUN_VISITS_TEST(Edge_size_is16Bytes);
    RUN_VISITS_TEST(Edge_getP_roundTripUnchangedForAllQuantizedPolicies);
    RUN_VISITS_TEST(Edge_addVisits_acrossTwoPow32_countsExactly);
    RUN_VISITS_TEST(Edge_addVisits_returnsVisitsBeforeTheAdd);
    RUN_VISITS_TEST(Edge_addVisits_nearTwoPow48_policyBitsIntact);
    RUN_VISITS_TEST(Edge_setChild_preservesMoveAndReturnsPointer);
    RUN_VISITS_TEST(Edge_getChild_initiallyNull_andVisitsZero);
    RUN_VISITS_TEST(Edge_concurrentAddVisits_totalIsExact);
    RUN_VISITS_TEST(Node_size_is56BytesOn64BitTargets);
    RUN_VISITS_TEST(Node_incrementVisits_acrossTwoPow32_getVisitsIs64Bit);
    RUN_VISITS_TEST(Node_finishVisit_acrossTwoPow32_nVirtualReturnsToZero);
    RUN_VISITS_TEST(Node_setTerminal_freshNode_visitsOneAndTerminal);
    RUN_VISITS_TEST(Node_isTerminal_visitsAboveTwoPow32_staysFalseUntilMarked);
    RUN_VISITS_TEST(Node_tryMarkTerminal_concurrentWithIncrementVisits_visitsPreserved);
    RUN_VISITS_TEST(updateStats_edgeVisitsAboveTwoPow32_qIsWeightedAverage);
    RUN_VISITS_TEST(readyToExpand_visitsAboveTwoPow32_true);
    RUN_VISITS_TEST(cpuctExplorationFactor_visitsAboveTwoPow32_monotonicAndFinite);
    RUN_VISITS_TEST(puctSelectionValue_childVisitsAboveTwoPow32_noWrap);
    RUN_ENGINE_TEST(searchNode_rootPrimedAboveTwoPow32_visitsKeepGrowing);
    RUN_ENGINE_TEST(searchNode_transpositionRule_edgeAboveNodeStillDescends);
}
