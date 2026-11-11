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
// Slice 3 (move generation): the shared move generation of a non-root node is reached through
// Search::MCTS::Testing::generateNodeMoves (search/mcts/searchtesting.h), on a board bound to the
// engine's main thread with a stand-in evaluator. Slice 3 added a pass edge for the VCN defender;
// VCN-04 removed it again, so these tests now pin "no pass edge, the ordinary list".
//
// VCN-04 (the pass is a test, not an edge): the pass test in evaluateNode (VC3 exact, VC2 static
// rule and bounded VCT solver) and the solver itself, through the Testing forwarders.
//
// Slice 4 (VC4 / VC5 terminals): evaluateNode and SimpleVCF are reached through the Testing
// forwarders; the AB cross-check and the smoke run drive both searchers directly.
//
// Set RAPFI_TEST_ISOLATE=1 to run each pure test in a forked child (one failing assert reports
// [FAIL] and the suite continues); RAPFI_TEST_SKIP=<substring> skips an engine-driven test.

#include "testutil.h"

#include "../eval/eval.h"
#include "../eval/evaluator.h"
#include "../game/board.h"
#include "../search/mcts/node.h"
#include "../search/mcts/searcher.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"
#include "../config.h"
#include "../search/mcts/parameter.h"
#include "../search/mcts/searchtesting.h"
#include "../search/movepick.h"
#include "../search/searchconfig.h"
#include "../core/iohelper.h"
#include "../core/time.h"
#include "../game/wincheck.h"
#include "../search/hashtable.h"
#include "../search/vcn.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Command::GomocupProtocol {
extern std::unique_ptr<Board> board;
extern Search::SearchOptions  options;
void                          searchVCN();
}  // namespace Command::GomocupProtocol

namespace GP = Command::GomocupProtocol;

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
// Move generation of a non-root node (expandNode<false> and widenNode share it), reached through
// Testing::generateNodeMoves on a board bound to the engine's main search thread, which holds a
// deterministic stand-in evaluator. VCN-03 slice 3 gave the VCN defender a pass edge here; VCN-04
// removed it (the pass is a test in evaluateNode), so the list is the ordinary one in every mode.

/// Stand-in policy with negative logits for part of the moves: the picker's insertion sort only
/// orders moves with a score >= 0, so the list it returns has an unsorted tail.
class SignedPolicyEvaluator : public HashEvaluator
{
public:
    using HashEvaluator::HashEvaluator;

    void evaluatePolicy(const Board &board,
                        Evaluation::PolicyBuffer &policyBuffer,
                        Evaluation::AccLevel) override
    {
        for (int i = 0; i < boardSize * boardSize; i++)
            if (policyBuffer.getComputeFlag(i))
                policyBuffer(i) = float((i * 37) % 11) * 0.5f - 2.5f;
    }
};

struct MoveList
{
    std::vector<Pos>   moves;
    std::vector<float> policy;

    int indexOf(Pos m) const
    {
        auto it = std::find(moves.begin(), moves.end(), m);
        return it == moves.end() ? -1 : int(it - moves.begin());
    }
    float sum() const
    {
        float s = 0.0f;
        for (float p : policy)
            s += p;
        return s;
    }
    bool sortedDescending() const
    {
        for (size_t i = 1; i < policy.size(); i++)
            if (policy[i] > policy[i - 1])
                return false;
        return true;
    }
};

/// The engine's main thread with a fresh stand-in evaluator for `rule` (15x15).
template <typename Eval = HashEvaluator>
SearchThread *threadWithEvaluator(Rule rule)
{
    SearchThread *th = Search::Engine.main();
    assert(th);
    th->evaluator = std::make_unique<Eval>(15, rule);
    assert(th->evaluator);
    return th;
}

MoveList nodeMoves(const Board &board, const SearchOptions &opts)
{
    Pos      moves[MAX_MOVES];
    float    policy[MAX_MOVES];
    uint32_t n = Testing::generateNodeMoves(board, opts, moves, policy);
    return {std::vector<Pos>(moves, moves + n), std::vector<float>(policy, policy + n)};
}

/// The move generation expandNode<false> did before slice 3 (the reference for VCN off).
MoveList referenceMoves(const Board &board, Rule rule)
{
    MovePicker mp(rule,
                  board,
                  MovePicker::ExtraArgs<MovePicker::MAIN> {Pos::NONE, {}, true, PolicyTemperature});
    MoveList   l;
    while (Pos move = mp()) {
        l.moves.push_back(move);
        l.policy.push_back(mp.curMoveNormalizePolicy());
    }
    return l;
}

/// makeBoard() bound to `th`: black (side to move) is the defender when white attacks.
std::unique_ptr<Board> boundBoard(SearchThread *th)
{
    auto src = makeBoard();
    return std::make_unique<Board>(*src, th);
}

/// The default of SearchConfig::mctsVcnPassTestNodes (VCN-04 stage 2, chosen by measurement).
constexpr int DefaultPassTestNodes = 0;

/// Restores SearchCfg on scope exit.
struct ScopedSearchCfg
{
    SearchConfig saved = SearchCfg;
    ~ScopedSearchCfg() { SearchCfg = saved; }
};

/// Asserts that `l` holds no pass and is the VCN-off generation `plain`, move for move.
void assertNoPassAndEqual(const MoveList &l, const MoveList &plain)
{
    assert(l.indexOf(Pos::PASS) < 0);
    assert(!l.moves.empty());
    assert(l.moves == plain.moves);
    assert(l.policy == plain.policy);
}

void expandNode_vcnDefender_noPassEdge()
{
    // Arrange: white attacks, black (to move) defends; no five on the board. Both stand-in
    // policies (the second leaves the picker's list with an unsorted tail), every root level.
    MctsEngineFixture fx;
    for (int k = 0; k < 2; k++) {
        SearchThread *th = k == 0 ? threadWithEvaluator(FREESTYLE)
                                  : threadWithEvaluator<SignedPolicyEvaluator>(FREESTYLE);
        auto          board = boundBoard(th);
        assert(board->sideToMove() == BLACK);
        assert(!board->p4Count(WHITE, A_FIVE));
        MoveList plain = referenceMoves(*board, FREESTYLE);

        for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3, VCNLevel::VC4}) {
            SearchOptions opts = vcnOptions(level, WHITE);

            // Act
            MoveList l = nodeMoves(*board, opts);

            // Assert: the defender's list is the ordinary list (VCN-04: the pass is a test)
            assertNoPassAndEqual(l, plain);
        }
    }
}

void expandNode_vcnDefenderFacingFive_noPassEdge()
{
    // Arrange: black (attacker) has an open four; white (defender) is to move and must block.
    MctsEngineFixture fx;
    SearchThread     *th  = threadWithEvaluator(FREESTYLE);
    auto              src = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    src->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 5), Pos(0, 0), Pos(7, 6), Pos(0, 2), Pos(7, 7), Pos(0, 4), Pos(7, 8)})
        src->move(FREESTYLE, p);
    Board board(*src, th);
    assert(board.sideToMove() == WHITE);
    assert(board.p4Count(BLACK, A_FIVE));
    SearchOptions opts  = vcnOptions(VCNLevel::VC3, BLACK);
    MoveList      plain = referenceMoves(board, FREESTYLE);

    // Act
    MoveList l = nodeMoves(board, opts);

    // Assert: no pass; the list is exactly the ordinary one.
    assertNoPassAndEqual(l, plain);
}

void expandNode_vcnDefenderFacingOpenThree_noPassEdgeAndFullDefendList()
{
    // Arrange: black (attacker) has an open three, so white (defender, to move) gets the picker's
    // DEFENDFOUR stage; the negative-logit policy exercises the unsorted-tail case there.
    MctsEngineFixture fx;
    SearchThread     *th  = threadWithEvaluator<SignedPolicyEvaluator>(FREESTYLE);
    auto              src = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    src->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 6), Pos(0, 0), Pos(7, 7), Pos(0, 3), Pos(7, 8)})
        src->move(FREESTYLE, p);
    Board board(*src, th);
    assert(board.sideToMove() == WHITE);
    assert(!board.p4Count(BLACK, A_FIVE));
    assert(board.p4Count(BLACK, B_FLEX4));
    MoveList plain = referenceMoves(board, FREESTYLE);

    for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3}) {
        SearchOptions opts = vcnOptions(level, BLACK);

        // Act
        MoveList l = nodeMoves(board, opts);

        // Assert: no pass against an open three; every defence of the ordinary list is there.
        assertNoPassAndEqual(l, plain);
    }
}

void expandNode_vcnAttacker_noPassEdge()
{
    // Arrange: black is to move and is the attacker.
    MctsEngineFixture fx;
    auto              board = boundBoard(threadWithEvaluator(FREESTYLE));
    SearchOptions     opts  = vcnOptions(VCNLevel::VC3, BLACK);
    MoveList          plain = referenceMoves(*board, FREESTYLE);

    // Act
    MoveList l = nodeMoves(*board, opts);

    // Assert
    assertNoPassAndEqual(l, plain);
}

void expandNode_vcnOff_edgesUnchanged()
{
    // Arrange: both stand-in policies, defender-shaped position, VCN off.
    MctsEngineFixture fx;
    for (int k = 0; k < 2; k++) {
        SearchThread *th = k == 0 ? threadWithEvaluator(FREESTYLE)
                                  : threadWithEvaluator<SignedPolicyEvaluator>(FREESTYLE);
        auto          board = boundBoard(th);
        SearchOptions opts;
        opts.rule = {FREESTYLE, GameRule::FREEOPEN};
        assert(!opts.vcnMode.enabled());
        MoveList plain = referenceMoves(*board, FREESTYLE);

        // Act
        MoveList l = nodeMoves(*board, opts);

        // Assert: same moves, policies and order as the pre-slice generation.
        assertNoPassAndEqual(l, plain);
    }
}

/// Renju, black to move with a real forbidden 3x3 point at (7,7): black (7,5) (7,6) and
/// (5,7) (6,7); the white stones are far away.
std::vector<Pos> renjuBlackForbiddenMoves()
{
    return {Pos(7, 5), Pos(0, 0), Pos(7, 6), Pos(0, 3), Pos(5, 7), Pos(0, 6), Pos(6, 7), Pos(0, 9)};
}

void expandNode_vcnDefenderRenjuBlack_noPassEdgeAndNoForbiddenMove()
{
    // Arrange: Renju, black (to move) defends against white. Black has a forbidden 3x3 point.
    MctsEngineFixture fx;
    SearchThread     *th  = threadWithEvaluator(RENJU);
    auto              src = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    src->newGame(RENJU);
    for (Pos p : renjuBlackForbiddenMoves())
        src->move(RENJU, p);
    Board board(*src, th);
    assert(board.sideToMove() == BLACK);
    assert(board.checkForbiddenPoint(Pos(7, 7)));
    MoveList plain = referenceMoves(board, RENJU);

    for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3}) {
        SearchOptions opts;
        opts.rule             = {RENJU, GameRule::FREEOPEN};
        opts.vcnMode.n        = level;
        opts.vcnMode.attacker = WHITE;

        // Act
        MoveList l = nodeMoves(board, opts);

        // Assert
        assertNoPassAndEqual(l, plain);
        assert(l.indexOf(Pos(7, 7)) < 0);
    }
}

void widenNode_vcnDefenderCutNode_noPassEdgeAfterWiden()
{
    // Arrange: a cut defender node from the shared generation.
    MctsEngineFixture fx;
    auto              board = boundBoard(threadWithEvaluator(FREESTYLE));
    SearchOptions     opts  = vcnOptions(VCNLevel::VC3, WHITE);
    MoveList          gen   = nodeMoves(*board, opts);
    const EdgeLimits  limits {0.5f, 2};
    Node              parent(HashKey(100), 0);
    parent.setNonTerminal(0.0f, 0.1f);
    parent.createEdgesFrom(gen.moves.data(), gen.policy.data(), uint32_t(gen.moves.size()), limits,
                           nullptr);
    EdgeArray *old = parent.getEdges();
    assert(old && old->edgesCut());

    // Every kept reply is a proven win for the attacker.
    std::vector<std::unique_ptr<Node>> children;
    auto attach = [&](EdgeArray &edges, uint32_t from, Value v) {
        for (uint32_t i = from; i < edges.numEdges; i++) {
            children.push_back(std::make_unique<Node>(HashKey(200 + i), 0));
            children.back()->setTerminal(v);
            edges[i].setChild(children.back().get());
            edges[i].addVisits(1);
        }
    };
    attach(*old, 0, mate_in(3));
    const uint32_t numKept = old->numEdges;
    bool           passInOld = false;
    for (uint32_t i = 0; i < old->numEdges; i++)
        passInOld |= (*old)[i].getMove() == Pos::PASS;
    assert(parent.updateStats());  // widen needed

    // Act: widenNode's step, on the same generation.
    MoveList          full = nodeMoves(*board, opts);
    RetiredEdgeArrays retired;
    bool              ok = parent.replaceEdgesWithFull(old,
                                          full.moves.data(),
                                          full.policy.data(),
                                          uint32_t(full.moves.size()),
                                          nullptr,
                                          retired);

    // Assert: widened, no pass before or after, and the node proves lost once the added replies
    // lose too (no pass edge is left to hold the proof open).
    assert(ok);
    assert(!passInOld);
    EdgeArray *wide = parent.getEdges();
    assert(!wide->edgesCut() && wide->numEdges == full.moves.size());
    for (uint32_t i = 0; i < wide->numEdges; i++)
        assert((*wide)[i].getMove() != Pos::PASS);
    attach(*wide, numKept, mate_in(5));
    assert(!parent.updateStats());
    assert(parent.getBound().isTerminal());
    assert(parent.getBound().upper == mated_in(5));
}

