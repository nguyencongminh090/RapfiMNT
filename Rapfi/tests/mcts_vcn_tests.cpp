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
// Slice 3 (pass edge and prior): the shared move generation of a non-root node is reached through
// Search::MCTS::Testing::generateNodeMoves (search/mcts/searchtesting.h), on a board bound to the
// engine's main thread with a stand-in evaluator.
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
// VCN-03 slice 3: the defender's pass edge and its prior. The move generation of a non-root node
// (expandNode<false> and widenNode share it) is reached through Testing::generateNodeMoves on a
// board bound to the engine's main search thread, which holds a deterministic stand-in evaluator.

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

/// Restores SearchCfg on scope exit.
struct ScopedSearchCfg
{
    SearchConfig saved = SearchCfg;
    ~ScopedSearchCfg() { SearchCfg = saved; }
};

void expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior()
{
    // Arrange: white attacks, black (to move) defends; no five on the board.
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    SearchCfg.mctsVcnPassPrior = 0.25f;
    auto          board        = boundBoard(threadWithEvaluator(FREESTYLE));
    SearchOptions opts         = vcnOptions(VCNLevel::VC3, WHITE);
    assert(board->sideToMove() == BLACK);
    MoveList plain = referenceMoves(*board, FREESTYLE);

    // Act
    MoveList l = nodeMoves(*board, opts);

    // Assert: one pass edge with the configured prior; every board move is kept, scaled by 0.75.
    int pass = l.indexOf(Pos::PASS);
    assert(pass >= 0);
    assert(l.policy[pass] == 0.25f);
    assert(l.moves.size() == plain.moves.size() + 1);
    for (size_t i = 0; i < plain.moves.size(); i++) {
        int j = l.indexOf(plain.moves[i]);
        assert(j >= 0 && j != pass);
        assert(l.policy[j] == plain.policy[i] * (1.0f - 0.25f));
    }
}

void expandNode_vcnDefender_edgesSortedByPolicyDescendingAndSumToOne()
{
    // Arrange: a policy with negative logits, so the picker's own list is not fully sorted.
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    SearchCfg.mctsVcnPassPrior = 0.25f;
    auto          board        = boundBoard(threadWithEvaluator<SignedPolicyEvaluator>(FREESTYLE));
    SearchOptions opts         = vcnOptions(VCNLevel::VC2, WHITE);
    MoveList      plain        = referenceMoves(*board, FREESTYLE);
    assert(!plain.sortedDescending());  // precondition: the unsorted tail exists

    // Act
    MoveList l = nodeMoves(*board, opts);

    // Assert
    assert(l.indexOf(Pos::PASS) >= 0);
    assert(l.sortedDescending());
    assert(std::abs(l.sum() - 1.0f) < 1e-3f);
    // The pass is not ahead of a board move with a higher policy, nor behind one with a lower.
    int pass = l.indexOf(Pos::PASS);
    for (int i = 0; i < int(l.moves.size()); i++) {
        if (i < pass)
            assert(l.policy[i] >= 0.25f);
        if (i > pass)
            assert(l.policy[i] <= 0.25f);
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
    assert(l.indexOf(Pos::PASS) < 0);
    assert(l.moves == plain.moves);
    assert(l.policy == plain.policy);
}

void expandNode_vcnDefenderFacingOpenThree_passEdgeAndSortedDefendList()
{
    // Arrange: black (attacker) has an open three, so white (defender, to move) gets the picker's
    // DEFENDFOUR stage; the negative-logit policy exercises the unsorted-tail case there.
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    SearchCfg.mctsVcnPassPrior = 0.25f;
    SearchThread *th  = threadWithEvaluator<SignedPolicyEvaluator>(FREESTYLE);
    auto          src = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    src->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 6), Pos(0, 0), Pos(7, 7), Pos(0, 3), Pos(7, 8)})
        src->move(FREESTYLE, p);
    Board board(*src, th);
    assert(board.sideToMove() == WHITE);
    assert(!board.p4Count(BLACK, A_FIVE));
    assert(board.p4Count(BLACK, B_FLEX4));
    SearchOptions opts  = vcnOptions(VCNLevel::VC3, BLACK);
    MoveList      plain = referenceMoves(board, FREESTYLE);

    // Act
    MoveList l = nodeMoves(board, opts);

    // Assert: the pass is offered against an open three; the list is sorted and sums to 1.
    assert(l.indexOf(Pos::PASS) >= 0);
    assert(l.moves.size() == plain.moves.size() + 1);
    assert(l.sortedDescending());
    assert(std::abs(l.sum() - 1.0f) < 1e-3f);
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
    assert(l.indexOf(Pos::PASS) < 0);
    assert(l.moves == plain.moves);
    assert(l.policy == plain.policy);
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
        assert(!l.moves.empty());
        assert(l.moves == plain.moves);
        assert(l.policy == plain.policy);
    }
}

