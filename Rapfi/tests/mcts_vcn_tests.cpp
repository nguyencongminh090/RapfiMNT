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
#include "../config.h"
#include "../search/mcts/parameter.h"
#include "../search/mcts/searchtesting.h"
#include "../search/movepick.h"
#include "../search/searchconfig.h"
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
}