void config_mctsVcnPassTestNodes_parsedAndClamped()
{
    // Arrange
    ScopedSearchCfg cfg;
    auto load = [](const char *value) {
        std::istringstream in(std::string("[search]\nmcts_vcn_pass_test_nodes = ") + value + "\n");
        ScopedCoutCapture  cap;
        bool               ok = Config::loadConfig(in);
        assert(ok);
        return SearchCfg.mctsVcnPassTestNodes;
    };

    // Act / Assert: the documented default, plain values, negative to 0, capped above
    assert(SearchConfig {}.mctsVcnPassTestNodes == DefaultPassTestNodes);
    assert(load("0") == 0);
    assert(load("500") == 500);
    assert(load("-3") == 0);
    assert(load("1000000") == 1000000);
    assert(load("5000000") == 1000000);
}

void config_oldMctsVcnPassPriorKey_ignoredWithoutError()
{
    // Arrange: a config written for the pass edge (the key was removed by VCN-04)
    ScopedSearchCfg    cfg;
    const SearchConfig before = SearchCfg;
    std::istringstream in("[search]\nmcts_vcn_pass_prior = 0.25\nmcts_min_edges = 9\n");

    // Act
    bool ok;
    {
        ScopedCoutCapture cap;
        ok = Config::loadConfig(in);
    }

    // Assert: loads; the removed key changes nothing, the known key next to it is read
    assert(ok);
    assert(SearchCfg.mctsMinEdges == 9);
    assert(SearchCfg.mctsVcnPassTestNodes == before.mctsVcnPassTestNodes);
}

/// Walks the graph below `node`, replaying the moves on `board`: counts the nodes and the edges
/// that hold a pass, and asserts every node key is the key of its board at the root level.
struct GraphWalk
{
    size_t nodes     = 0;
    size_t passEdges = 0;
};

void walkGraph(Node                       &node,
               Board                      &board,
               const SearchOptions        &opts,
               int                         rootDefenderPasses,
               std::unordered_set<Node *> &visited,
               GraphWalk                  &out)
{
    if (!visited.insert(&node).second)
        return;
    out.nodes++;
    assert(node.getHash() == mctsNodeKey(board, opts, rootDefenderPasses));
    assert(node.getHash() == (board.zobristKey() ^ Vcn::hashXor(opts, opts.vcnMode.n)));
    EdgeArray *edges = node.getEdges();
    if (!edges)
        return;
    for (uint32_t i = 0; i < edges->numEdges; i++) {
        Pos move = (*edges)[i].getMove();
        if (move == Pos::PASS) {
            out.passEdges++;
            continue;
        }
        Node *child = (*edges)[i].getChild();
        if (!child)
            continue;
        board.move(FREESTYLE, move);
        walkGraph(*child, board, opts, rootDefenderPasses, visited, out);
        board.undo(FREESTYLE);
    }
}

void mctsVcn_graphNeverContainsPassEdge()
{
    MctsEngineFixture fx;
    for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3}) {
        // Arrange: black attacks from `level` (side to move at the root).
        Search::Engine.clear(true);
        auto          board = makeBoard();
        SearchOptions vcn   = searchOptions(*board, 3000, level);
        assert(vcn.vcnMode.attacker == BLACK);

        // Act
        runMctsSearch(*board, vcn);

        // Assert: no pass edge anywhere; every node is at the root level.
        auto                       replay = makeBoard();
        std::unordered_set<Node *> visited;
        GraphWalk                  walk;
        walkGraph(*fx.searcher().graph.root,
                  *replay,
                  vcn,
                  fx.searcher().scratch.rootDefenderPasses,
                  visited,
                  walk);
        assert(walk.nodes > 10);
        assert(walk.passEdges == 0);
    }
}

// ----------------------------------------------------------------------------------------------
// VCN-03 slice 4: VC4 / VC5 terminal rules in evaluateNode, the VCF-only mode of SimpleVCF and its
// VCN-keyed VCF TT entries. evaluateNode and SimpleVCF are reached through the Testing forwarders
// (search/mcts/searchtesting.h) on a board bound to the engine's main thread (stand-in evaluator).
// Fixtures marked "as in vcn_tests.cpp" are copies of the AB VCN test positions.

/// Interleave black and white stones into a move sequence starting with black.
std::vector<Pos> alternateMoves(const std::vector<Pos> &black, const std::vector<Pos> &white)
{
    std::vector<Pos> moves;
    for (size_t i = 0; i < std::max(black.size(), white.size()); i++) {
        if (i < black.size())
            moves.push_back(black[i]);
        if (i < white.size())
            moves.push_back(white[i]);
    }
    return moves;
}

/// A single black stone and a far white stone. Black to move (as in vcn_tests.cpp).
std::vector<Pos> quietMoves()
{
    return {Pos(7, 7), Pos(3, 3)};
}

/// Black has an open three on row 7; white stones are far away. Black to move (vcn_tests.cpp).
std::vector<Pos> openThreeMoves()
{
    return {Pos(7, 7), Pos(3, 3), Pos(8, 7), Pos(3, 12), Pos(9, 7), Pos(12, 12)};
}

/// Black to move with a double open three at (5,5) and no four (vcn_tests.cpp doubleThreeBoard).
std::vector<Pos> doubleThreeMoves()
{
    return alternateMoves({Pos(3, 5), Pos(4, 5), Pos(5, 3), Pos(5, 4)},
                          {Pos(0, 0), Pos(14, 0), Pos(0, 14), Pos(14, 14)});
}

/// vcn_tests.cpp farVcfAfterForcedBlockPosition(): black to move, must block white's five at
/// (12,9) (a block four), white blocks at (13,9); black's only VCF is then far from (12,9).
std::vector<Pos> farVcfAfterForcedBlockMoves()
{
    return alternateMoves({Pos(3, 8),
                           Pos(3, 9),
                           Pos(4, 6),
                           Pos(5, 6),
                           Pos(6, 6),
                           Pos(5, 5),
                           Pos(6, 4),
                           Pos(9, 9),
                           Pos(10, 9),
                           Pos(11, 9),
                           Pos(12, 14)},
                          {Pos(3, 10),
                           Pos(7, 6),
                           Pos(7, 3),
                           Pos(8, 9),
                           Pos(12, 10),
                           Pos(12, 11),
                           Pos(12, 12),
                           Pos(12, 13),
                           Pos(0, 0),
                           Pos(2, 9),
                           Pos(2, 10)});
}

/// vcn_tests.cpp doubleThreeAfterForcedBlockPosition(): the same forced pair, after which black has
/// no four anywhere, only a double open three at (5,5).
std::vector<Pos> doubleThreeAfterForcedBlockMoves()
{
    return alternateMoves(
        {Pos(3, 5), Pos(4, 5), Pos(5, 3), Pos(5, 4), Pos(9, 9), Pos(10, 9), Pos(11, 9), Pos(12, 14)},
        {Pos(8, 9), Pos(12, 10), Pos(12, 11), Pos(12, 12), Pos(12, 13), Pos(0, 0), Pos(14, 0), Pos(0, 14)});
}

/// `moves` followed by the forced pair (12,9) / (13,9) of the two positions above.
std::vector<Pos> afterForcedPair(std::vector<Pos> moves)
{
    moves.push_back(Pos(12, 9));
    moves.push_back(Pos(13, 9));
    return moves;
}

/// Black has a four on row 7 closed at (7,4); its five point is (7,9). White to move.
std::vector<Pos> blackFourWhiteToMoveMoves()
{
    return alternateMoves({Pos(7, 5), Pos(7, 6), Pos(7, 7), Pos(7, 8)},
                          {Pos(7, 4), Pos(0, 0), Pos(0, 14)});
}

/// White to move with a four-three at (6,5): a block four on y = 5 (closed by black (2,5)) and a
/// flex three on x = 6. Black has no four. Used under Renju, where quickWinCheck reports this
/// C_BLOCK4_FLEX3 win only when its beta gate admits it.
std::vector<Pos> whiteFourThreeMoves()
{
    return alternateMoves({Pos(2, 5), Pos(0, 14), Pos(14, 0), Pos(14, 14), Pos(0, 7), Pos(12, 12)},
                          {Pos(3, 5), Pos(4, 5), Pos(5, 5), Pos(6, 3), Pos(6, 4)});
}

std::unique_ptr<Board> plainBoard(Rule rule, const std::vector<Pos> &moves)
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(rule);
    for (Pos p : moves)
        board->move(rule, p);
    return board;
}

/// The position bound to the engine's main thread, which gets a fresh stand-in evaluator.
std::unique_ptr<Board> leafBoard(Rule rule, const std::vector<Pos> &moves)
{
    auto src = plainBoard(rule, moves);
    return std::make_unique<Board>(*src, threadWithEvaluator(rule));
}

SearchOptions leafOptions(Rule rule, VCNLevel level, Color attacker)
{
    SearchOptions opts    = vcnOptions(level, attacker);
    opts.rule             = {rule, GameRule::FREEOPEN};
    if (level == VCNLevel::None)
        opts.vcnMode.attacker = BLACK;  // unused when VCN is off
    return opts;
}

struct LeafResult
{
    bool  terminal;
    Value value;    // the terminal value (VALUE_NONE when not terminal)
    float utility;  // the evaluated utility
};

/// Evaluates `board` as a freshly allocated non-root node (tree ply 1) of a search whose root had
/// `rootDefenderPasses` defender passes.
LeafResult evaluateLeaf(MctsEngineFixture   &fx,
                        Board               &board,
                        const SearchOptions &opts,
                        int                  rootDefenderPasses = 0)
{
    fx.searcher().scratch.rootDefenderPasses = rootDefenderPasses;
    Node node(mctsNodeKey(board, opts, rootDefenderPasses), 0);
    Testing::evaluateNode(node, opts, board, 1);
    const bool terminal = node.isTerminal();
    return {terminal, terminal ? Value(node.getBound().lower) : VALUE_NONE, node.getEvalUtility()};
}

void evaluateNode_vc4AttackerDoubleThreeOnly_attackerLoses()
{
    // Arrange: black attacks at VC4 with a double open three and no four (VCN-02 1b).
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, doubleThreeMoves());
    assert(board->pattern4(Pos(5, 5), BLACK) == F_FLEX3_2X && "fixture: (5,5) double three");
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: a double three is not a VCF; the defender passes and the attacker loses (AB value).
    assert(r.terminal);
    assert(r.value == mated_in(board->ply() + 2));
}

void evaluateNode_vc4AttackerFarVcf_attackerWins()
{
    // Arrange: after the forced pair black's only VCF starts far from its last stone (VCN-02 1).
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, afterForcedPair(farVcfAfterForcedBlockMoves()));
    assert(board->sideToMove() == BLACK);
    assert(board->getLastActualMoveOfSide(BLACK) == Pos(12, 9));
    assert(board->pattern4(Pos(3, 6), BLACK) >= E_BLOCK4 && "fixture: (3,6) must be a four");
    assert(board->p4Count(BLACK, B_FLEX4) + board->p4Count(BLACK, C_BLOCK4_FLEX3)
                   + board->p4Count(BLACK, F_FLEX3_2X)
               == 0
           && "fixture: nothing quickWinCheck() reports on its own");
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: the whole-board VCF picker finds it.
    assert(r.terminal);
    assert(r.value >= VALUE_MATE_IN_MAX_PLY);
}