void expandNode_vcnDefenderPassPriorZero_passLastOrAbsentAndBoardPoliciesUnscaled()
{
    // Arrange: prior 0 keeps the pass (the move set stays the AB one) but places it last.
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    SearchCfg.mctsVcnPassPrior = 0.0f;
    auto          board        = boundBoard(threadWithEvaluator(FREESTYLE));
    SearchOptions opts         = vcnOptions(VCNLevel::VC3, WHITE);
    MoveList      plain        = referenceMoves(*board, FREESTYLE);

    // Act
    MoveList l = nodeMoves(*board, opts);

    // Assert: pass last with policy 0, board policies bit-equal to the ordinary ones.
    assert(l.moves.size() == plain.moves.size() + 1);
    assert(l.moves.back() == Pos::PASS);
    assert(l.policy.back() == 0.0f);
    for (size_t i = 0; i < plain.moves.size(); i++) {
        int j = l.indexOf(plain.moves[i]);
        assert(j >= 0);
        assert(l.policy[j] == plain.policy[i]);
    }
    assert(l.sortedDescending());
}

void expandNode_vcnDefenderRenjuBlack_passEdgeAndNoForbiddenMove()
{
    // Arrange: Renju, black (to move) defends against white. Black has a forbidden 3x3 point.
    MctsEngineFixture fx;
    SearchThread     *th  = threadWithEvaluator(RENJU);
    auto              src = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    src->newGame(RENJU);
    // Black (7,5) (7,6) and (5,7) (6,7): (7,7) makes a double three for black.
    for (Pos p : {Pos(7, 5), Pos(0, 0), Pos(7, 6), Pos(0, 3), Pos(5, 7), Pos(0, 6), Pos(6, 7),
                  Pos(0, 9)})
        src->move(RENJU, p);
    Board board(*src, th);
    assert(board.sideToMove() == BLACK);
    assert(board.checkForbiddenPoint(Pos(7, 7)));
    SearchOptions opts;
    opts.rule             = {RENJU, GameRule::FREEOPEN};
    opts.vcnMode.n        = VCNLevel::VC3;
    opts.vcnMode.attacker = WHITE;

    // Act
    MoveList l = nodeMoves(board, opts);

    // Assert
    assert(l.indexOf(Pos::PASS) >= 0);
    assert(l.indexOf(Pos(7, 7)) < 0);
    assert(l.sortedDescending());
    assert(std::abs(l.sum() - 1.0f) < 1e-2f);  // the forbidden point keeps a share of softmax
}

void widenNode_vcnDefenderCutNodeWithPassEdge_widensAndKeepsPass()
{
    // Arrange: a cut defender node from the shared generation, the pass among the kept edges.
    MctsEngineFixture fx;
    ScopedSearchCfg   cfg;
    SearchCfg.mctsVcnPassPrior = 0.25f;
    auto             board     = boundBoard(threadWithEvaluator(FREESTYLE));
    SearchOptions    opts      = vcnOptions(VCNLevel::VC3, WHITE);
    MoveList         gen       = nodeMoves(*board, opts);
    const EdgeLimits limits {0.5f, 2};
    Node             parent(HashKey(100), 0);
    parent.setNonTerminal(0.0f, 0.1f);
    parent.createEdgesFrom(gen.moves.data(), gen.policy.data(), uint32_t(gen.moves.size()), limits,
                           nullptr);
    EdgeArray *old = parent.getEdges();
    assert(old && old->edgesCut());
    bool passKept = false;
    for (uint32_t i = 0; i < old->numEdges; i++)
        passKept |= (*old)[i].getMove() == Pos::PASS;
    assert(passKept);

    // Every kept reply (the pass too) is a proven win for the attacker.
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

    // Assert: widened, the pass kept, and the node proves lost once the cut replies lose too.
    assert(ok);
    EdgeArray *wide = parent.getEdges();
    assert(!wide->edgesCut() && wide->numEdges == full.moves.size());
    passKept = false;
    for (uint32_t i = 0; i < numKept; i++)
        passKept |= (*wide)[i].getMove() == Pos::PASS && (*wide)[i].getVisits() == 1;
    assert(passKept);
    attach(*wide, numKept, mate_in(5));
    assert(!parent.updateStats());
    assert(parent.getBound().isTerminal());
    assert(parent.getBound().upper == mated_in(5));
}

