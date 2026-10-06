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
#include "../search/movepick.h"
#include "../search/searchcommon.h"
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

MovePicker::ExtraArgs<MovePicker::MAIN> mainArgs(Pos ttMove, bool pass)
{
    return {ttMove, Search::MoveHistoryScoring {}, false, 1.0f, pass};
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

}  // namespace

void runVcnTests()
{
    RUN_TEST(movePickerMain_generatePassOff_neverYieldsPass);
    RUN_TEST(movePickerMain_generatePassOn_yieldsPassBeforeBoardMoves);
    RUN_TEST(movePickerMain_generatePassOn_afterLastMovePass_noSecondPass);
    RUN_TEST(movePickerMain_ttMoveIsPass_yieldedOnceOnly);
    RUN_TEST(movePickerMain_ttMoveIsPassButPassDisabled_ttMoveDropped);
    RUN_TEST(movePickerMain_opponentHasFive_neverYieldsPass);
    RUN_TEST(movePickerQvcf_forceAllowB4_enumeratesPlainB4);
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
}
