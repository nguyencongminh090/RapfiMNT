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

// VCN-03 slice 2: MCTS node key and root identity in VCN mode. The VCN XOR is part of the node
// key at every key site, so VCN nodes never mix with ordinary nodes, other levels or the other
// attacker, also across searches through graph reuse; with VCN off the key is the zobrist key.
//
// Set RAPFI_TEST_ISOLATE=1 to run each pure test in a forked child (one failing assert reports
// [FAIL] and the suite continues); RAPFI_TEST_SKIP=<substring> skips an engine-driven test.

#include "testutil.h"

#include "../eval/evaluator.h"
#include "../game/board.h"
#include "../search/mcts/node.h"
#include "../search/mcts/searcher.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"
#include "../search/vcn.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace Search;
using namespace Search::MCTS;

namespace {

/// A small Freestyle position, black to move.
std::unique_ptr<Board> makeBoard()
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 7), Pos(8, 8), Pos(7, 8), Pos(6, 9)})
        board->move(FREESTYLE, p);
    return board;
}

SearchOptions vcnOptions(VCNLevel level, Color attacker)
{
    SearchOptions opts;
    opts.rule             = {FREESTYLE, GameRule::FREEOPEN};
    opts.vcnMode.n        = level;
    opts.vcnMode.attacker = attacker;
    return opts;
}

// ----------------------------------------------------------------------------------------------
// mctsNodeKey (pure)

void mctsNodeKey_vcnOff_equalsZobrist()
{
    // Arrange
    auto          board = makeBoard();
    SearchOptions opts;  // VCN off by default
    assert(!opts.vcnMode.enabled());

    // Act
    HashKey key0 = mctsNodeKey(*board, opts, 0);
    HashKey key3 = mctsNodeKey(*board, opts, 3);  // the root pass count plays no role when off

    // Assert
    assert(key0 == board->zobristKey());
    assert(key3 == board->zobristKey());
}

void mctsNodeKey_vcnOn_differsByLevelAndAttacker()
{
    // Arrange
    auto          board  = makeBoard();
    const HashKey zob    = board->zobristKey();
    SearchOptions b2     = vcnOptions(VCNLevel::VC2, BLACK);
    SearchOptions b3     = vcnOptions(VCNLevel::VC3, BLACK);
    SearchOptions w2     = vcnOptions(VCNLevel::VC2, WHITE);
    SearchOptions w3     = vcnOptions(VCNLevel::VC3, WHITE);
    const int     passes = 0;

    // Act
    HashKey keys[4] = {mctsNodeKey(*board, b2, passes),
                       mctsNodeKey(*board, b3, passes),
                       mctsNodeKey(*board, w2, passes),
                       mctsNodeKey(*board, w3, passes)};

    // Assert: four distinct keys, none equal to the zobrist key, each the AB TT formula.
    for (int i = 0; i < 4; i++) {
        assert(keys[i] != zob);
        for (int j = i + 1; j < 4; j++)
            assert(keys[i] != keys[j]);
    }
    assert(keys[0] == (zob ^ Vcn::hashXor(b2, VCNLevel::VC2)));
    assert(keys[1] == (zob ^ Vcn::hashXor(b3, VCNLevel::VC3)));
    assert(keys[2] == (zob ^ Vcn::hashXor(w2, VCNLevel::VC2)));
    assert(keys[3] == (zob ^ Vcn::hashXor(w3, VCNLevel::VC3)));
}