void evaluateNode_vc4AttackerNoVcf_attackerLosesNotNnValue()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, quietMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: terminal loss, never the network value.
    assert(r.terminal);
    assert(r.value == mated_in(board->ply() + 2));
}

void evaluateNode_vc4DefenderNoFiveThreat_defenderWins()
{
    // Arrange: black (attacker) made a quiet move; white (defender) is to move at VC4.
    MctsEngineFixture fx;
    TT.clear();
    std::vector<Pos> moves = quietMoves();
    moves.push_back(Pos(10, 10));
    auto board = leafBoard(FREESTYLE, moves);
    assert(board->sideToMove() == WHITE);
    assert(!board->p4Count(BLACK, A_FIVE));
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: the defender wins by passing (AB vcfdefend value).
    assert(r.terminal);
    assert(r.value == mate_in(board->ply() + 1));
}

void evaluateNode_vc4DefenderFacingFive_notTerminalByPassRule()
{
    // Arrange: black (attacker) has a four, white (defender) to move must block at (7,9). After
    // the block black has no four left.
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, blackFourWhiteToMoveMoves());
    assert(board->sideToMove() == WHITE);
    assert(board->p4Count(BLACK, A_FIVE) == 1);
    assert(board->pattern4(Pos(7, 9), BLACK) == A_FIVE);
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: not the pass rule (mate_in(ply + 1)); the block line decides: after the forced block
    // the VC4 attacker has no VCF, so the defender wins two plies later (AB vcfdefend value).
    assert(r.value != mate_in(board->ply() + 1));
    assert(r.terminal);
    assert(r.value == mate_in(board->ply() + 3));
}

/// White has a four on y = 2 closed by black (1,2); its five point is (6,2), which is not a four for
/// black. Black (attacker) to move.
std::vector<Pos> whiteFourBlackToMoveMoves()
{
    return alternateMoves({Pos(1, 2), Pos(10, 10), Pos(12, 12), Pos(9, 13)},
                          {Pos(2, 2), Pos(3, 2), Pos(4, 2), Pos(5, 2)});
}

void evaluateNode_vc4AttackerBlocksFiveWithNonFour_attackerLoses()
{
    // Arrange: the VC4 attacker must block the defender's five, and the block is not a four.
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, whiteFourBlackToMoveMoves());
    assert(board->sideToMove() == BLACK);
    assert(board->p4Count(WHITE, A_FIVE) == 1);
    assert(board->pattern4(Pos(6, 2), WHITE) == A_FIVE);
    assert(board->pattern4(Pos(6, 2), BLACK) < E_BLOCK4 && "fixture: the block is not a four");
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: after the forced block the defender passes (AB vcfdefend), so a loss, not a draw.
    assert(r.terminal);
    assert(r.value == mated_in(board->ply() + 2));
}

/// Quiet position, then black (10,2) and a white pass: black to move, one defender pass after a
/// VC4 root, so the node level is VC5.
std::vector<Pos> vc5QuietMoves()
{
    std::vector<Pos> moves = quietMoves();
    moves.push_back(Pos(10, 2));
    moves.push_back(Pos::PASS);
    return moves;
}

void evaluateNode_vc5AttackerNoFive_attackerLoses()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, vc5QuietMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);
    assert(board->sideToMove() == BLACK);
    assert(mctsNodeVcnLevel(*board, opts, 0) == VCNLevel::VC5);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts, 0);

    // Assert
    assert(r.terminal);
    assert(r.value == mated_in(board->ply()));
}

void evaluateNode_vcnBoardFull_returnsDrawLikeAb()
{
    // Arrange: maxMoves reached (the board-full rule) at a VC3 attacker node.
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, quietMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);
    opts.maxMoves       = board->nonPassMoveCount();

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: a draw, as AB (decision §8.3)
    assert(r.terminal);
    assert(r.value == getDrawValue(*board, opts, board->ply()));
}

void evaluateNode_vc5AttackerNoFiveOnFullBoard_attackerLosesNotDraw()
{
    // Arrange: a VC5 attacker without a five at maxMoves.
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, vc5QuietMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);
    opts.maxMoves       = board->nonPassMoveCount();
    assert(mctsNodeVcnLevel(*board, opts, 0) == VCNLevel::VC5);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts, 0);

    // Assert: the VC5 loss is checked before the draw
    assert(r.terminal);
    assert(r.value == mated_in(board->ply()));
    assert(r.value != getDrawValue(*board, opts, board->ply()));
}

void evaluateNode_vc3_unchangedFromOrdinaryRules()
{
    // Arrange: VC3 nodes of both sides; VCN off is the reference.
    MctsEngineFixture             fx;
    std::vector<std::vector<Pos>> positions = {doubleThreeMoves(),
                                               quietMoves(),
                                               afterForcedPair(farVcfAfterForcedBlockMoves()),
                                               blackFourWhiteToMoveMoves()};
    for (const auto &moves : positions) {
        TT.clear();
        auto          board = leafBoard(FREESTYLE, moves);
        SearchOptions off   = leafOptions(FREESTYLE, VCNLevel::None, BLACK);
        SearchOptions vc3   = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);

        // Act
        LeafResult a = evaluateLeaf(fx, *board, off);
        LeafResult b = evaluateLeaf(fx, *board, vc3);

        // Assert
        assert(a.terminal == b.terminal);
        assert(a.value == b.value);
        if (!a.terminal)
            assert(a.utility == b.utility);
    }
}

void evaluateNode_vcnOff_unchanged()
{
    // Arrange / Act / Assert per position: the values evaluateNode gave before slice 4.
    MctsEngineFixture fx;
    SearchOptions     off = leafOptions(FREESTYLE, VCNLevel::None, BLACK);

    {  // double three: quickWinCheck's double-three rule
        TT.clear();
        auto       board = leafBoard(FREESTYLE, doubleThreeMoves());
        LeafResult r     = evaluateLeaf(fx, *board, off);
        assert(r.terminal && r.value == mate_in(board->ply() + 5));
    }
    {  // quiet: the network value
        TT.clear();
        auto       board = leafBoard(FREESTYLE, quietMoves());
        LeafResult r     = evaluateLeaf(fx, *board, off);
        assert(!r.terminal);
        assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
    }
    {  // facing a single five, no VCF after the block: SimpleVCF stands pat, network value
        TT.clear();
        auto       board = leafBoard(FREESTYLE, blackFourWhiteToMoveMoves());
        LeafResult r     = evaluateLeaf(fx, *board, off);
        assert(!r.terminal);
    }
    {  // maxMoves: draw
        TT.clear();
        auto          board = leafBoard(FREESTYLE, quietMoves());
        SearchOptions o     = off;
        o.maxMoves          = board->nonPassMoveCount();
        LeafResult r        = evaluateLeaf(fx, *board, o);
        assert(r.terminal && r.value == getDrawValue(*board, o, board->ply()));
    }
    {  // Renju, white four-three (the wrapper's beta is pinned in quickWinCheckRuntime_*)
        TT.clear();
        auto board = leafBoard(RENJU, whiteFourThreeMoves());
        assert(board->sideToMove() == WHITE);
        assert(board->pattern4(Pos(6, 5), WHITE) == C_BLOCK4_FLEX3 && "fixture: four-three");
        SearchOptions o = leafOptions(RENJU, VCNLevel::None, BLACK);
        LeafResult    r = evaluateLeaf(fx, *board, o);
        assert(r.terminal && r.value == mate_in(board->ply() + 5));
    }
}

void simpleVcf_vcfOnlyMode_ttKeySeparatedFromPlain()
{
    // Arrange: a quiet position, black (VC4 attacker) to move, no four anywhere.
    MctsEngineFixture fx;
    auto              board = leafBoard(FREESTYLE, quietMoves());
    SearchOptions     vc4   = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);
    SearchOptions     off   = leafOptions(FREESTYLE, VCNLevel::None, BLACK);
    const HashKey     zob   = board->zobristKey();
    const HashKey     vcnKey = zob ^ Vcn::hashXor(vc4, VCNLevel::VC4);
    const Value       fake   = mate_in(board->ply() + 9);  // a planted, wrong "win"
    assert(vcnKey != zob);

    // Act 1: a plain-VCF entry for this position must not satisfy a VC4 probe.
    TT.clear();
    TT.store(zob, fake, VALUE_NONE, true, BOUND_EXACT, Pos::NONE, 0, 0);
    Value vc4Value = Testing::simpleVcf(*board, vc4, VCNLevel::VC4, 1);

    // Assert 1: the VC4 attacker has no VCF, so it loses (no stand-pat), the plain entry unseen.
    assert(vc4Value == mated_in(board->ply() + 2));

    // Act 2: a VC4 entry must not satisfy a plain probe, and the VC4 probe uses that key.
    TT.clear();
    TT.store(vcnKey, fake, VALUE_NONE, true, BOUND_EXACT, Pos::NONE, 0, 0);
    Value plainValue = Testing::simpleVcf(*board, off, VCNLevel::None, 1);
    Value vc4Again   = Testing::simpleVcf(*board, vc4, VCNLevel::VC4, 1);

    // Assert 2
    assert(plainValue == VALUE_ZERO);
    assert(vc4Again == fake);
    TT.clear();
}

void quickWinCheckRuntime_vcfOnlyFlag_matchesTemplate()
{
    for (Rule rule : {FREESTYLE, STANDARD}) {
        // Arrange: a double three and no four
        auto board = plainBoard(rule, doubleThreeMoves());
        assert(board->pattern4(Pos(5, 5), BLACK) == F_FLEX3_2X && "fixture: double three");

        // Act
        Value dflt    = quickWinCheck(rule, *board, 0);
        Value plain   = quickWinCheck(rule, *board, 0, VALUE_MATE_IN_MAX_PLY, false);
        Value vcfOnly = quickWinCheck(rule, *board, 0, VALUE_MATE_IN_MAX_PLY, true);

        // Assert: the flag reaches the template; the default is unchanged
        Value tplPlain = rule == FREESTYLE
                             ? quickWinCheck<FREESTYLE>(*board, 0, VALUE_MATE_IN_MAX_PLY, false)
                             : quickWinCheck<STANDARD>(*board, 0, VALUE_MATE_IN_MAX_PLY, false);
        Value tplVcf = rule == FREESTYLE
                           ? quickWinCheck<FREESTYLE>(*board, 0, VALUE_MATE_IN_MAX_PLY, true)
                           : quickWinCheck<STANDARD>(*board, 0, VALUE_MATE_IN_MAX_PLY, true);
        assert(dflt == mate_in(5));
        assert(plain == tplPlain && plain == mate_in(5));
        assert(vcfOnly == tplVcf && vcfOnly == VALUE_ZERO);
    }

    // Renju: the wrapper's default beta (VALUE_MATE_IN_MAX_PLY) admits the four-three check that
    // the template's default (VALUE_INFINITE) skips; passing vcfOnly must keep the wrapper's beta.
    auto renju = plainBoard(RENJU, whiteFourThreeMoves());
    assert(renju->pattern4(Pos(6, 5), WHITE) == C_BLOCK4_FLEX3 && "fixture: four-three");
    assert(quickWinCheck(RENJU, *renju, 0) == mate_in(5));
    assert(quickWinCheck(RENJU, *renju, 0, VALUE_MATE_IN_MAX_PLY, true) == mate_in(5));
    assert(quickWinCheck<RENJU>(*renju, 0) == VALUE_ZERO);
}

// --- AB cross-check and VCN-on smoke run --------------------------------------------------------

enum class MateClass { Win, Loss, Draw, None };

MateClass mateClass(bool terminal, Value v)
{
    if (!terminal)
        return MateClass::None;
    return v >= VALUE_MATE_IN_MAX_PLY ? MateClass::Win
           : v <= VALUE_MATED_IN_MAX_PLY ? MateClass::Loss
                                          : MateClass::Draw;
}

const char *className(MateClass c)
{
    switch (c) {
    case MateClass::Win: return "win";
    case MateClass::Loss: return "loss";
    case MateClass::Draw: return "draw";
    default: return "none";
    }
}

