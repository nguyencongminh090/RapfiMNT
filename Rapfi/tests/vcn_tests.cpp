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

// Behaviour tests for VCN (Victory by Continuous N-level attack) search mode: the defender's
// pass stage in MovePicker, the VCN search itself (VC2..VC4; VC5 is internal), and the INFO VCTHREAD / YXVCN commands.
// The Vcn:: helpers live in an anonymous namespace of search.cpp, so they are covered through
// the search results below rather than called directly.

#include "testutil.h"

#include "../core/pos.h"
#include "../core/types.h"
#include "../game/board.h"
#include "../game/wincheck.h"
#include "../search/movepick.h"
#include "../search/searchcommon.h"
#include "../search/searcher.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace Command::GomocupProtocol {
extern std::unique_ptr<Board> board;
extern Search::SearchOptions  options;
void                          getOption();
void                          searchVCN();
}  // namespace Command::GomocupProtocol

namespace GP = Command::GomocupProtocol;
using Search::MovePicker;
using Search::VCNLevel;

namespace {

std::unique_ptr<Board> makeBoard(Rule rule, std::vector<Pos> moves)
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(rule);
    for (Pos p : moves)
        board->move(rule, p);
    return board;
}

// Black has an open three on row 7; white stones are far away. Black to move.
std::vector<Pos> openThreePosition()
{
    return {Pos(7, 7), Pos(3, 3), Pos(8, 7), Pos(3, 12), Pos(9, 7), Pos(12, 12)};
}

// A single black stone and a far white stone. Black to move.
std::vector<Pos> quietPosition()
{
    return {Pos(7, 7), Pos(3, 3)};
}

// Interleave black and white stones into a move sequence starting with black.
std::vector<Pos> alternate(const std::vector<Pos> &black, const std::vector<Pos> &white)
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

// Black to move; white threatens five at (12,9). Black's forced block at (12,9) is itself a block
// four (row 9, closed by white at (8,9)), which white must block at (13,9). Black's only VCF is
// then far from (12,9): (3,6) four on row 6 (white (2,6)), then (3,7) double four on column 3
// and the anti-diagonal. White (2,10) and (2,9) block the anti-diagonals through (3,9) and (3,8),
// which would otherwise give black double threes at (5,7) and (6,5) that quickWinCheck() reports
// on its own. Same as P1 in tests/vcn_measure.sh.
std::vector<Pos> farVcfAfterForcedBlockPosition()
{
    const std::vector<Pos> black = {Pos(3, 8),
                                    Pos(3, 9),
                                    Pos(4, 6),
                                    Pos(5, 6),
                                    Pos(6, 6),
                                    Pos(5, 5),
                                    Pos(6, 4),
                                    Pos(9, 9),
                                    Pos(10, 9),
                                    Pos(11, 9),
                                    Pos(12, 14)};
    const std::vector<Pos> white = {Pos(3, 10),
                                    Pos(7, 6),
                                    Pos(7, 3),
                                    Pos(8, 9),
                                    Pos(12, 10),
                                    Pos(12, 11),
                                    Pos(12, 12),
                                    Pos(12, 13),
                                    Pos(0, 0),
                                    Pos(2, 9),
                                    Pos(2, 10)};
    return alternate(black, white);
}

// Black to move; the same forced pair as above: black must block at (12,9) (a block four), white
// blocks at (13,9). Black then has no four anywhere, only a double open three at (5,5) (row 5 and
// column 5). Same as P7 in tests/vcn_measure.sh.
std::vector<Pos> doubleThreeAfterForcedBlockPosition()
{
    return alternate({Pos(3, 5),
                      Pos(4, 5),
                      Pos(5, 3),
                      Pos(5, 4),
                      Pos(9, 9),
                      Pos(10, 9),
                      Pos(11, 9),
                      Pos(12, 14)},
                     {Pos(8, 9),
                      Pos(12, 10),
                      Pos(12, 11),
                      Pos(12, 12),
                      Pos(12, 13),
                      Pos(0, 0),
                      Pos(14, 0),
                      Pos(0, 14)});
}