void config_mctsVcnPassPrior_parsedAndClamped()
{
    // Arrange
    ScopedSearchCfg cfg;
    assert(SearchConfig {}.mctsVcnPassPrior == 0.25f);
    auto load = [](const char *value) {
        std::istringstream in(std::string("[search]\nmcts_vcn_pass_prior = ") + value + "\n");
        ScopedCoutCapture  cap;
        bool               ok = Config::loadConfig(in);
        assert(ok);
        return SearchCfg.mctsVcnPassPrior;
    };

    // Act / Assert
    assert(load("0.4") == 0.4f);
    assert(load("0.0") == 0.0f);
    assert(load("-0.3") == 0.0f);
    float one = load("1.0");
    assert(one < 1.0f && one > 0.999f);
    float big = load("7.5");
    assert(big < 1.0f && big > 0.999f);
    assert(load("nan") == 0.0f);
}

void searchNode_vcnDefenderPass_childKeyHasRaisedLevel()
{
    // Arrange: black attacks from VC3 (side to move at the root).
    MctsEngineFixture fx;
    auto              board = makeBoard();
    SearchOptions     vcn   = searchOptions(*board, 2000, VCNLevel::VC3);
    assert(vcn.vcnMode.attacker == BLACK);

    // Act
    runMctsSearch(*board, vcn);

    // Assert: every PASS child found in the graph has the VC4 key of its board.
    const int                rootPasses = fx.searcher().scratch.rootDefenderPasses;
    int                      passChildren = 0;
    std::unordered_set<Node *> visited;
    auto                       replay = makeBoard();
    std::function<void(Node &)> walk  = [&](Node &node) {
        if (!visited.insert(&node).second)
            return;
        EdgeArray *edges = node.getEdges();
        if (!edges)
            return;
        for (uint32_t i = 0; i < edges->numEdges; i++) {
            Node *child = (*edges)[i].getChild();
            if (!child)
                continue;
            Pos move = (*edges)[i].getMove();
            replay->move(FREESTYLE, move);
            if (move == Pos::PASS) {
                passChildren++;
                assert(mctsNodeVcnLevel(*replay, vcn, rootPasses) > VCNLevel::VC3);
                assert(child->getHash() == mctsNodeKey(*replay, vcn, rootPasses));
                if (replay->passMoveCountOfSide(WHITE) == 1)
                    assert(child->getHash()
                           == (replay->zobristKey() ^ Vcn::hashXor(vcn, VCNLevel::VC4)));
            }
            walk(*child);
            replay->undo(FREESTYLE);
        }
    };
    walk(*fx.searcher().graph.root);
    assert(passChildren > 0);
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

    // VCN-03 slice 3: pass edge and prior
    RUN_PURE_TEST(config_mctsVcnPassPrior_parsedAndClamped);
    RUN_ENGINE_TEST(expandNode_vcnDefender_hasPassEdgeWithConfiguredPrior);
    RUN_ENGINE_TEST(expandNode_vcnDefender_edgesSortedByPolicyDescendingAndSumToOne);
    RUN_ENGINE_TEST(expandNode_vcnDefenderFacingFive_noPassEdge);
    RUN_ENGINE_TEST(expandNode_vcnDefenderFacingOpenThree_passEdgeAndSortedDefendList);
    RUN_ENGINE_TEST(expandNode_vcnAttacker_noPassEdge);
    RUN_ENGINE_TEST(expandNode_vcnOff_edgesUnchanged);
    RUN_ENGINE_TEST(expandNode_vcnDefenderPassPriorZero_passLastOrAbsentAndBoardPoliciesUnscaled);
    RUN_ENGINE_TEST(expandNode_vcnDefenderRenjuBlack_passEdgeAndNoForbiddenMove);
    RUN_ENGINE_TEST(widenNode_vcnDefenderCutNodeWithPassEdge_widensAndKeepsPass);
    RUN_ENGINE_TEST(searchNode_vcnDefenderPass_childKeyHasRaisedLevel);

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
}