/// AB's VCN answer at `moves` (the side to move attacks from `level`): the root move value of an
/// alpha-beta search driven directly (not through the protocol), depth 8 as tests/vcn_tests.cpp.
/// Runs on the default searcher and built-in evaluation; call outside MctsEngineFixture.
Value abVcnValue(const std::vector<Pos> &moves, VCNLevel level)
{
    Search::Engine.waitForIdle();
    Search::Engine.setupSearcher(Search::createSearcher(""));
    Search::Engine.clear(true);
    auto          board = plainBoard(FREESTYLE, moves);
    SearchOptions o;
    o.rule                = {FREESTYLE, GameRule::FREEOPEN};
    o.disableOpeningQuery = true;
    o.maxDepth            = 8;
    o.vcnMode.n           = level;
    o.vcnMode.attacker    = board->sideToMove();
    {
        ScopedCoutCapture cap;
        Search::Engine.startThinking(*board, o);
        Search::Engine.waitForIdle();
    }
    Value v = Search::Engine.main()->rootMoves[0].value;
    Search::Engine.clear(true);  // the TT is shared with the MCTS VCF (same VCN key formula)
    return v;
}

void evaluateNode_vcnLeaves_matchAbSignAndMateClass()
{
    // Arrange: VC4 / VC5 attacker-to-move leaves from tests/vcn_tests.cpp (plus a VC5 attacker
    // that has a five). AB is asked at the same position and level; AB's value is relative to its
    // root (ply 0), MCTS's to the game start (board.ply()), so only sign and class are compared.
    std::vector<Pos> fiveOnBoard = openThreeMoves();
    fiveOnBoard.push_back(Pos(10, 7));  // black open four
    fiveOnBoard.push_back(Pos(13, 13));
    struct Case
    {
        const char      *name;
        std::vector<Pos> moves;
        VCNLevel         level;
        Value            ab;
    };
    std::vector<Case> cases = {
        {"farVcfAfterPair VC4", afterForcedPair(farVcfAfterForcedBlockMoves()), VCNLevel::VC4},
        {"doubleThreeAfterPair VC4",
         afterForcedPair(doubleThreeAfterForcedBlockMoves()),
         VCNLevel::VC4},
        {"doubleThreeAfterPair VC3",
         afterForcedPair(doubleThreeAfterForcedBlockMoves()),
         VCNLevel::VC3},
        {"quiet VC4", quietMoves(), VCNLevel::VC4},
        {"openThree VC4", openThreeMoves(), VCNLevel::VC4},
        {"quiet VC5", quietMoves(), VCNLevel::VC5},
        {"fiveOnBoard VC5", fiveOnBoard, VCNLevel::VC5},
        {"whiteFourNonFourBlock VC4", whiteFourBlackToMoveMoves(), VCNLevel::VC4},
    };
    for (Case &c : cases)
        c.ab = abVcnValue(c.moves, c.level);

    MctsEngineFixture fx;
    for (const Case &c : cases) {
        TT.clear();
        auto          board = leafBoard(FREESTYLE, c.moves);
        SearchOptions opts  = leafOptions(FREESTYLE, c.level, board->sideToMove());

        // Act
        LeafResult r = evaluateLeaf(fx, *board, opts);

        // Assert
        MateClass ab = mateClass(true, c.ab), mcts = mateClass(r.terminal, r.value);
        std::printf("  cross-check %-26s AB %6d (%s)  MCTS %6d (%s)\n",
                    c.name,
                    int(c.ab),
                    className(ab),
                    int(r.value),
                    className(mcts));
        std::fflush(stdout);
        assert(mcts == ab);
    }
}

// ----------------------------------------------------------------------------------------------
// VCN-04 (option G): the defender's pass is a test at defender leaves, not an edge. Root level
// VC3: exact (null move, then the fours-only VCF). Root level VC2: the static rule (stage 1) and
// the bounded VCT solver (stage 2, SearchCfg.mctsVcnPassTestNodes). Only "no threat" ends a node.

using Testing::PassTestResult;

/// quietMoves() plus a black stone that is on no common line with (7,7): black has no cell that
/// makes a three or a four (it has cells that make two open twos, class J_FLEX2_2X, which is
/// above K_BLOCK3 in the enum and holds no three). White (defender) to move.
std::vector<Pos> quietDefenderMoves()
{
    return {Pos(7, 7), Pos(3, 3), Pos(10, 2)};
}

/// Black has two in a row on row 7; white stones are far away. White to move.
std::vector<Pos> twoInRowDefenderMoves()
{
    return {Pos(7, 7), Pos(3, 3), Pos(8, 7)};
}

/// The same two in a row with black to move (the position after white's null move, with one more
/// far white stone to keep the stone counts legal).
std::vector<Pos> twoInRowAttackerMoves()
{
    return {Pos(7, 7), Pos(3, 3), Pos(8, 7), Pos(3, 12)};
}

/// Black has an open three on row 7, white (defender) to move.
std::vector<Pos> openThreeDefenderMoves()
{
    return {Pos(7, 7), Pos(3, 3), Pos(8, 7), Pos(3, 12), Pos(9, 7)};
}

/// White (defender, to move) has an open three on row 3; the four black stones share no line.
std::vector<Pos> defenderOpenThreeMoves()
{
    return alternateMoves({Pos(7, 7), Pos(10, 12), Pos(13, 5), Pos(0, 9)},
                          {Pos(3, 3), Pos(4, 3), Pos(5, 3)});
}

/// Black to move with a double three at (5,5). White's three on the diagonal (7,6)-(9,8) is
/// closed by black (11,10) and (5,4), so white (6,5) is a block four that also closes black's
/// row three: the defender's refutation of the double three is a counter four.
std::vector<Pos> doubleThreeCounterFourMoves()
{
    return alternateMoves({Pos(3, 5), Pos(4, 5), Pos(5, 3), Pos(5, 4), Pos(11, 10)},
                          {Pos(7, 6), Pos(8, 7), Pos(9, 8), Pos(0, 14), Pos(14, 0)});
}

/// The control of the position above: the same black stones, white without the three.
std::vector<Pos> doubleThreeNoCounterMoves()
{
    return alternateMoves({Pos(3, 5), Pos(4, 5), Pos(5, 3), Pos(5, 4), Pos(11, 10)},
                          {Pos(14, 7), Pos(0, 0), Pos(14, 14), Pos(0, 14), Pos(14, 0)});
}

/// Black to move. Row 7: black (5,7) (6,7) (8,7) closed by white (4,7), so (7,7) is a block four
/// and black has no VCF. Black (7,9) is alone on column 7: the quiet move (7,10) makes only a two
/// there, yet after it the four at (7,7) also makes an open three on the column (a four-three).
std::vector<Pos> quietMoveEnablesVcfMoves()
{
    return alternateMoves({Pos(5, 7), Pos(6, 7), Pos(8, 7), Pos(7, 9)},
                          {Pos(4, 7), Pos(0, 0), Pos(14, 0), Pos(0, 14)});
}

struct BoardSnapshot
{
    HashKey  key;
    int      ply;
    Color    side;
    int      passes[SIDE_NB];
    Pos      lastMove;
    uint16_t p4[SIDE_NB][PATTERN4_NB];

    explicit BoardSnapshot(const Board &b)
        : key(b.zobristKey())
        , ply(b.ply())
        , side(b.sideToMove())
        , passes {b.passMoveCountOfSide(BLACK), b.passMoveCountOfSide(WHITE)}
        , lastMove(b.getLastMove())
    {
        for (Color c : {BLACK, WHITE})
            for (int p = 0; p < PATTERN4_NB; p++)
                p4[c][p] = b.p4Count(c, Pattern4(p));
    }

    bool operator==(const BoardSnapshot &o) const
    {
        return key == o.key && ply == o.ply && side == o.side && passes[BLACK] == o.passes[BLACK]
               && passes[WHITE] == o.passes[WHITE] && lastMove == o.lastMove
               && std::memcmp(p4, o.p4, sizeof(p4)) == 0;
    }
};

/// evaluateLeaf with the stage 2 budget set to `passTestNodes` for the call.
LeafResult evaluateLeafWithBudget(MctsEngineFixture   &fx,
                                  Board               &board,
                                  const SearchOptions &opts,
                                  int                  passTestNodes)
{
    ScopedSearchCfg cfg;
    SearchCfg.mctsVcnPassTestNodes = passTestNodes;
    return evaluateLeaf(fx, board, opts);
}

constexpr int LargeBudget = 200000;

std::vector<Pos> admissibleMoves(Board &board, const SearchOptions &opts)
{
    Pos      moves[MAX_MOVES];
    uint32_t n = Testing::vctAdmissibleMoves(board, opts, moves);
    return std::vector<Pos>(moves, moves + n);
}

bool contains(const std::vector<Pos> &v, Pos p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

// --- evaluateNode, root level VC3 (exact) --------------------------------------------------------

void evaluateNode_vc3DefenderAttackerHasNoVcfAfterNull_defenderWinsTerminal()
{
    // Arrange: black (attacker) made a quiet move; white (defender) is to move at VC3.
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, quietDefenderMoves());
    assert(board->sideToMove() == WHITE);
    assert(!board->p4Count(BLACK, A_FIVE));
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);

    // The value of the old pass child: the VC4 VCF of the attacker after the defender's pass.
    auto passed = leafBoard(FREESTYLE, quietDefenderMoves());
    passed->move(FREESTYLE, Pos::PASS);
    Value passChild = Testing::simpleVcf(*passed, opts, VCNLevel::VC4, 2);
    assert(passChild <= VALUE_MATED_IN_MAX_PLY && "fixture: no VCF after the null move");
    TT.clear();

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: the defender wins by the pass test, with the negated value of the old pass child.
    assert(r.terminal);
    assert(r.value == -passChild);
    assert(r.value == mate_in(board->ply() + 3));
}

void evaluateNode_vc3DefenderAttackerHasVcfAfterNull_notTerminal()
{
    // Arrange: black (attacker) has an open three; white (defender) is to move at VC3.
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, openThreeDefenderMoves());
    assert(board->sideToMove() == WHITE);
    assert(board->p4Count(BLACK, B_FLEX4));
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: a pass loses to the open four, so the defender must answer: an ordinary node.
    assert(!r.terminal);
    assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
}

void evaluateNode_vc3DefenderFacingFive_noPassTest()
{
    // Arrange: black (attacker) has a four, white (defender) to move must block; VCN off is the
    // reference (after the block black has no VCF, so the node takes the network value).
    MctsEngineFixture fx;
    for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3}) {
        TT.clear();
        auto board = leafBoard(FREESTYLE, blackFourWhiteToMoveMoves());
        assert(board->sideToMove() == WHITE);
        assert(board->p4Count(BLACK, A_FIVE) == 1);
        SearchOptions off = leafOptions(FREESTYLE, VCNLevel::None, BLACK);
        SearchOptions vcn = leafOptions(FREESTYLE, level, BLACK);

        // Act
        LeafResult a = evaluateLeaf(fx, *board, off);
        LeafResult b = evaluateLeafWithBudget(fx, *board, vcn, LargeBudget);

        // Assert: a pass is no answer to a five, so the pass test does not run.
        assert(!a.terminal && !b.terminal);
        assert(a.utility == b.utility);
    }
}

void evaluateNode_vc2DefenderHasOwnFlexFour_defenderWinStaysFromQuickWin()
{
    // Arrange: white (defender, to move) has an open three; black has nothing.
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, defenderOpenThreeMoves());
    assert(board->sideToMove() == WHITE);
    assert(board->p4Count(WHITE, B_FLEX4));
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    LeafResult r = evaluateLeafWithBudget(fx, *board, opts, 0);

    // Assert: the defender's own win (rule 3) decides, with its value, not the pass-test value.
    assert(r.terminal);
    assert(r.value == mate_in(board->ply() + 3));
}

void evaluateNode_vc3AttackerToMove_noPassTest()
{
    // Arrange: quiet attacker leaves at VC2 and VC3: the test is for defender leaves only.
    MctsEngineFixture fx;
    for (VCNLevel level : {VCNLevel::VC2, VCNLevel::VC3}) {
        TT.clear();
        auto          board = leafBoard(FREESTYLE, quietMoves());
        SearchOptions opts  = leafOptions(FREESTYLE, level, BLACK);
        assert(board->sideToMove() == BLACK);

        // Act
        LeafResult r = evaluateLeafWithBudget(fx, *board, opts, LargeBudget);

        // Assert
        assert(!r.terminal);
        assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
    }
}

void evaluateNode_vc3PassTestMateBeyondMaxMoves_draw()
{
    // Arrange: the pass-test win would end after maxMoves (one stone is left to play).
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, quietDefenderMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);
    opts.maxMoves       = board->nonPassMoveCount() + 1;

    // Act
    LeafResult r = evaluateLeaf(fx, *board, opts);

    // Assert: a draw, as rule 3 does for a mate longer than maxMoves
    assert(r.terminal);
    assert(r.value == getDrawValue(*board, opts, board->ply()));
}