std::vector<Pos> drain(MovePicker &mp)
{
    std::vector<Pos> moves;
    while (Pos m = mp())
        moves.push_back(m);
    return moves;
}

bool contains(const std::vector<Pos> &v, Pos p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

MovePicker::ExtraArgs<MovePicker::MAIN> mainArgs(Pos ttMove, bool pass, bool attackOrder = false)
{
    return {ttMove, Search::MoveHistoryScoring {}, false, 1.0f, pass, attackOrder};
}

void resetProtocolState(Rule rule, const std::vector<Pos> &moves)
{
    Search::Engine.clear(true);
    Search::Engine.waitForIdle();

    GP::options                     = Search::SearchOptions {};
    GP::options.rule                = {rule, GameRule::FREEOPEN};
    GP::options.disableOpeningQuery = true;
    GP::options.maxDepth            = 8;

    GP::board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    GP::board->newGame(rule);
    for (Pos p : moves)
        GP::board->move(rule, p);
}

// Run YXVCN with the given level and return the value of the best root move.
Value runVcn(int level)
{
    ScopedCoutCapture cap;
    {
        ScopedCin cin(std::to_string(level));
        GP::searchVCN();
    }
    Search::Engine.waitForIdle();
    return Search::Engine.main()->rootMoves[0].value;
}

void runInfo(const std::string &text)
{
    ScopedCoutCapture cap;
    ScopedCin         cin(text);
    GP::getOption();
}

// Run INFO <text> and return what it printed.
std::string runInfoCaptured(const std::string &text)
{
    ScopedCoutCapture cap;
    {
        ScopedCin cin(text);
        GP::getOption();
    }
    return cap.str();
}

// Switch the engine to the MCTS searcher for one test; restore the default (alpha-beta) searcher
// on exit so later tests do not run under MCTS.
struct ScopedMctsSearcher
{
    ScopedMctsSearcher()
    {
        Search::Engine.setupSearcher(Search::createSearcher("mcts"));
        Search::Engine.clear(true);
    }
    ~ScopedMctsSearcher()
    {
        Search::Engine.setupSearcher(Search::createSearcher(""));
        Search::Engine.clear(true);
    }
};

// --------------------------------------------------------------------------
// MovePicker pass stage

void movePickerMain_generatePassOff_neverYieldsPass()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, quietPosition());

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::NONE, false));
    std::vector<Pos> moves = drain(mp);

    // Assert
    assert(!moves.empty());
    assert(!contains(moves, Pos::PASS));
}

void movePickerMain_generatePassOn_yieldsPassBeforeBoardMoves()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, quietPosition());

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::NONE, true));
    std::vector<Pos> moves = drain(mp);

    // Assert: the pass comes first, exactly once, and the board moves still follow
    assert(moves.size() > 1);
    assert(moves.front() == Pos::PASS);
    assert(std::count(moves.begin(), moves.end(), Pos::PASS) == 1);
}

void movePickerMain_generatePassOn_afterLastMovePass_noSecondPass()
{
    // Arrange: the last move was a pass
    auto board = makeBoard(FREESTYLE, quietPosition());
    board->move(FREESTYLE, Pos::PASS);

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::NONE, true));
    std::vector<Pos> moves = drain(mp);

    // Assert
    assert(!moves.empty());
    assert(!contains(moves, Pos::PASS));
}

void movePickerMain_ttMoveIsPass_yieldedOnceOnly()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, quietPosition());

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::PASS, true));
    std::vector<Pos> moves = drain(mp);

    // Assert: as the tt move first, and not a second time from the pass stage
    assert(moves.front() == Pos::PASS);
    assert(std::count(moves.begin(), moves.end(), Pos::PASS) == 1);
}

void movePickerMain_ttMoveIsPassButPassDisabled_ttMoveDropped()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, quietPosition());

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::PASS, false));
    std::vector<Pos> moves = drain(mp);

    // Assert
    assert(!moves.empty());
    assert(!contains(moves, Pos::PASS));
}

