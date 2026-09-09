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

// Protocol-level tests for the YXANALZ handler analyzeMoves(): token parsing,
// legality/PASS/duplicate handling, the multiPV / balanceMode side effects, the
// "no valid move -> do not search" guard, and the one-shot whitelist contract
// (the next TURN must not inherit a stale list). Plus one targeted end-to-end
// position check that the reported per-move values are sane.

#include "testutil.h"

#include "../core/pos.h"
#include "../core/types.h"
#include "../game/board.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

// Symbols defined in command/gomocup.cpp (namespace, external linkage).
namespace Command::GomocupProtocol {
extern std::unique_ptr<Board>  board;
extern Search::SearchOptions   options;
void                           analyzeMoves();
void                           turn();
}  // namespace Command::GomocupProtocol

namespace GP = Command::GomocupProtocol;

namespace {

// Reset the protocol-global board + options into a known, cheap-to-search state.
void resetProtocolState(Rule rule, const std::vector<Pos> &moves)
{
    Search::Engine.clear(true);
    Search::Engine.waitForIdle();

    GP::options                     = Search::SearchOptions {};
    GP::options.rule                = {rule, GameRule::FREEOPEN};
    GP::options.disableOpeningQuery = true;
    GP::options.maxDepth            = 4;  // keep every search tiny

    GP::board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    GP::board->newGame(rule);
    for (Pos p : moves)
        GP::board->move(rule, p);
}

// Run analyzeMoves() with `input` on std::cin, swallowing its console output,
// and block until the (possibly started) search finishes.
std::string runAnalyzeMoves(const std::string &input)
{
    ScopedCoutCapture cap;
    {
        ScopedCin cin(input);
        GP::analyzeMoves();
    }
    Search::Engine.waitForIdle();
    return cap.str();
}

bool contains(const std::vector<Pos> &v, Pos p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

// --------------------------------------------------------------------------

void analyzeMoves_validCoords_populatesWhitelistAndSetsMultiPv()
{
    // Arrange
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});

    // Act
    runAnalyzeMoves("6,6 9,9 5,8 DONE");

    // Assert: whitelist holds exactly the three parsed moves (order preserved)
    // and multiPV was bumped to the whitelist size. think() intentionally does
    // NOT clear the whitelist, so it is still observable here.
    std::vector<Pos> expected {Pos(6, 6), Pos(9, 9), Pos(5, 8)};
    assert(GP::options.rootMovesWhitelist == expected);
    assert(GP::options.multiPV == 3);
}

void analyzeMoves_illegalCoord_skipsEntryAndWarns()
{
    // Arrange: (7,7) is occupied.
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});

    // Act
    std::string out = runAnalyzeMoves("7,7 6,6 DONE");

    // Assert: occupied cell dropped, the valid one kept, a warning was emitted.
    assert(GP::options.rootMovesWhitelist == std::vector<Pos> {Pos(6, 6)});
    assert(out.find("ERROR") != std::string::npos);
}

void analyzeMoves_forbiddenPointRenjuBlack_skipsEntry()
{
    // Arrange: a Renju double-three at (7,7) for black. White plays in a far
    // corner so it neither interferes with the shape nor leaves a trailing pass
    // (a trailing pass would make parseLegalCoord reject every following coord).
    resetProtocolState(RENJU,
                       {Pos(5, 7),
                        Pos(0, 0),
                        Pos(6, 7),
                        Pos(1, 0),
                        Pos(7, 5),
                        Pos(0, 1),
                        Pos(7, 6),
                        Pos(2, 0)});
    assert(GP::board->sideToMove() == BLACK);
    assert(GP::board->checkForbiddenPoint(Pos(7, 7)) && "test fixture: (7,7) must be forbidden");

    // Act
    runAnalyzeMoves("7,7 10,10 DONE");

    // Assert: the forbidden point is dropped, the legal move survives.
    assert(!contains(GP::options.rootMovesWhitelist, Pos(7, 7)));
    assert(GP::options.rootMovesWhitelist == std::vector<Pos> {Pos(10, 10)});
}

void analyzeMoves_passToken_rejected()
{
    // Arrange
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});

    // Act: -1,-1 is the protocol PASS coordinate.
    runAnalyzeMoves("-1,-1 6,6 DONE");

    // Assert: PASS never enters the whitelist.
    assert(!contains(GP::options.rootMovesWhitelist, Pos::PASS));
    assert(GP::options.rootMovesWhitelist == std::vector<Pos> {Pos(6, 6)});
}