void evaluateNode_vc3DefenderPassTest_agreesWithAbRootLoss()
{
    // Arrange: AB's answer for the quiet root at VC3 (the attacker loses: every move is refuted
    // by a pass), then the MCTS defender leaf below a sample of root moves.
    const Value ab = abVcnValue(quietMoves(), VCNLevel::VC3);
    assert(ab <= VALUE_MATED_IN_MAX_PLY && "precondition: AB says the VC3 attacker loses");

    MctsEngineFixture fx;
    for (Pos rootMove : {Pos(8, 8), Pos(7, 8), Pos(10, 10), Pos(1, 13)}) {
        TT.clear();
        std::vector<Pos> moves = quietMoves();
        moves.push_back(rootMove);
        auto          board = leafBoard(FREESTYLE, moves);
        SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC3, BLACK);
        assert(board->sideToMove() == WHITE);

        // Act
        LeafResult r = evaluateLeaf(fx, *board, opts);

        // Assert: the defender wins at the leaf, so the root move loses, as in AB
        assert(mateClass(r.terminal, r.value) == MateClass::Win);
    }
}

// --- evaluateNode, root level VC2 (static rule, solver) ------------------------------------------

void evaluateNode_vc2DefenderAttackerHasNoThreeOrFourMove_defenderWinsTerminal()
{
    // Arrange: black (attacker) has two stones on no common line; stage 2 is off (budget 0).
    MctsEngineFixture fx;
    TT.clear();
    auto board = leafBoard(FREESTYLE, quietDefenderMoves());
    assert(board->sideToMove() == WHITE);
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    LeafResult r = evaluateLeafWithBudget(fx, *board, opts, 0);

    // Assert: no move of the attacker can threaten a VCF, so a pass refutes the last move
    assert(r.terminal);
    assert(r.value == mate_in(board->ply() + 5));
}

void evaluateNode_vc2DefenderAttackerHasThreeMove_notEndedByStaticRule()
{
    // Arrange: black has two in a row (a three-making move exists); stage 2 is off.
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowDefenderMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    assert(board->sideToMove() == WHITE);

    // Act
    LeafResult r = evaluateLeafWithBudget(fx, *board, opts, 0);

    // Assert: unknown, so an ordinary node with the network value
    assert(!r.terminal);
    assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
}

void evaluateNode_vc2DefenderSolverProvesNoThreat_defenderWinsTerminal()
{
    // Arrange: the same two in a row, with the solver on: two in a row and one free move do not
    // make a VC3 win.
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowDefenderMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    LeafResult r = evaluateLeafWithBudget(fx, *board, opts, LargeBudget);

    // Assert
    assert(r.terminal);
    assert(r.value == mate_in(board->ply() + 5));
}

void evaluateNode_vc2DefenderSolverThreatOrUnknown_notTerminal()
{
    MctsEngineFixture fx;
    {  // Threat: black has an open three, a pass loses
        TT.clear();
        auto          board = leafBoard(FREESTYLE, openThreeDefenderMoves());
        SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
        LeafResult    r     = evaluateLeafWithBudget(fx, *board, opts, LargeBudget);
        assert(!r.terminal);
        assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
    }
    {  // Unknown: the budget runs out before the two-in-a-row position is decided
        TT.clear();
        auto          board = leafBoard(FREESTYLE, twoInRowDefenderMoves());
        SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
        LeafResult    r     = evaluateLeafWithBudget(fx, *board, opts, 3);
        assert(!r.terminal);
        assert(r.utility == Evaluation::computeEvaluatorValue(*board).winLossRate());
    }
}

void evaluateNode_passTest_boardUnchanged()
{
    MctsEngineFixture fx;
    struct Case
    {
        std::vector<Pos> moves;
        VCNLevel         level;
        int              budget;
    };
    const std::vector<Case> cases = {
        {quietDefenderMoves(), VCNLevel::VC3, 0},               // VC3 terminal
        {openThreeDefenderMoves(), VCNLevel::VC3, 0},           // VC3 not terminal
        {quietDefenderMoves(), VCNLevel::VC2, 0},               // VC2 static
        {twoInRowDefenderMoves(), VCNLevel::VC2, LargeBudget},  // VC2 solver proof
        {twoInRowDefenderMoves(), VCNLevel::VC2, 3},            // VC2 solver budget out
        {openThreeDefenderMoves(), VCNLevel::VC2, LargeBudget}, // VC2 solver threat
    };
    for (const Case &c : cases) {
        // Arrange
        TT.clear();
        auto                board = leafBoard(FREESTYLE, c.moves);
        SearchOptions       opts  = leafOptions(FREESTYLE, c.level, BLACK);
        const BoardSnapshot before(*board);

        // Act
        evaluateLeafWithBudget(fx, *board, opts, c.budget);

        // Assert
        assert(BoardSnapshot(*board) == before);
    }
}

// --- the static rule ----------------------------------------------------------------------------

void vcnPassTestStatic_noThreeOrFourCell_noThreat()
{
    // Arrange: two black stones on no common line; (10,4) makes two open twos, no three
    auto          board = plainBoard(FREESTYLE, quietDefenderMoves());
    assert(board->pattern4(Pos(10, 4), BLACK) == J_FLEX2_2X && "fixture: a double two exists");
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    PassTestResult r = Testing::vcnPassTestStatic(*board, opts);

    // Assert
    assert(r == PassTestResult::NoThreat);
}

void vcnPassTestStatic_threeMakingCell_unknown()
{
    // Arrange: two in a row; (9,7) makes an open three
    auto board = plainBoard(FREESTYLE, twoInRowDefenderMoves());
    assert(board->pattern4(Pos(9, 7), BLACK) >= K_BLOCK3);
    SearchOptions opts = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    PassTestResult r = Testing::vcnPassTestStatic(*board, opts);

    // Assert
    assert(r == PassTestResult::Unknown);
}

void vcnPassTestStatic_renjuBlackAttackerForbidCell_unknown()
{
    // Arrange: Renju, black attacks and has a pattern-forbidden cell
    auto board = plainBoard(RENJU, renjuBlackForbiddenMoves());
    assert(board->p4Count(BLACK, FORBID) > 0);
    SearchOptions opts = leafOptions(RENJU, VCNLevel::VC2, BLACK);

    // Act
    PassTestResult r = Testing::vcnPassTestStatic(*board, opts);

    // Assert
    assert(r == PassTestResult::Unknown);
}

// --- the bounded VCT solver ---------------------------------------------------------------------

struct SolveResult
{
    PassTestResult result;
    uint32_t       nodes;
};

SolveResult solve(Board &board, const SearchOptions &opts, uint32_t budget)
{
    uint32_t       nodes = 12345;
    PassTestResult r     = Testing::vctSolve(board, opts, budget, &nodes);
    return {r, nodes};
}

void vctSolve_openThreeAttacker_threat()
{
    // Arrange: black to move with an open three
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, openThreeMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult r = solve(*board, opts, 100);

    // Assert
    assert(r.result == PassTestResult::Threat);
}

void vctSolve_doubleThreeAttacker_threat()
{
    // Arrange: black to move with a double three, white has no four
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, doubleThreeNoCounterMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    assert(board->pattern4(Pos(5, 5), BLACK) == F_FLEX3_2X);

    // Act
    SolveResult r = solve(*board, opts, 100);

    // Assert
    assert(r.result == PassTestResult::Threat);
}

void vctSolve_quietPosition_noThreat()
{
    // Arrange: one black stone
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, quietMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult r = solve(*board, opts, 100);

    // Assert: no move can threaten a VCF; decided without placing a stone (the only work is
    // the VCF probe of the root)
    assert(r.result == PassTestResult::NoThreat);
    assert(r.nodes <= 2);
}

void vctSolve_twoInRow_noThreatWithinBudget()
{
    // Arrange: black to move with two in a row
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowAttackerMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult r = solve(*board, opts, LargeBudget);

    // Assert
    std::printf("  vctSolve two-in-a-row: %u nodes\n", r.nodes);
    assert(r.result == PassTestResult::NoThreat);
    assert(r.nodes > 0 && r.nodes < uint32_t(LargeBudget));
}

void vctSolve_budgetZero_unknown()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, openThreeMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult r = solve(*board, opts, 0);

    // Assert: stage 2 is off
    assert(r.result == PassTestResult::Unknown);
    assert(r.nodes == 0);
}

void vctSolve_budgetExhausted_unknown()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowAttackerMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult r = solve(*board, opts, 3);

    // Assert: the work count stops at the budget, plus at most the last VCF search
    assert(r.result == PassTestResult::Unknown);
    assert(r.nodes >= 3 && r.nodes <= 3 + 16);
}

void vctSolve_defenderCounterFourRefutes_noThreatOrUnknownNeverThreat()
{
    // Arrange: the double three and the defender's counter four; the control has no counter
    MctsEngineFixture fx;
    TT.clear();
    auto          board   = leafBoard(FREESTYLE, doubleThreeCounterFourMoves());
    SearchOptions opts    = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    assert(board->sideToMove() == BLACK);
    assert(board->pattern4(Pos(5, 5), BLACK) == F_FLEX3_2X && "fixture: double three");
    assert(board->pattern4(Pos(6, 5), WHITE) >= E_BLOCK4 && "fixture: counter four");
    assert(!board->p4Count(WHITE, B_FLEX4) && "fixture: white's three is closed");

    // Act
    SolveResult r = solve(*board, opts, 3000);

    // Assert: never a proven threat
    std::printf("  vctSolve counter-four: result %d, %u nodes\n", int(r.result), r.nodes);
    assert(r.result != PassTestResult::Threat);
}

void vctSolve_quietMoveEnablesVcf_isCandidate()
{
    // Arrange: black has a four cell and no VCF; (7,10) makes only a two
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, quietMoveEnablesVcfMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    SearchOptions vc4   = leafOptions(FREESTYLE, VCNLevel::VC4, BLACK);
    assert(board->sideToMove() == BLACK);
    assert(board->pattern4(Pos(7, 7), BLACK) >= E_BLOCK4 && "fixture: a four cell exists");
    assert(board->pattern4(Pos(7, 10), BLACK) < K_BLOCK3 && "fixture: the move is quiet");
    assert(Testing::simpleVcf(*board, vc4, VCNLevel::VC4, 1) <= VALUE_MATED_IN_MAX_PLY
           && "fixture: no VCF yet");
    TT.clear();

    // Act
    std::vector<Pos> admissible = admissibleMoves(*board, opts);
    SolveResult      r          = solve(*board, opts, 2000);

    // Assert: the quiet move is searched (candidates are every legal move that passes the exact
    // test, not a pattern shortlist), so the position is not declared free of threats
    assert(contains(admissible, Pos(7, 10)));
    assert(r.result != PassTestResult::NoThreat);
}

void vctAdmissibleMoves_fourAndVcfThreatOnly()
{
    // Arrange: black to move with two in a row
    MctsEngineFixture fx;
    TT.clear();
    auto                board = leafBoard(FREESTYLE, twoInRowAttackerMoves());
    SearchOptions       opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    const BoardSnapshot before(*board);

    // Act
    std::vector<Pos> admissible = admissibleMoves(*board, opts);

    // Assert: the moves that make an open three are in; quiet moves are out; all legal
    assert(contains(admissible, Pos(9, 7)));
    assert(contains(admissible, Pos(6, 7)));
    assert(!contains(admissible, Pos(7, 8)));
    assert(!contains(admissible, Pos(12, 3)));
    for (Pos p : admissible)
        assert(board->isInBoard(p) && board->isEmpty(p));
    assert(BoardSnapshot(*board) == before);
}

void vctSolve_attackerMustBlockFive_onlyBlockConsidered()
{
    // Arrange: white (defender) has a four; black (attacker) must block at (6,2), which makes
    // no threat
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, whiteFourBlackToMoveMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
    assert(board->p4Count(WHITE, A_FIVE) == 1);

    // Act
    std::vector<Pos> admissible = admissibleMoves(*board, opts);
    SolveResult      r          = solve(*board, opts, 1000);

    // Assert: nothing but the block may be searched; the block is no threat, so no win
    for (Pos p : admissible)
        assert(p == Pos(6, 2));
    assert(r.result == PassTestResult::NoThreat);
    assert(r.nodes <= 8);
}

void vctSolve_provenResultCached_secondCallUsesNoNodes()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowAttackerMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult first  = solve(*board, opts, LargeBudget);
    SolveResult second = solve(*board, opts, LargeBudget);

    // Assert
    assert(first.result == PassTestResult::NoThreat && first.nodes > 0);
    assert(second.result == PassTestResult::NoThreat);
    assert(second.nodes == 0);
}