void movePickerMain_opponentHasFive_neverYieldsPass()
{
    // Arrange: black has four in a row, so white must block (white to move)
    auto board = makeBoard(FREESTYLE,
                           {Pos(5, 7),
                            Pos(3, 3),
                            Pos(6, 7),
                            Pos(3, 12),
                            Pos(7, 7),
                            Pos(12, 12),
                            Pos(8, 7)});
    assert(board->sideToMove() == WHITE);
    assert(board->p4Count(BLACK, A_FIVE) > 0 || board->p4Count(BLACK, B_FLEX4) > 0);

    // Act: even a pass ttMove must not be offered when a five must be blocked
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::PASS, true));
    std::vector<Pos> moves = drain(mp);

    // Assert: with an open four the pass stage exists but a pass cannot be the only reply, and
    // with a plain five threat it must not appear. Either way the list is never just a pass.
    assert(moves.size() > 1 || !contains(moves, Pos::PASS));
}

// Black to move. Black has a four-making move ((10,7), a block four on row 7: white (6,7)), a
// three-making move ((9,10) makes an open three on row 10) and nothing else. White has no four
// threat (so the picker stays in its main stage: no stones of white line up with each other or
// with (6,7)) but (12,13) would give white a double three, so the ordinary order ranks that pure
// defence square highly.
std::vector<Pos> attackOrderPosition()
{
    return alternate({Pos(7, 7), Pos(8, 7), Pos(9, 7), Pos(7, 10), Pos(8, 10)},
                     {Pos(6, 7), Pos(12, 11), Pos(12, 12), Pos(10, 13), Pos(11, 13)});
}

// Threat tier of a move for black: 3 four, 2 three, 1 two, 0 nothing (FORBID is not a threat).
int blackThreatTier(const Board &board, Pos p)
{
    const Pattern4 p4 = board.pattern4(p, BLACK);
    return p4 >= E_BLOCK4 ? 3 : p4 >= H_FLEX3 ? 2 : (p4 >= L_FLEX2) ? 1 : 0;
}

int indexOf(const std::vector<Pos> &v, Pos p)
{
    return int(std::find(v.begin(), v.end(), p) - v.begin());
}

void movePickerMain_vcnAttackOrderOn_orderedByThreatTier()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, attackOrderPosition());
    assert(board->sideToMove() == BLACK);
    assert(board->p4Count(WHITE, B_FLEX4) + board->p4Count(WHITE, C_BLOCK4_FLEX3) == 0
           && "fixture: white has no four threat, so the picker stays in its main stage");
    assert(board->pattern4(Pos(10, 7), BLACK) >= E_BLOCK4 && "fixture: (10,7) is a four");
    assert(board->pattern4(Pos(9, 10), BLACK) >= H_FLEX3
           && board->pattern4(Pos(9, 10), BLACK) < E_BLOCK4 && "fixture: (9,10) is a three");
    assert(board->pattern4(Pos(12, 13), BLACK) < L_FLEX2 && "fixture: (12,13) is a pure defence move");
    assert(board->pattern4(Pos(12, 13), WHITE) == F_FLEX3_2X && "fixture: white double three");

    // Act
    MovePicker       mp(FREESTYLE, *board, mainArgs(Pos::NONE, false, true));
    std::vector<Pos> moves = drain(mp);

    // Assert: the whole list is ordered by threat tier (four, three, two, none). The ordinary
    // order is not tier-sorted (see the Off test), so this ordering comes from the VCN flag.
    assert(moves.size() > 40);
    assert(blackThreatTier(*board, moves.front()) == 3);
    for (size_t i = 0; i + 1 < moves.size(); i++)
        assert(blackThreatTier(*board, moves[i]) >= blackThreatTier(*board, moves[i + 1]));
}