void mctsNodeKey_defenderPassRaisesLevel_cappedAtVc5()
{
    // Arrange: black (side to move) attacks from VC3; white is the defender. The root board has
    // one earlier white pass, so the per-search base is not zero.
    auto board = makeBoard();
    board->move(FREESTYLE, Pos(9, 9));  // black
    board->move(FREESTYLE, Pos::PASS);  // white pass before the search root
    assert(board->sideToMove() == BLACK);
    SearchOptions opts               = vcnOptions(VCNLevel::VC3, BLACK);
    const int     rootDefenderPasses = board->passMoveCountOfSide(WHITE);
    assert(rootDefenderPasses == 1);
    const HashKey rootKey = mctsNodeKey(*board, opts, rootDefenderPasses);

    // Act / Assert: root keeps the root level.
    assert(mctsNodeVcnLevel(*board, opts, rootDefenderPasses) == VCNLevel::VC3);
    assert(rootKey == (board->zobristKey() ^ Vcn::hashXor(opts, VCNLevel::VC3)));

    // One defender pass in the search: VC4.
    board->move(FREESTYLE, Pos(9, 10));  // black (attacker) move
    board->move(FREESTYLE, Pos::PASS);   // white (defender) pass
    assert(mctsNodeVcnLevel(*board, opts, rootDefenderPasses) == VCNLevel::VC4);
    assert(mctsNodeKey(*board, opts, rootDefenderPasses)
           == (board->zobristKey() ^ Vcn::hashXor(opts, VCNLevel::VC4)));
    // The attacker's own move does not raise the level (white to move, no new pass).
    board->move(FREESTYLE, Pos(10, 10));  // black
    assert(mctsNodeVcnLevel(*board, opts, rootDefenderPasses) == VCNLevel::VC4);

    // Two defender passes: VC5.
    board->move(FREESTYLE, Pos::PASS);  // white pass
    assert(mctsNodeVcnLevel(*board, opts, rootDefenderPasses) == VCNLevel::VC5);
    assert(mctsNodeKey(*board, opts, rootDefenderPasses)
           == (board->zobristKey() ^ Vcn::hashXor(opts, VCNLevel::VC5)));

    // A third defender pass stays at VC5.
    board->move(FREESTYLE, Pos(11, 11));  // black
    board->move(FREESTYLE, Pos::PASS);    // white pass
    assert(mctsNodeVcnLevel(*board, opts, rootDefenderPasses) == VCNLevel::VC5);
    assert(mctsNodeKey(*board, opts, rootDefenderPasses)
           == (board->zobristKey() ^ Vcn::hashXor(opts, VCNLevel::VC5)));
}

// ----------------------------------------------------------------------------------------------
// Searcher-driven tests (single thread, deterministic stand-in evaluator)

/// Deterministic stand-in evaluator (the test binary runs on the built-in config, which loads no
/// network, and MCTS refuses to search without one). Same as in mcts_visits_tests.cpp.
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

/// Options of one fixed-node search. With `level` != None, the searcher is driven directly in VCN
/// mode; the attacker is set as think() does (side to move), since the protocol is bypassed.
SearchOptions searchOptions(const Board &board, uint64_t maxNodes, VCNLevel level)
{
    SearchOptions options;
    options.rule                = {FREESTYLE, GameRule::FREEOPEN};
    options.disableOpeningQuery = true;
    options.maxNodes            = maxNodes;
    options.vcnMode.n           = level;
    if (options.vcnMode.enabled())
        options.vcnMode.attacker = board.sideToMove();
    return options;
}

void runMctsSearch(const Board &board, const SearchOptions &options)
{
    ScopedCoutCapture cap;
    Search::Engine.startThinking(board, options);
    Search::Engine.waitForIdle();
}

/// Walks the graph below `node` through every edge with a child, replaying the moves on `board`,
/// and asserts every node key equals mctsNodeKey of its board. Returns the number of nodes seen.
size_t checkGraphKeys(Node                     &node,
                      Board                    &board,
                      const SearchOptions      &opts,
                      int                       rootDefenderPasses,
                      std::unordered_set<Node *> &visited)
{
    if (!visited.insert(&node).second)
        return 0;
    assert(node.getHash() == mctsNodeKey(board, opts, rootDefenderPasses));

    size_t     count = 1;
    EdgeArray *edges = node.getEdges();
    if (!edges)
        return count;
    for (uint32_t i = 0; i < edges->numEdges; i++) {
        Node *child = (*edges)[i].getChild();
        if (!child)
            continue;
        Pos move = (*edges)[i].getMove();
        board.move(FREESTYLE, move);
        count += checkGraphKeys(*child, board, opts, rootDefenderPasses, visited);
        board.undo(FREESTYLE);
    }
    return count;
}