void vctSolve_unknownNotCachedAsProof_largerBudgetStillProves()
{
    // Arrange
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(FREESTYLE, twoInRowAttackerMoves());
    SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);

    // Act
    SolveResult small = solve(*board, opts, 3);
    SolveResult again = solve(*board, opts, 3);
    SolveResult large = solve(*board, opts, LargeBudget);

    // Assert: an exhausted budget leaves no proof behind
    assert(small.result == PassTestResult::Unknown);
    assert(again.result == PassTestResult::Unknown);
    assert(large.result == PassTestResult::NoThreat);
}

void vctSolve_foreignTtEntryUnderCacheKey_ignored()
{
    MctsEngineFixture fx;
    {  // Arrange: a VCF-shaped "win" planted under the cache key of a quiet position
        TT.clear();
        auto          board = leafBoard(FREESTYLE, quietMoves());
        SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
        const HashKey key   = Testing::vctCacheKey(*board, opts);
        assert(key != board->zobristKey());
        assert(key != (board->zobristKey() ^ Vcn::hashXor(opts, VCNLevel::VC3)));
        TT.store(key, mate_in(board->ply() + 9), VALUE_NONE, true, BOUND_EXACT, Pos::NONE,
                 (int)DEPTH_QVCF, 0);

        // Act
        SolveResult r = solve(*board, opts, 100);

        // Assert: computed, not read
        assert(r.result == PassTestResult::NoThreat);
    }
    {  // A VCF-shaped "loss" under the cache key of a won position
        TT.clear();
        auto          board = leafBoard(FREESTYLE, openThreeMoves());
        SearchOptions opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
        TT.store(Testing::vctCacheKey(*board, opts), mated_in(board->ply() + 2), VALUE_NONE, true,
                 BOUND_EXACT, Pos::NONE, (int)DEPTH_QVCF, 0);

        SolveResult r = solve(*board, opts, 100);

        assert(r.result == PassTestResult::Threat);
    }
    TT.clear();
}

void vctSolve_boardUnchanged()
{
    MctsEngineFixture fx;
    struct Case
    {
        std::vector<Pos> moves;
        uint32_t         budget;
    };
    const std::vector<Case> cases = {{twoInRowAttackerMoves(), uint32_t(LargeBudget)},
                                     {twoInRowAttackerMoves(), 3},
                                     {doubleThreeCounterFourMoves(), 500},
                                     {quietMoveEnablesVcfMoves(), 50},
                                     {openThreeMoves(), 100}};
    for (const Case &c : cases) {
        // Arrange
        TT.clear();
        auto                board = leafBoard(FREESTYLE, c.moves);
        SearchOptions       opts  = leafOptions(FREESTYLE, VCNLevel::VC2, BLACK);
        const BoardSnapshot before(*board);

        // Act
        solve(*board, opts, c.budget);

        // Assert
        assert(BoardSnapshot(*board) == before);
    }
}

void vctSolve_renjuBlackAttacker_forbiddenMoveNotCandidate()
{
    // Arrange: Renju, black (attacker) to move; (7,7) is a real double-three forbidden point
    MctsEngineFixture fx;
    TT.clear();
    auto          board = leafBoard(RENJU, renjuBlackForbiddenMoves());
    SearchOptions opts  = leafOptions(RENJU, VCNLevel::VC2, BLACK);
    assert(board->sideToMove() == BLACK);
    assert(board->checkForbiddenPoint(Pos(7, 7)));

    // Act
    std::vector<Pos> admissible = admissibleMoves(*board, opts);

    // Assert: the forbidden point is not a move; a legal three-making move is
    assert(!contains(admissible, Pos(7, 7)));
    assert(contains(admissible, Pos(7, 4)));
}

/// VCN-on smoke run through the searcher (not the protocol): root result of a fixed-node MCTS
/// VCN search next to AB's answer. Informational, except that a proven MCTS win AB does not
/// confirm is a failure. RAPFI_VCN_SMOKE_NODES overrides the node count.
void mctsVcn_smokeVsAb_noMateAbRefutes()
{
    uint64_t nodes = 20000;
    if (const char *env = std::getenv("RAPFI_VCN_SMOKE_NODES"))
        nodes = std::strtoull(env, nullptr, 10);
    struct Case
    {
        const char      *name;
        std::vector<Pos> moves;
        VCNLevel         level;
        Value            ab;
    };
    // Roots without a five or a flex four on the board: the ROOT picker does not mark a policy on
    // those forced roots, which Node::createEdges asserts (pre-existing, VCN off too; not slice 4).
    // So the forced positions of tests/vcn_tests.cpp start after their forced pair.
    std::vector<Case> cases = {
        {"doubleThreeAfterPair", afterForcedPair(doubleThreeAfterForcedBlockMoves()), VCNLevel::VC4},
        {"doubleThreeAfterPair", afterForcedPair(doubleThreeAfterForcedBlockMoves()), VCNLevel::VC3},
        {"farVcfAfterPair", afterForcedPair(farVcfAfterForcedBlockMoves()), VCNLevel::VC4},
        {"farVcfAfterPair", afterForcedPair(farVcfAfterForcedBlockMoves()), VCNLevel::VC3},
        {"quiet", quietMoves(), VCNLevel::VC4},
        {"quiet", quietMoves(), VCNLevel::VC3},
    };
    for (const Case &c : cases) {
        auto b = plainBoard(FREESTYLE, c.moves);
        for (Color side : {BLACK, WHITE})
            assert(!b->p4Count(side, A_FIVE) && !b->p4Count(side, B_FLEX4) && "smoke: forced root");
    }
    for (Case &c : cases)
        c.ab = abVcnValue(c.moves, c.level);

    MctsEngineFixture fx;
    for (const Case &c : cases) {
        Search::Engine.clear(true);
        auto          board = plainBoard(FREESTYLE, c.moves);
        SearchOptions opts  = searchOptions(*board, nodes, c.level);
        Time          t0    = now();
        runMctsSearch(*board, opts);
        Time       elapsed = now() - t0;
        Node      *root    = fx.searcher().graph.root;
        ValueBound bound   = root ? root->getBound() : ValueBound {};
        Value      best    = Search::Engine.main()->rootMoves[0].value;
        Pos        move    = Search::Engine.main()->rootMoves[0].pv[0];
        bool       mctsWin = best >= VALUE_MATE_IN_MAX_PLY || bound.lower >= VALUE_MATE_IN_MAX_PLY;
        std::printf("  smoke %-22s VC%d nodes %llu: AB %6d (%s) | MCTS best (%d,%d) value %6d "
                    "bound [%d, %d] root visits %u time %lld ms\n",
                    c.name,
                    int(c.level) + 1,
                    (unsigned long long)nodes,
                    int(c.ab),
                    className(mateClass(true, c.ab)),
                    move.x(),
                    move.y(),
                    int(best),
                    int(bound.lower),
                    int(bound.upper),
                    root ? unsigned(root->getVisits()) : 0u,
                    (long long)elapsed);
        std::fflush(stdout);
        // A proven MCTS win that AB refutes is a wrong +M.
        if (mctsWin)
            assert(c.ab >= VALUE_MATE_IN_MAX_PLY);
    }
}

// ----------------------------------------------------------------------------------------------
// VCN-03 slice 5: YXVCN through the protocol under the MCTS searcher (stand-in evaluator, one
// thread, fixed node count, so deterministic). Roots have no five or flex four (SEARCH-12).

/// The protocol state of a YXVCN query: `moves` on a fresh board, a fixed node limit.
void resetProtocolForMcts(const std::vector<Pos> &moves, uint64_t maxNodes)
{
    Search::Engine.clear(true);
    Search::Engine.waitForIdle();
    GP::options                     = SearchOptions {};
    GP::options.rule                = {FREESTYLE, GameRule::FREEOPEN};
    GP::options.disableOpeningQuery = true;
    GP::options.maxDepth            = 8;  // ignored by MCTS; AB stops on it
    GP::options.maxNodes            = maxNodes;
    GP::board                       = plainBoard(FREESTYLE, moves);
}

/// Runs `YXVCN <level>` and returns everything it printed.
std::string runYxvcnCaptured(int level)
{
    ScopedCoutCapture cap;
    {
        ScopedCin cin(std::to_string(level));
        GP::searchVCN();
    }
    Search::Engine.waitForIdle();
    return cap.str();
}

std::string movesText(const std::vector<Pos> &pv)
{
    std::ostringstream oss;
    oss << MovesText {pv};
    return oss.str();
}

constexpr uint64_t ProtocolNodes = 20000;

/// The NORMAL message mode for one test (the mode a GUI gets: setGUIMode turns BRIEF into NORMAL).
/// MCTS prints its root-move lines in NORMAL / UCILIKE only; the built-in default is BRIEF.
/// A message mode (BRIEF / NORMAL / UCILIKE) for one test; the previous mode is restored on exit.
struct ScopedMessageMode
{
    MsgMode saved = Config::GeneralCfg.messageMode;
    explicit ScopedMessageMode(MsgMode mode) { Config::GeneralCfg.messageMode = mode; }
    ~ScopedMessageMode() { Config::GeneralCfg.messageMode = saved; }
};

struct ScopedNormalMessages : ScopedMessageMode
{
    ScopedNormalMessages() : ScopedMessageMode(MsgMode::NORMAL) {}
};

void yxvcn_mctsSearcher_runsVcnSearchAndRestoresLevel()
{
    // Arrange: MCTS, persistent level VC2 set through INFO VCTHREAD, quiet position
    MctsEngineFixture fx;
    resetProtocolForMcts(quietMoves(), ProtocolNodes);
    GP::options.vcnMode.n = VCNLevel::VC2;
    assert(Search::Engine.main()->rootMoves.empty());

    // Act: one-shot VC4 query
    std::string out = runYxvcnCaptured(4);

    // Assert: no refusal, a search ran in VCN mode (a quiet VC4 attacker loses: an ordinary
    // search would print a near-zero value), the root is VCN-keyed, the persistent level is back
    assert(out.find("ERROR") == std::string::npos);
    assert(!Search::Engine.main()->rootMoves.empty());
    assert(Search::Engine.main()->rootMoves[0].value <= VALUE_MATED_IN_MAX_PLY);
    auto quiet = plainBoard(FREESTYLE, quietMoves());
    assert(fx.searcher().graph.root->getHash() != quiet->zobristKey());
    assert(GP::options.vcnMode.n == VCNLevel::VC2);
}

/// §7 slice 5 (`mctsVcn_vc2/vc3/vc4_openThreeAttacker_mates`): the open-three root is a forced
/// ROOT-picker branch (a flex-four move exists), which aborts in assert builds (SEARCH-12). So the
/// end-to-end mate is checked on the far-VCF root after the forced pair, at every level; the open
/// three is measured on the release build (slice 5 Part C).
void mctsVcn_vc2Vc3Vc4_farVcfAttacker_mates()
{
    MctsEngineFixture fx;
    for (int level : {2, 3, 4}) {
        // Arrange
        resetProtocolForMcts(afterForcedPair(farVcfAfterForcedBlockMoves()), ProtocolNodes);

        // Act
        std::string out = runYxvcnCaptured(level);

        // Assert
        assert(out.find("ERROR") == std::string::npos);
        assert(Search::Engine.main()->rootMoves[0].value >= VALUE_MATE_IN_MAX_PLY);
        assert(!GP::options.vcnMode.enabled());
    }
}

/// The double three after the forced pair: a VC3 win (pass, then an open four), a VC4 loss
/// (a defender pass refutes it), through the protocol.
void mctsVcn_doubleThreeAfterForcedPair_vc3WinsVc4Loses()
{
    MctsEngineFixture fx;
    resetProtocolForMcts(afterForcedPair(doubleThreeAfterForcedBlockMoves()), ProtocolNodes);
    runYxvcnCaptured(3);
    assert(Search::Engine.main()->rootMoves[0].value >= VALUE_MATE_IN_MAX_PLY);

    resetProtocolForMcts(afterForcedPair(doubleThreeAfterForcedBlockMoves()), ProtocolNodes);
    runYxvcnCaptured(4);
    assert(Search::Engine.main()->rootMoves[0].value <= VALUE_MATED_IN_MAX_PLY);
}