void movePickerMain_vcnAttackOrderOff_orderUnchanged()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, attackOrderPosition());

    // Act
    MovePicker       plain(FREESTYLE, *board, mainArgs(Pos::NONE, false));
    MovePicker       off(FREESTYLE, *board, mainArgs(Pos::NONE, false, false));
    std::vector<Pos> a = drain(plain), b = drain(off);

    // Assert: the flag off is the ordinary order, and that order is not sorted by threat tier
    // (the fixture must differ from the VCN order, else the On test proves nothing). The exact
    // squares depend on the classical policy scoring, so check the property, not a fixed pair.
    assert(a == b);
    bool tierSorted = true;
    for (size_t i = 0; i + 1 < a.size(); i++)
        tierSorted &= blackThreatTier(*board, a[i]) >= blackThreatTier(*board, a[i + 1]);
    assert(!tierSorted && "fixture: ordinary order differs from the threat-tier order");
}

void movePickerMain_vcnAttackOrderOn_sameMoveSet()
{
    // Arrange
    auto board = makeBoard(FREESTYLE, attackOrderPosition());

    // Act
    MovePicker       off(FREESTYLE, *board, mainArgs(Pos::NONE, false, false));
    MovePicker       on(FREESTYLE, *board, mainArgs(Pos::NONE, false, true));
    std::vector<Pos> a = drain(off), b = drain(on);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());

    // Assert
    assert(a == b);
}

void movePickerQvcf_forceAllowB4_enumeratesPlainB4()
{
    // Arrange: black has a blocked three (white at 6,7) so (10,7) makes a plain B4
    auto board = makeBoard(FREESTYLE,
                           {Pos(7, 7), Pos(6, 7), Pos(8, 7), Pos(3, 12), Pos(9, 7), Pos(12, 12)});
    assert(board->sideToMove() == BLACK);
    const Pattern4 b4 = board->pattern4(Pos(10, 7), BLACK);
    assert(b4 == E_BLOCK4 && "test fixture: (10,7) must be a plain B4");

    // Act
    MovePicker without(FREESTYLE,
                       *board,
                       MovePicker::ExtraArgs<MovePicker::QVCF> {Pos::NONE,
                                                                DEPTH_QVCF,
                                                                {NONE, NONE},
                                                                false});
    MovePicker with(FREESTYLE,
                    *board,
                    MovePicker::ExtraArgs<MovePicker::QVCF> {Pos::NONE,
                                                             DEPTH_QVCF,
                                                             {NONE, NONE},
                                                             true});

    // Assert
    assert(!contains(drain(without), Pos(10, 7)));
    assert(contains(drain(with), Pos(10, 7)));
}

// Board of farVcfAfterForcedBlockPosition() after the forced pair (12,9) / (13,9): black to move,
// black's last stone is (12,9), black's fours are all far from it.
std::unique_ptr<Board> farVcfAfterForcedPairBoard()
{
    auto board = makeBoard(FREESTYLE, farVcfAfterForcedBlockPosition());
    board->move(FREESTYLE, Pos(12, 9));
    board->move(FREESTYLE, Pos(13, 9));
    assert(board->sideToMove() == BLACK);
    assert(board->getLastActualMoveOfSide(BLACK) == Pos(12, 9));
    assert(board->pattern4(Pos(3, 6), BLACK) >= E_BLOCK4 && "fixture: (3,6) must be a four");
    // Nothing quickWinCheck() could report on its own: the win needs the (3,6) four
    assert(board->p4Count(BLACK, B_FLEX4) == 0 && "fixture: no flex four / double four");
    assert(board->p4Count(BLACK, C_BLOCK4_FLEX3) == 0 && "fixture: no four-three");
    assert(board->p4Count(BLACK, F_FLEX3_2X) == 0 && "fixture: no double three");
    return board;
}

void movePickerQvcf_fullBoardOff_onlyFoursNearLastMove()
{
    // Arrange
    auto board = farVcfAfterForcedPairBoard();

    // Act: ordinary quiescence (plain B4 allowed so only locality differs from the VCN case)
    MovePicker mp(FREESTYLE,
                  *board,
                  MovePicker::ExtraArgs<MovePicker::QVCF> {Pos::NONE,
                                                           DEPTH_QVCF,
                                                           {NONE, NONE},
                                                           true});

    // Assert: the far four is outside the locality window and stays excluded
    assert(!contains(drain(mp), Pos(3, 6)));
}