void analyzeMoves_duplicateCoord_deduplicates()
{
    // Arrange
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});

    // Act
    runAnalyzeMoves("6,6 6,6 9,9 6,6 DONE");

    // Assert: each coordinate appears once, first-seen order preserved.
    std::vector<Pos> expected {Pos(6, 6), Pos(9, 9)};
    assert(GP::options.rootMovesWhitelist == expected);
    assert(GP::options.multiPV == 2);
}

void analyzeMoves_allEntriesInvalid_errorsAndDoesNotSearch()
{
    // Arrange: both coords are occupied cells.
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});
    int plyBefore = GP::board->ply();

    // Act
    std::string out = runAnalyzeMoves("7,7 8,8 DONE");

    // Assert: no whitelist, an error, and no search happened (no bestmove was
    // played onto the protocol board).
    assert(GP::options.rootMovesWhitelist.empty());
    assert(out.find("No valid analyze move") != std::string::npos);
    assert(GP::board->ply() == plyBefore);
}

void analyzeMoves_forcesBalanceModeNone()
{
    // Arrange: a leftover balance mode from a previous YXBALANCEONE.
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});
    GP::options.balanceMode = Search::SearchOptions::BALANCE_ONE;

    // Act
    runAnalyzeMoves("6,6 9,9 DONE");

    // Assert
    assert(GP::options.balanceMode == Search::SearchOptions::BALANCE_NONE);
}

void analyzeMoves_leavesNoStaleWhitelistForNextTurn()
{
    // Arrange: a YXANALZ turn sets the whitelist...
    resetProtocolState(FREESTYLE, {Pos(7, 7), Pos(8, 8)});
    runAnalyzeMoves("6,6 9,9 DONE");
    assert(!GP::options.rootMovesWhitelist.empty());

    // Act: ...the next TURN must clear it before generating its own root moves.
    {
        ScopedCoutCapture cap;
        {
            ScopedCin cin("10,10\n");
            GP::turn();
        }
        Search::Engine.waitForIdle();
    }

    // Assert
    assert(GP::options.rootMovesWhitelist.empty());
}

void analyzeMoves_threeMoves_reportsSanePerMoveValues()
{
    // Arrange: a small freestyle middlegame, analyse three concrete replies at a
    // real (if shallow) depth.
    resetProtocolState(FREESTYLE,
                       {Pos(7, 7), Pos(8, 8), Pos(7, 8), Pos(7, 9), Pos(6, 8)});
    GP::options.maxDepth = 8;
    std::vector<Pos> asked {Pos(8, 7), Pos(9, 8), Pos(6, 9)};

    // Act
    runAnalyzeMoves("8,7 9,8 6,9 DONE");

    // Assert: one PV line per asked move, each with a real value and a PV that
    // starts on one of the asked moves.
    const Search::RootMoves &rm = Search::Engine.main()->rootMoves;
    assert(rm.size() == asked.size());
    for (const auto &r : rm) {
        assert(!r.pv.empty());
        assert(contains(asked, r.pv[0]));
        assert(r.value != VALUE_NONE);
        assert(r.value > -VALUE_INFINITE && r.value < VALUE_INFINITE);
    }
    // Values are sorted descending (best first) by the search.
    for (size_t i = 1; i < rm.size(); i++)
        assert(rm[i - 1].value >= rm[i].value);
}

}  // namespace

void runAnalyzeMovesTests()
{
    std::printf("-- analyzeMoves --\n");
    RUN_TEST(analyzeMoves_validCoords_populatesWhitelistAndSetsMultiPv);
    RUN_TEST(analyzeMoves_illegalCoord_skipsEntryAndWarns);
    RUN_TEST(analyzeMoves_forbiddenPointRenjuBlack_skipsEntry);
    RUN_TEST(analyzeMoves_passToken_rejected);
    RUN_TEST(analyzeMoves_duplicateCoord_deduplicates);
    RUN_TEST(analyzeMoves_allEntriesInvalid_errorsAndDoesNotSearch);
    RUN_TEST(analyzeMoves_forcesBalanceModeNone);
    RUN_TEST(analyzeMoves_leavesNoStaleWhitelistForNextTurn);
    RUN_TEST(analyzeMoves_threeMoves_reportsSanePerMoveValues);
}