void yxvcn_mctsSearcher_vc4FarVcf_printsMateAndBestmoveNotPass()
{
    // Arrange: a non-forced root (SEARCH-12) with a VC4 win
    ScopedNormalMessages messages;
    MctsEngineFixture    fx;
    const std::vector<Pos> moves = afterForcedPair(farVcfAfterForcedBlockMoves());
    resetProtocolForMcts(moves, ProtocolNodes);
    const auto before = plainBoard(FREESTYLE, moves);

    // Act
    std::string out = runYxvcnCaptured(4);

    // Assert: a "+M" root line; the move sent (and played on the protocol board) is not a pass
    const auto &rm = Search::Engine.main()->rootMoves[0];
    assert(rm.value >= VALUE_MATE_IN_MAX_PLY);
    assert(out.find("+M") != std::string::npos);
    assert(rm.pv[0] != Pos::PASS);
    assert(Search::Engine.ctx.bestMove != Pos::PASS);
    assert(GP::board->ply() == before->ply() + 1);
    assert(GP::board->getLastMove() == rm.pv[0]);
    std::ostringstream passCoord;
    passCoord << CoordText {Pos::PASS, 15, CoordConversionMode::NONE};
    assert(out.find("\n" + passCoord.str() + "\n") == std::string::npos);
}

/// §7 slice 5: a VC4 attacker without a VCF loses, through the protocol.
void mctsVcn_vc4_attackerWithoutVcf_loses()
{
    MctsEngineFixture fx;
    resetProtocolForMcts(quietMoves(), ProtocolNodes);
    runYxvcnCaptured(4);
    assert(Search::Engine.main()->rootMoves[0].value <= VALUE_MATED_IN_MAX_PLY);
}

/// A VC3 attacker whose moves carry no VCF threat loses: the pass test ends every defender leaf
/// (VCN-04). No pass is played in the graph, so no PV holds one.
void mctsVcn_vc3_attackerWithoutThreat_losesByPassTestNoPassInPv()
{
    MctsEngineFixture fx;
    resetProtocolForMcts(quietMoves(), ProtocolNodes);
    runYxvcnCaptured(3);
    const auto &rootMoves = Search::Engine.main()->rootMoves;
    assert(rootMoves[0].value <= VALUE_MATED_IN_MAX_PLY);
    for (const auto &rm : rootMoves)
        assert(std::count(rm.pv.begin(), rm.pv.end(), Pos::PASS) == 0);
}

/// A quiet VC2 root: no pass in any PV and no proven attacker win.
void mctsVcn_vc2_quietRoot_noPassInPvAndNoProvenWin()
{
    MctsEngineFixture fx;
    resetProtocolForMcts(quietMoves(), ProtocolNodes);
    std::string out = runYxvcnCaptured(2);
    const auto &rootMoves = Search::Engine.main()->rootMoves;
    assert(out.find("ERROR") == std::string::npos);
    assert(!rootMoves.empty());
    assert(rootMoves[0].value < VALUE_MATE_IN_MAX_PLY);
    for (const auto &rm : rootMoves)
        assert(std::count(rm.pv.begin(), rm.pv.end(), Pos::PASS) == 0);
}

// ----------------------------------------------------------------------------------------------

// ----------------------------------------------------------------------------------------------
// SEARCH-12: the ROOT MovePicker marks a policy on forced roots (five, flex four), which
// Node::createEdges asserts (hasPolicyScore) for every MCTS root expansion.

/// What a ROOT picker built with a normalised policy yields: the moves in order, the policy of
/// each (read right after the call that returned it), and the picker's two policy flags.
struct RootPick
{
    std::vector<Pos>   moves;
    std::vector<float> policy;
    bool               hasPolicyScore      = false;
    bool               hasNormalizedPolicy = false;
};

RootPick pickRoot(const Board &board, Rule rule)
{
    MovePicker mp(rule, board, MovePicker::ExtraArgs<MovePicker::ROOT> {true, 1.0f});
    RootPick   r;
    while (Pos move = mp()) {
        r.moves.push_back(move);
        r.policy.push_back(mp.curMoveNormalizePolicy());
    }
    r.hasPolicyScore      = mp.hasPolicyScore();
    r.hasNormalizedPolicy = mp.hasNormalizedPolicy();
    return r;
}

/// Black has a closed four on column 7 (white at (7,4)); its only five point is (7,9).
/// White to move: the opponent has a five.
std::vector<Pos> opponentFiveMoves()
{
    return {Pos(7, 5), Pos(7, 4), Pos(7, 6), Pos(0, 0), Pos(7, 7), Pos(0, 2), Pos(7, 8)};
}

/// The same closed four with black to move: the side to move has a five.
std::vector<Pos> ownFiveMoves()
{
    std::vector<Pos> m = opponentFiveMoves();
    m.push_back(Pos(0, 4));
    return m;
}

/// Black has an open three (7,5)-(7,7) and white to move: the opponent has a flex-four threat.
std::vector<Pos> opponentFlexFourMoves()
{
    return {Pos(7, 5), Pos(0, 0), Pos(7, 6), Pos(0, 2), Pos(7, 7)};
}

/// The same open three with black to move: the side to move can make a flex four.
std::vector<Pos> ownFlexFourMoves()
{
    std::vector<Pos> m = opponentFlexFourMoves();
    m.push_back(Pos(0, 4));
    return m;
}

/// Asserts the picker output is one-hot: a non-empty list, first move 1.0, every other 0.0.
void assertOneHot(const RootPick &r)
{
    assert(!r.moves.empty());
    assert(r.policy.size() == r.moves.size());
    assert(r.policy[0] == 1.0f);
    for (size_t i = 1; i < r.policy.size(); i++)
        assert(r.policy[i] == 0.0f);
}

void movePickerRoot_opponentFive_normalizedPolicy_hasPolicyScore()
{
    // Arrange: white to move, black threatens five at (7,9)
    auto board = plainBoard(FREESTYLE, opponentFiveMoves());
    assert(board->sideToMove() == WHITE);
    assert(board->p4Count(BLACK, A_FIVE) && !board->p4Count(WHITE, A_FIVE));

    // Act
    RootPick r = pickRoot(*board, FREESTYLE);

    // Assert: the single block, one-hot, flags set
    assert(r.moves.size() == 1 && r.moves[0] == Pos(7, 9));
    assertOneHot(r);
    assert(r.hasNormalizedPolicy);
    assert(r.hasPolicyScore);
}

void movePickerRoot_ownFive_normalizedPolicy_hasPolicyScore()
{
    // Arrange: black to move with a five available at (7,9)
    auto board = plainBoard(FREESTYLE, ownFiveMoves());
    assert(board->sideToMove() == BLACK);
    assert(board->p4Count(BLACK, A_FIVE));

    // Act
    RootPick r = pickRoot(*board, FREESTYLE);

    // Assert
    assert(r.moves[0] == Pos(7, 9));
    assertOneHot(r);
    assert(r.hasNormalizedPolicy);
    assert(r.hasPolicyScore);
}

void movePickerRoot_ownFlexFour_normalizedPolicy_hasPolicyScore()
{
    // Arrange: black to move with an open three, no five on the board
    auto board = plainBoard(FREESTYLE, ownFlexFourMoves());
    assert(board->sideToMove() == BLACK);
    assert(!board->p4Count(BLACK, A_FIVE) && !board->p4Count(WHITE, A_FIVE));
    assert(board->p4Count(BLACK, B_FLEX4));

    // Act
    RootPick r = pickRoot(*board, FREESTYLE);

    // Assert
    assertOneHot(r);
    assert(r.hasNormalizedPolicy);
    assert(r.hasPolicyScore);
}

void movePickerRoot_opponentFlexFour_normalizedPolicy_hasPolicyScore()
{
    // Arrange: white to move facing an open three; this branch scores through the evaluator
    // (scoreAllMoves<POLICY>), so the board is bound to a thread with a stand-in evaluator.
    MctsEngineFixture fx;
    auto src = plainBoard(FREESTYLE, opponentFlexFourMoves());
    Board board(*src, threadWithEvaluator(FREESTYLE));
    assert(board.sideToMove() == WHITE);
    assert(!board.p4Count(BLACK, A_FIVE) && !board.p4Count(WHITE, A_FIVE));
    assert(!board.p4Count(WHITE, B_FLEX4) && board.p4Count(BLACK, B_FLEX4));

    // Act
    RootPick r = pickRoot(board, FREESTYLE);

    // Assert: pins current behaviour (not changed by SEARCH-12)
    assert(!r.moves.empty());
    assert(r.hasNormalizedPolicy);
    assert(r.hasPolicyScore);
}

void mctsSearch_rootOpponentFive_assertBuild_completesWithBlockMove()
{
    // Arrange: MCTS with the stand-in evaluator, white to move against a closed four
    MctsEngineFixture fx;
    auto board = plainBoard(FREESTYLE, opponentFiveMoves());
    assert(board->p4Count(BLACK, A_FIVE));
    SearchOptions opts = searchOptions(*board, 2000, VCNLevel::None);

    // Act: asserts are live in rapfi_tests, so this aborts in Node::createEdges before the fix
    runMctsSearch(*board, opts);

    // Assert: the search completed and answers the only block
    assert(!Search::Engine.main()->rootMoves.empty());
    assert(Search::Engine.main()->rootMoves[0].pv[0] == Pos(7, 9));
}

// ----------------------------------------------------------------------------------------------
// SEARCH-13: the MCTS singular-root stop must not end a VCN search at a forced root

/// VCN-03 slice 5 Part C position P4 (tests/vcn_measure.sh): white has a four on x = 12, black's
/// only legal move is the block at (12,9), which is not itself a four. Black to move.
std::vector<Pos> forcedBlockWithoutFourMoves()
{
    return alternateMoves({Pos(3, 7), Pos(4, 7), Pos(5, 7), Pos(12, 14), Pos(0, 0)},
                          {Pos(12, 10), Pos(12, 11), Pos(12, 12), Pos(12, 13), Pos(14, 0)});
}

void mctsVcn_vc3_forcedRootMoveWithNodeBudget_searchesPastSingularLimit()
{
    // Arrange: a forced root (one root move), VC3, a fixed budget far above the singular-root
    // stop (numNodesAfterSingularRoot = 100, checked after each playout batch of up to 100 visits).
    MctsEngineFixture fx;
    auto              board = plainBoard(FREESTYLE, forcedBlockWithoutFourMoves());
    assert(board->p4Count(WHITE, A_FIVE));
    const uint64_t maxNodes = 20000;
    SearchOptions  opts     = searchOptions(*board, maxNodes, VCNLevel::VC3);
    assert(opts.vcnMode.enabled());

    // Act
    runMctsSearch(*board, opts);

    // Assert: the root really is forced, and the search was not cut off by the singular-root stop
    assert(Search::Engine.main()->rootMoves.size() == 1);
    uint64_t nodes = Search::Engine.nodesSearched();
    std::printf("  forced-root VC3 nodes searched: %llu\n", (unsigned long long)nodes);
    std::fflush(stdout);
    assert(nodes > uint64_t(SearchCfg.numNodesAfterSingularRoot + SearchCfg.maxNumVisitsPerPlayout));
}

void mctsSearch_vcnOff_singularRoot_stopsAfterNumNodesAfterSingularRoot()
{
    // Arrange: the same forced root with VCN off; the budget is far above the stop
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    auto              board = plainBoard(FREESTYLE, forcedBlockWithoutFourMoves());
    SearchOptions     opts  = searchOptions(*board, 20000, VCNLevel::None);
    assert(!opts.vcnMode.enabled());

    // Act
    runMctsSearch(*board, opts);

    // Assert: forced root, stopped right after the singular-root node count (current behaviour)
    assert(Search::Engine.main()->rootMoves.size() == 1);
    uint64_t nodes = Search::Engine.nodesSearched();
    assert(nodes >= uint64_t(SearchCfg.numNodesAfterSingularRoot));
    assert(nodes <= uint64_t(SearchCfg.numNodesAfterSingularRoot + SearchCfg.maxNumVisitsPerPlayout));
}

// ----------------------------------------------------------------------------------------------
// SEARCH-14: MCTS prints one end-of-search result line in BRIEF (AB's words, no Depth)

/// One fixed-node MCTS search on the quiet position (VCN off); returns everything it printed.
std::string runMctsSearchCaptured(uint64_t maxNodes)
{
    auto          board = plainBoard(FREESTYLE, quietMoves());
    SearchOptions opts  = searchOptions(*board, maxNodes, VCNLevel::None);
    ScopedCoutCapture cap;
    Search::Engine.startThinking(*board, opts);
    Search::Engine.waitForIdle();
    return cap.str();
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream       iss(text);
    for (std::string line; std::getline(iss, line);)
        lines.push_back(line);
    return lines;
}

size_t countLinesStartingWith(const std::vector<std::string> &lines, const std::string &prefix)
{
    return size_t(std::count_if(lines.begin(), lines.end(), [&](const std::string &l) {
        return l.compare(0, prefix.size(), prefix) == 0;
    }));
}