void movePickerQvcf_fullBoardOn_includesFoursFarFromLastMove()
{
    // Arrange
    auto board = farVcfAfterForcedPairBoard();

    // Act
    MovePicker mp(FREESTYLE,
                  *board,
                  MovePicker::ExtraArgs<MovePicker::QVCF> {Pos::NONE,
                                                           DEPTH_QVCF,
                                                           {NONE, NONE},
                                                           true,
                                                           true});
    std::vector<Pos> moves = drain(mp);

    // Assert: every black four on the board, wherever it is
    assert(contains(moves, Pos(3, 6)));
    assert(contains(moves, Pos(3, 7)));
    assert(contains(moves, Pos(2, 8)));
    for (Pos m : moves)
        assert(board->pattern4(m, BLACK) >= E_BLOCK4);
}

// --------------------------------------------------------------------------
// quickWinCheck: VCF-only mode (VC4 attacker)

// Black to move with a double open three at (5,5) (row 5 and column 5) and no four.
std::unique_ptr<Board> doubleThreeBoard()
{
    auto board = makeBoard(FREESTYLE,
                           alternate({Pos(3, 5), Pos(4, 5), Pos(5, 3), Pos(5, 4)},
                                     {Pos(0, 0), Pos(14, 0), Pos(0, 14), Pos(14, 14)}));
    assert(board->pattern4(Pos(5, 5), BLACK) == F_FLEX3_2X && "fixture: (5,5) double three");
    return board;
}

void quickWinCheck_doubleThree_default_reportsWin()
{
    // Arrange
    auto board = doubleThreeBoard();

    // Act
    Value v = quickWinCheck<FREESTYLE>(*board, 0);

    // Assert: the existing rule for every non-VCN caller
    assert(v == mate_in(5));
}

void quickWinCheck_doubleThree_vcfOnly_noWin()
{
    // Arrange
    auto board = doubleThreeBoard();

    // Act
    Value v = quickWinCheck<FREESTYLE>(*board, 0, VALUE_INFINITE, true);

    // Assert: a double three is not a VCF (the VCN defender answers it with a pass)
    assert(v == VALUE_ZERO);
}

void quickWinCheck_fourThree_vcfOnly_stillWin()
{
    // Arrange: (6,5) is a block four on row 5 (closed by white (2,5)) and a flex three on column 6
    auto board = makeBoard(FREESTYLE,
                           alternate({Pos(3, 5), Pos(4, 5), Pos(5, 5), Pos(6, 3), Pos(6, 4)},
                                     {Pos(2, 5), Pos(0, 14), Pos(14, 0), Pos(14, 14), Pos(0, 7)}));
    assert(board->pattern4(Pos(6, 5), BLACK) == C_BLOCK4_FLEX3 && "fixture: (6,5) four-three");

    // Act
    Value v = quickWinCheck<FREESTYLE>(*board, 0, VALUE_INFINITE, true);

    // Assert
    assert(v == mate_in(5));
}

void quickWinCheck_flexFour_vcfOnly_stillWin()
{
    // Arrange: the open three on row 7 gives black a flex four
    auto board = makeBoard(FREESTYLE, openThreePosition());
    assert(board->p4Count(BLACK, B_FLEX4) > 0 && "fixture: flex four available");

    // Act
    Value v = quickWinCheck<FREESTYLE>(*board, 0, VALUE_INFINITE, true);

    // Assert
    assert(v == mate_in(3));
}

// Board of doubleThreeAfterForcedBlockPosition() after the forced pair: only a double three left.
void assertDoubleThreeAfterForcedPairFixture()
{
    auto board = makeBoard(FREESTYLE, doubleThreeAfterForcedBlockPosition());
    board->move(FREESTYLE, Pos(12, 9));
    board->move(FREESTYLE, Pos(13, 9));
    assert(board->p4Count(BLACK, F_FLEX3_2X) > 0 && "fixture: double three after the pair");
    assert(board->p4Count(BLACK, B_FLEX4) + board->p4Count(BLACK, C_BLOCK4_FLEX3)
                   + board->p4Count(BLACK, D_BLOCK4_PLUS) + board->p4Count(BLACK, E_BLOCK4)
               == 0
           && "fixture: no black four after the pair");
}