void setupRootNode_samePositionVcnToggled_rebuildsRoot()
{
    // Arrange
    MctsEngineFixture fx;
    auto              board    = makeBoard();
    const HashKey     zob      = board->zobristKey();
    SearchOptions     ordinary = searchOptions(*board, 500, VCNLevel::None);
    SearchOptions     vcn      = searchOptions(*board, 500, VCNLevel::VC3);
    runMctsSearch(*board, ordinary);
    assert(fx.searcher().graph.root);
    assert(fx.searcher().graph.root->getHash() == zob);

    // Act: VCN search on the same moves.
    runMctsSearch(*board, vcn);

    // Assert: a new VCN-keyed root, not the reused ordinary one.
    Node *vcnRoot = fx.searcher().graph.root;
    assert(vcnRoot);
    assert(vcnRoot->getHash() != zob);
    assert(vcnRoot->getHash() == (zob ^ Vcn::hashXor(vcn, VCNLevel::VC3)));

    // Act: ordinary search on the same moves again.
    runMctsSearch(*board, ordinary);

    // Assert: the ordinary root is back (compared by key only: the old ordinary root may have been
    // recycled and reallocated at any address).
    assert(fx.searcher().graph.root);
    assert(fx.searcher().graph.root->getHash() == zob);
}

void mctsVcn_afterVcnSearch_nextOrdinarySearchReusesNoVcnNode()
{
    // Arrange: ordinary search, then a VCN search on the same moves.
    MctsEngineFixture fx;
    auto              board    = makeBoard();
    SearchOptions     ordinary = searchOptions(*board, 2000, VCNLevel::None);
    SearchOptions     vcn      = searchOptions(*board, 2000, VCNLevel::VC3);
    runMctsSearch(*board, ordinary);
    runMctsSearch(*board, vcn);

    // The VCN graph carries VCN keys at every node (this half is the fail-first check: before
    // the key change every node key is the zobrist key).
    {
        auto                       replay = makeBoard();
        std::unordered_set<Node *> visited;
        size_t                     n      = checkGraphKeys(*fx.searcher().graph.root,
                                          *replay,
                                          vcn,
                                          fx.searcher().scratch.rootDefenderPasses,
                                          visited);
        assert(n > 1);
        assert(fx.searcher().graph.root->getHash() != board->zobristKey());
    }

    // Act
    runMctsSearch(*board, ordinary);

    // Assert: every node an ordinary search reaches has key == its board's zobrist key.
    auto                       replay = makeBoard();
    std::unordered_set<Node *> visited;
    size_t                     n = checkGraphKeys(*fx.searcher().graph.root,
                                      *replay,
                                      ordinary,
                                      fx.searcher().scratch.rootDefenderPasses,
                                      visited);
    assert(n > 1);
    assert(fx.searcher().graph.root->getHash() == board->zobristKey());
}

// ----------------------------------------------------------------------------------------------

bool isolateMode()
{
    const char *env = std::getenv("RAPFI_TEST_ISOLATE");
    return env && env[0] == '1';
}

#if defined(__unix__) || defined(__APPLE__)
    #define RUN_PURE_TEST(fn)                                                   \
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
    #define RUN_PURE_TEST(fn) RUN_TEST(fn)
#endif

// Engine-driven tests cannot run in a forked child (the engine's worker threads do not survive
// fork), so they always run in-process. RAPFI_TEST_SKIP (a substring of the test name) skips one.
bool skipped(const char *name)
{
    const char *env = std::getenv("RAPFI_TEST_SKIP");
    return env && env[0] && std::strstr(name, env) != nullptr;
}

#define RUN_ENGINE_TEST(fn)                  \
    do {                                     \
        if (skipped(#fn)) {                  \
            std::printf("[SKIP] %s\n", #fn); \
            break;                           \
        }                                    \
        RUN_TEST(fn);                        \
    } while (0)

}  // namespace

void runMctsVcnTests()
{
    RUN_PURE_TEST(mctsNodeKey_vcnOff_equalsZobrist);
    RUN_PURE_TEST(mctsNodeKey_vcnOn_differsByLevelAndAttacker);
    RUN_PURE_TEST(mctsNodeKey_defenderPassRaisesLevel_cappedAtVc5);
    RUN_ENGINE_TEST(setupRootNode_samePositionVcnToggled_rebuildsRoot);
    RUN_ENGINE_TEST(mctsVcn_afterVcnSearch_nextOrdinarySearchReusesNoVcnNode);
}