void mctsSearch_briefMode_printsResultLineLikeAb()
{
    // Arrange: BRIEF, the built-in default mode
    ScopedMessageMode messages(MsgMode::BRIEF);
    MctsEngineFixture fx;

    // Act
    std::string out   = runMctsSearchCaptured(2000);
    auto        lines = splitLines(out);

    // Assert: one "Speed … | Eval … | Node … | Time …" line (no Depth: MCTS has no iteration
    // depth) and one "Bestline" line that starts with the best move
    assert(!Search::Engine.main()->rootMoves.empty());
    const auto &rm = Search::Engine.main()->rootMoves[0];
    assert(countLinesStartingWith(lines, "MESSAGE Speed ") == 1);
    for (const auto &l : lines) {
        if (l.compare(0, 14, "MESSAGE Speed ") != 0)
            continue;
        assert(l.find(" | Eval " + std::to_string(int(rm.value)) + " | Node ") != std::string::npos);
        assert(l.find(" | Time ") != std::string::npos);
        assert(l.find("Depth") == std::string::npos);
    }
    assert(countLinesStartingWith(lines, "MESSAGE Bestline ") == 1);
    assert(out.find("MESSAGE Bestline " + movesText({rm.pv[0]})) != std::string::npos);
    assert(out.find("MESSAGE Bestline " + movesText(rm.pv) + "\n") != std::string::npos);
    // the result lines are the last output of the search, after the final root-move update
    assert(lines.size() >= 2 && lines[lines.size() - 2].compare(0, 14, "MESSAGE Speed ") == 0);
}

void mctsSearch_normalMode_outputUnchanged()
{
    // Arrange: NORMAL, the mode a GUI gets
    ScopedMessageMode messages(MsgMode::NORMAL);
    MctsEngineFixture fx;

    // Act
    auto lines = splitLines(runMctsSearchCaptured(2000));

    // Assert: no BRIEF result lines; the root-move lines and the final "Speed … | Visit" stay
    assert(countLinesStartingWith(lines, "MESSAGE Bestline ") == 0);
    for (const auto &l : lines)
        assert(!(l.compare(0, 14, "MESSAGE Speed ") == 0 && l.find(" | Eval ") != std::string::npos));
    assert(countLinesStartingWith(lines, "MESSAGE (1) ") >= 1);
    assert(countLinesStartingWith(lines, "MESSAGE Speed ") >= 1);
    bool hasVisit = false;
    for (const auto &l : lines)
        hasVisit |= l.compare(0, 14, "MESSAGE Speed ") == 0 && l.find(" | Visit ") != std::string::npos;
    assert(hasVisit);
}

void abSearch_briefMode_outputUnchanged()
{
    // Arrange: alpha-beta (the default searcher), BRIEF, a fixed-depth YXVCN query
    ScopedMessageMode messages(MsgMode::BRIEF);
    resetProtocolForMcts(quietMoves(), 0);

    // Act
    auto lines = splitLines(runYxvcnCaptured(3));

    // Assert: one "Speed … | Depth …" line and one "Bestline" line, as before SEARCH-14
    assert(countLinesStartingWith(lines, "MESSAGE Speed ") == 1);
    for (const auto &l : lines)
        if (l.compare(0, 14, "MESSAGE Speed ") == 0) {
            assert(l.find(" | Depth ") != std::string::npos);
            assert(l.find(" | Eval ") != std::string::npos);
        }
    assert(countLinesStartingWith(lines, "MESSAGE Bestline ") == 1);
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
// fork), so they always run in-process. RAPFI_TEST_SKIP (comma-separated substrings of test names)
// skips each test whose name contains one of them.
bool skipped(const char *name)
{
    const char *env = std::getenv("RAPFI_TEST_SKIP");
    if (!env)
        return false;
    std::string list(env);
    for (size_t b = 0; b <= list.size();) {
        size_t      e   = std::min(list.find(',', b), list.size());
        std::string sub = list.substr(b, e - b);
        if (!sub.empty() && std::strstr(name, sub.c_str()) != nullptr)
            return true;
        b = e + 1;
    }
    return false;
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

    // Move generation of a non-root node: no pass edge in any mode (VCN-04)
    RUN_PURE_TEST(config_mctsVcnPassTestNodes_parsedAndClamped);
    RUN_PURE_TEST(config_oldMctsVcnPassPriorKey_ignoredWithoutError);
    RUN_ENGINE_TEST(expandNode_vcnDefender_noPassEdge);
    RUN_ENGINE_TEST(expandNode_vcnDefenderFacingFive_noPassEdge);
    RUN_ENGINE_TEST(expandNode_vcnDefenderFacingOpenThree_noPassEdgeAndFullDefendList);
    RUN_ENGINE_TEST(expandNode_vcnAttacker_noPassEdge);
    RUN_ENGINE_TEST(expandNode_vcnOff_edgesUnchanged);
    RUN_ENGINE_TEST(expandNode_vcnDefenderRenjuBlack_noPassEdgeAndNoForbiddenMove);
    RUN_ENGINE_TEST(widenNode_vcnDefenderCutNode_noPassEdgeAfterWiden);
    RUN_ENGINE_TEST(mctsVcn_graphNeverContainsPassEdge);

    // VCN-03 slice 4: VC4 / VC5 terminals
    RUN_PURE_TEST(quickWinCheckRuntime_vcfOnlyFlag_matchesTemplate);
    RUN_ENGINE_TEST(evaluateNode_vc4AttackerDoubleThreeOnly_attackerLoses);
    RUN_ENGINE_TEST(evaluateNode_vc4AttackerFarVcf_attackerWins);
    RUN_ENGINE_TEST(evaluateNode_vc4AttackerNoVcf_attackerLosesNotNnValue);
    RUN_ENGINE_TEST(evaluateNode_vc4DefenderNoFiveThreat_defenderWins);
    RUN_ENGINE_TEST(evaluateNode_vc4DefenderFacingFive_notTerminalByPassRule);
    RUN_ENGINE_TEST(evaluateNode_vc4AttackerBlocksFiveWithNonFour_attackerLoses);
    RUN_ENGINE_TEST(evaluateNode_vc5AttackerNoFive_attackerLoses);
    RUN_ENGINE_TEST(evaluateNode_vcnBoardFull_returnsDrawLikeAb);
    RUN_ENGINE_TEST(evaluateNode_vc5AttackerNoFiveOnFullBoard_attackerLosesNotDraw);
    RUN_ENGINE_TEST(evaluateNode_vc3_unchangedFromOrdinaryRules);
    RUN_ENGINE_TEST(evaluateNode_vcnOff_unchanged);
    RUN_ENGINE_TEST(simpleVcf_vcfOnlyMode_ttKeySeparatedFromPlain);
    RUN_ENGINE_TEST(evaluateNode_vcnLeaves_matchAbSignAndMateClass);
    RUN_ENGINE_TEST(mctsVcn_smokeVsAb_noMateAbRefutes);

    // VCN-04: the pass test at defender leaves (VC3 exact, VC2 static rule and solver)
    RUN_ENGINE_TEST(evaluateNode_vc3DefenderAttackerHasNoVcfAfterNull_defenderWinsTerminal);
    RUN_ENGINE_TEST(evaluateNode_vc3DefenderAttackerHasVcfAfterNull_notTerminal);
    RUN_ENGINE_TEST(evaluateNode_vc3DefenderFacingFive_noPassTest);
    RUN_ENGINE_TEST(evaluateNode_vc2DefenderHasOwnFlexFour_defenderWinStaysFromQuickWin);
    RUN_ENGINE_TEST(evaluateNode_vc3AttackerToMove_noPassTest);
    RUN_ENGINE_TEST(evaluateNode_vc3PassTestMateBeyondMaxMoves_draw);
    RUN_ENGINE_TEST(evaluateNode_vc3DefenderPassTest_agreesWithAbRootLoss);
    RUN_ENGINE_TEST(evaluateNode_vc2DefenderAttackerHasNoThreeOrFourMove_defenderWinsTerminal);
    RUN_ENGINE_TEST(evaluateNode_vc2DefenderAttackerHasThreeMove_notEndedByStaticRule);
    RUN_ENGINE_TEST(evaluateNode_vc2DefenderSolverProvesNoThreat_defenderWinsTerminal);
    RUN_ENGINE_TEST(evaluateNode_vc2DefenderSolverThreatOrUnknown_notTerminal);
    RUN_ENGINE_TEST(evaluateNode_passTest_boardUnchanged);
    RUN_PURE_TEST(vcnPassTestStatic_noThreeOrFourCell_noThreat);
    RUN_PURE_TEST(vcnPassTestStatic_threeMakingCell_unknown);
    RUN_PURE_TEST(vcnPassTestStatic_renjuBlackAttackerForbidCell_unknown);
    RUN_ENGINE_TEST(vctSolve_openThreeAttacker_threat);
    RUN_ENGINE_TEST(vctSolve_doubleThreeAttacker_threat);
    RUN_ENGINE_TEST(vctSolve_quietPosition_noThreat);
    RUN_ENGINE_TEST(vctSolve_twoInRow_noThreatWithinBudget);
    RUN_ENGINE_TEST(vctSolve_budgetZero_unknown);
    RUN_ENGINE_TEST(vctSolve_budgetExhausted_unknown);
    RUN_ENGINE_TEST(vctSolve_defenderCounterFourRefutes_noThreatOrUnknownNeverThreat);
    RUN_ENGINE_TEST(vctSolve_quietMoveEnablesVcf_isCandidate);
    RUN_ENGINE_TEST(vctAdmissibleMoves_fourAndVcfThreatOnly);
    RUN_ENGINE_TEST(vctSolve_attackerMustBlockFive_onlyBlockConsidered);
    RUN_ENGINE_TEST(vctSolve_provenResultCached_secondCallUsesNoNodes);
    RUN_ENGINE_TEST(vctSolve_unknownNotCachedAsProof_largerBudgetStillProves);
    RUN_ENGINE_TEST(vctSolve_foreignTtEntryUnderCacheKey_ignored);
    RUN_ENGINE_TEST(vctSolve_boardUnchanged);
    RUN_ENGINE_TEST(vctSolve_renjuBlackAttacker_forbiddenMoveNotCandidate);

    // VCN-03 slice 5: YXVCN under MCTS through the protocol (VCN-04: no pass in the graph or PV)
    RUN_ENGINE_TEST(yxvcn_mctsSearcher_runsVcnSearchAndRestoresLevel);
    RUN_ENGINE_TEST(mctsVcn_vc2Vc3Vc4_farVcfAttacker_mates);
    RUN_ENGINE_TEST(mctsVcn_doubleThreeAfterForcedPair_vc3WinsVc4Loses);
    RUN_ENGINE_TEST(yxvcn_mctsSearcher_vc4FarVcf_printsMateAndBestmoveNotPass);
    RUN_ENGINE_TEST(mctsVcn_vc4_attackerWithoutVcf_loses);
    RUN_ENGINE_TEST(mctsVcn_vc3_attackerWithoutThreat_losesByPassTestNoPassInPv);
    RUN_ENGINE_TEST(mctsVcn_vc2_quietRoot_noPassInPvAndNoProvenWin);

    // SEARCH-12: ROOT picker policy flag on forced roots
    RUN_PURE_TEST(movePickerRoot_opponentFive_normalizedPolicy_hasPolicyScore);
    RUN_PURE_TEST(movePickerRoot_ownFive_normalizedPolicy_hasPolicyScore);
    RUN_PURE_TEST(movePickerRoot_ownFlexFour_normalizedPolicy_hasPolicyScore);
    RUN_ENGINE_TEST(movePickerRoot_opponentFlexFour_normalizedPolicy_hasPolicyScore);
    RUN_ENGINE_TEST(mctsSearch_rootOpponentFive_assertBuild_completesWithBlockMove);

    // SEARCH-13: singular-root stop under VCN
    RUN_ENGINE_TEST(mctsVcn_vc3_forcedRootMoveWithNodeBudget_searchesPastSingularLimit);
    RUN_ENGINE_TEST(mctsSearch_vcnOff_singularRoot_stopsAfterNumNodesAfterSingularRoot);

    // SEARCH-14: MCTS result line in BRIEF
    RUN_ENGINE_TEST(mctsSearch_briefMode_printsResultLineLikeAb);
    RUN_ENGINE_TEST(mctsSearch_normalMode_outputUnchanged);
    RUN_ENGINE_TEST(abSearch_briefMode_outputUnchanged);
}