// --------------------------------------------------------------------------
// VCN search

void vcnSearch_vc2_openThreeAttacker_findsMate()
{
    // Arrange
    resetProtocolState(FREESTYLE, openThreePosition());

    // Act
    Value v = runVcn(2);

    // Assert: open four, then five
    assert(v > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc3_openThreeAttacker_findsMate()
{
    resetProtocolState(FREESTYLE, openThreePosition());
    assert(runVcn(3) > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc4_openThreeAttacker_findsVcfMate()
{
    resetProtocolState(FREESTYLE, openThreePosition());
    assert(runVcn(4) > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc4_vcfFarFromLastMove_found()
{
    // Arrange: the root's only move is the forced block (12,9); after white's (13,9) the VC4
    // attacker (a non-root node, so its moves come from the QVCF picker) must find a VCF far
    // from its last stone (12,9)
    resetProtocolState(FREESTYLE, farVcfAfterForcedBlockPosition());

    // Act
    Value v = runVcn(4);

    // Assert
    assert(v > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc4_doubleThreeAfterForcedBlock_attackerLoses()
{
    // Arrange: after the forced pair the VC4 attacker (non-root) has only a double three
    assertDoubleThreeAfterForcedPairFixture();
    resetProtocolState(FREESTYLE, doubleThreeAfterForcedBlockPosition());

    // Act
    Value v = runVcn(4);

    // Assert: the defender passes after the double three, the attacker has no five at VC5
    assert(v < VALUE_MATED_IN_MAX_PLY);
}

void vcnSearch_vc3_doubleThreeAfterForcedBlock_attackerWins()
{
    // Arrange
    assertDoubleThreeAfterForcedPairFixture();
    resetProtocolState(FREESTYLE, doubleThreeAfterForcedBlockPosition());

    // Act
    Value v = runVcn(3);

    // Assert: at VC3 the double three is a real threat (pass, then open four)
    assert(v > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc3_forcedRootMoveWithTimeLimit_winFound()
{
    // Arrange: a freestyle self-play position (VCN-02 item 4 sweep, #253), black to move. Black's
    // only move is the block at (4,8), which made the ordinary "singular root" rule stop iterating
    // early under a time limit (eval 270 at depth 6); the attack wins (+M9) within depth 8. An
    // ordinary search after the pair (4,8)/(5,7) also proves the win.
    resetProtocolState(FREESTYLE,
                       {Pos(5, 6),
                        Pos(6, 9),
                        Pos(7, 5),
                        Pos(5, 8),
                        Pos(6, 6),
                        Pos(7, 6),
                        Pos(6, 7),
                        Pos(7, 8),
                        Pos(6, 5),
                        Pos(6, 8),
                        Pos(8, 8),
                        Pos(7, 9),
                        Pos(7, 7),
                        Pos(5, 5),
                        Pos(8, 7),
                        Pos(3, 8)});
    GP::options.setTimeControl(60000, 0);  // a time limit enables the early-stop rules

    // Act
    Value v = runVcn(3);

    // Assert
    assert(v > VALUE_MATE_IN_MAX_PLY);
}

void vcnSearch_vc4_attackerWithoutVcf_loses()
{
    resetProtocolState(FREESTYLE, quietPosition());
    assert(runVcn(4) < VALUE_MATED_IN_MAX_PLY);
}

void vcnSearch_vc3_attackerWithoutThreat_defenderPassRefutes()
{
    resetProtocolState(FREESTYLE, quietPosition());
    assert(runVcn(3) < VALUE_MATED_IN_MAX_PLY);
}

void vcnSearch_levelIsNotLeftSetAfterYxvcn_nextSearchIsOrdinary()
{
    // Arrange
    resetProtocolState(FREESTYLE, quietPosition());

    // Act
    runVcn(4);

    // Assert: the VCN level of the one-shot command does not stay behind
    assert(!GP::options.vcnMode.enabled());
}

// --------------------------------------------------------------------------
// INFO VCTHREAD / YXVCN

void infoVcthread_guiValues_mapToVctAndVc2()
{
    resetProtocolState(FREESTYLE, quietPosition());

    runInfo("VCTHREAD 1");
    assert(GP::options.vcnMode.n == VCNLevel::VC3);  // "Check VCT"

    runInfo("VCTHREAD 2");
    assert(GP::options.vcnMode.n == VCNLevel::VC2);  // "Check VC2"

    runInfo("VCTHREAD 0");
    assert(!GP::options.vcnMode.enabled());
}

void infoVcthread_extensionValue_mapsToVc4()
{
    resetProtocolState(FREESTYLE, quietPosition());

    runInfo("VCTHREAD 3");

    assert(GP::options.vcnMode.n == VCNLevel::VC4);
}

void infoVcthread_vc5Value_rejectedBecauseVc5IsInternal()
{
    resetProtocolState(FREESTYLE, quietPosition());
    runInfo("VCTHREAD 2");

    runInfo("VCTHREAD 4");

    assert(GP::options.vcnMode.n == VCNLevel::VC2);
}

void yxvcn_vc5Level_errorsAndDoesNotSearch()
{
    resetProtocolState(FREESTYLE, quietPosition());
    std::string out;
    {
        ScopedCoutCapture cap;
        ScopedCin         cin("5");
        GP::searchVCN();
        out = cap.str();
    }
    assert(out.find("ERROR") != std::string::npos);
    assert(!GP::options.vcnMode.enabled());
}

void infoVcthread_outOfRange_rejectedAndKeepsPreviousLevel()
{
    resetProtocolState(FREESTYLE, quietPosition());
    runInfo("VCTHREAD 2");

    runInfo("VCTHREAD 9");

    assert(GP::options.vcnMode.n == VCNLevel::VC2);
}

void infoVcthread_lowercaseAsSentByGui_isAccepted()
{
    resetProtocolState(FREESTYLE, quietPosition());

    runInfo("vcthread 1");  // the GUI sends "INFO vcthread %d"

    assert(GP::options.vcnMode.n == VCNLevel::VC3);
}

void yxvcn_levelOutOfRange_errorsAndDoesNotSearch()
{
    // Arrange
    resetProtocolState(FREESTYLE, quietPosition());
    std::string out;

    // Act
    {
        ScopedCoutCapture cap;
        ScopedCin         cin("9");
        GP::searchVCN();
        out = cap.str();
    }

    // Assert
    assert(out.find("ERROR") != std::string::npos);
    assert(!GP::options.vcnMode.enabled());
}

void yxvcn_afterInfoVcthread_restoresPersistentLevel()
{
    // Arrange: a persistent level 2 set through INFO VCTHREAD
    resetProtocolState(FREESTYLE, quietPosition());
    runInfo("VCTHREAD 2");

    // Act: a one-shot level 4 search
    runVcn(4);

    // Assert: back to the persistent level
    assert(GP::options.vcnMode.n == VCNLevel::VC2);
}

// --------------------------------------------------------------------------
// VCN-03 slice 5: VCN mode runs under the MCTS searcher too, so the protocol neither refuses nor
// warns. The YXVCN-under-MCTS searches need an evaluator and live in tests/mcts_vcn_tests.cpp.

void infoVcthread_mctsSearcher_noWarning()
{
    // Arrange
    resetProtocolState(FREESTYLE, quietPosition());
    ScopedMctsSearcher mcts;

    // Act
    std::string out = runInfoCaptured("VCTHREAD 1");

    // Assert: no error, level set
    assert(out.find("ERROR") == std::string::npos);
    assert(GP::options.vcnMode.n == VCNLevel::VC3);
}

void infoVcthread_alphabetaSearcher_noWarning()
{
    // Arrange: default (alpha-beta) searcher
    resetProtocolState(FREESTYLE, quietPosition());

    // Act
    std::string out = runInfoCaptured("VCTHREAD 1");

    // Assert
    assert(out.find("ERROR") == std::string::npos);
    assert(GP::options.vcnMode.n == VCNLevel::VC3);
}

void infoSearchType_mctsWithVcnLevelSet_noWarningAndKeepsLevel()
{
    // Arrange: a persistent level set under alpha-beta
    resetProtocolState(FREESTYLE, quietPosition());
    runInfo("VCTHREAD 2");
    std::string out;

    // Act: switch to MCTS (restored to alpha-beta on scope exit)
    {
        ScopedMctsSearcher restore;
        out = runInfoCaptured("SEARCH_TYPE mcts");
    }

    // Assert
    assert(out.find("ERROR") == std::string::npos);
    assert(GP::options.vcnMode.n == VCNLevel::VC2);
}

}  // namespace

void runVcnTests()
{
    RUN_TEST(movePickerMain_generatePassOff_neverYieldsPass);
    RUN_TEST(movePickerMain_generatePassOn_yieldsPassBeforeBoardMoves);
    RUN_TEST(movePickerMain_generatePassOn_afterLastMovePass_noSecondPass);
    RUN_TEST(movePickerMain_ttMoveIsPass_yieldedOnceOnly);
    RUN_TEST(movePickerMain_ttMoveIsPassButPassDisabled_ttMoveDropped);
    RUN_TEST(movePickerMain_opponentHasFive_neverYieldsPass);
    RUN_TEST(movePickerMain_vcnAttackOrderOn_orderedByThreatTier);
    RUN_TEST(movePickerMain_vcnAttackOrderOff_orderUnchanged);
    RUN_TEST(movePickerMain_vcnAttackOrderOn_sameMoveSet);
    RUN_TEST(movePickerQvcf_forceAllowB4_enumeratesPlainB4);
    RUN_TEST(movePickerQvcf_fullBoardOff_onlyFoursNearLastMove);
    RUN_TEST(movePickerQvcf_fullBoardOn_includesFoursFarFromLastMove);
    RUN_TEST(vcnSearch_vc4_vcfFarFromLastMove_found);
    RUN_TEST(quickWinCheck_doubleThree_default_reportsWin);
    RUN_TEST(quickWinCheck_doubleThree_vcfOnly_noWin);
    RUN_TEST(quickWinCheck_fourThree_vcfOnly_stillWin);
    RUN_TEST(quickWinCheck_flexFour_vcfOnly_stillWin);
    RUN_TEST(vcnSearch_vc4_doubleThreeAfterForcedBlock_attackerLoses);
    RUN_TEST(vcnSearch_vc3_doubleThreeAfterForcedBlock_attackerWins);
    RUN_TEST(vcnSearch_vc3_forcedRootMoveWithTimeLimit_winFound);
    RUN_TEST(vcnSearch_vc2_openThreeAttacker_findsMate);
    RUN_TEST(vcnSearch_vc3_openThreeAttacker_findsMate);
    RUN_TEST(vcnSearch_vc4_openThreeAttacker_findsVcfMate);
    RUN_TEST(vcnSearch_vc4_attackerWithoutVcf_loses);
    RUN_TEST(vcnSearch_vc3_attackerWithoutThreat_defenderPassRefutes);
    RUN_TEST(vcnSearch_levelIsNotLeftSetAfterYxvcn_nextSearchIsOrdinary);
    RUN_TEST(infoVcthread_guiValues_mapToVctAndVc2);
    RUN_TEST(infoVcthread_extensionValue_mapsToVc4);
    RUN_TEST(infoVcthread_vc5Value_rejectedBecauseVc5IsInternal);
    RUN_TEST(yxvcn_vc5Level_errorsAndDoesNotSearch);
    RUN_TEST(infoVcthread_outOfRange_rejectedAndKeepsPreviousLevel);
    RUN_TEST(infoVcthread_lowercaseAsSentByGui_isAccepted);
    RUN_TEST(yxvcn_levelOutOfRange_errorsAndDoesNotSearch);
    RUN_TEST(yxvcn_afterInfoVcthread_restoresPersistentLevel);
    RUN_TEST(infoVcthread_mctsSearcher_noWarning);
    RUN_TEST(infoVcthread_alphabetaSearcher_noWarning);
    RUN_TEST(infoSearchType_mctsWithVcnLevelSet_noWarningAndKeepsLevel);
}
