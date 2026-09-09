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

// Behaviour tests for Search::generateRootMoves(), driven directly through a
// SearchThread fixture on the global engine. Focus: the rootMovesWhitelist
// early branch added for YXANALZ, and a regression lock that the empty-whitelist
// path still equals MovePicker + symmetry filtering.

#include "testutil.h"

#include "../core/pos.h"
#include "../game/board.h"
#include "../search/movepick.h"
#include "../search/opening.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <vector>

using Search::SearchOptions;

namespace {

std::unique_ptr<Board> makeBoard(Rule rule, std::vector<Pos> moves)
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(rule);
    for (Pos p : moves)
        board->move(rule, p);
    return board;
}

// Arrange the engine, run generateRootMoves() on the main thread for the given
// board + options, and return the first move of every generated root move.
std::vector<Pos> generatedRootPvHeads(const Board &board, const SearchOptions &opts)
{
    Search::Engine.clear(true);  // spawn threads if needed
    Search::Engine.ctx.options = opts;

    Search::SearchThread *th = Search::Engine.main();
    th->clear();
    th->setBoardAndEvaluator(board);
    Opening::expandCandidate(*th->board);

    Search::generateRootMoves(*th);

    std::vector<Pos> heads;
    heads.reserve(th->rootMoves.size());
    for (const auto &rm : th->rootMoves)
        heads.push_back(rm.pv[0]);
    return heads;
}

// Independent recomputation of what the non-whitelist path is supposed to yield:
// every ROOT MovePicker move, then Opening::filterSymmetryMoves.
std::vector<Pos> movePickerRootReference(const Board &board, Rule rule)
{
    std::vector<Pos>   moves;
    Search::MovePicker mp(rule, board, Search::MovePicker::ExtraArgs<Search::MovePicker::ROOT> {});
    while (Pos m = mp())
        moves.push_back(m);
    Opening::filterSymmetryMoves(board, moves);
    return moves;
}

std::vector<Pos> sorted(std::vector<Pos> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

bool contains(const std::vector<Pos> &v, Pos p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

// --------------------------------------------------------------------------

void generateRootMoves_whitelistEmpty_fallsBackToMovePicker()
{
    // Arrange
    auto           board = makeBoard(FREESTYLE, {Pos(7, 7), Pos(6, 6), Pos(8, 7), Pos(7, 8)});
    SearchOptions  opts;
    opts.rule = {FREESTYLE, GameRule::FREEOPEN};
    // whitelist left empty

    // Act
    std::vector<Pos> heads = generatedRootPvHeads(*board, opts);

    // Assert: identical set to an independent MovePicker + symmetry computation.
    std::vector<Pos> reference =
        movePickerRootReference(*Search::Engine.main()->board, FREESTYLE);
    assert(!heads.empty());
    assert(sorted(heads) == sorted(reference));
}

void generateRootMoves_whitelistNonEmpty_rootMovesEqualWhitelistExactly()
{
    // Arrange
    auto            board = makeBoard(FREESTYLE, {Pos(7, 7), Pos(8, 8)});
    SearchOptions   opts;
    opts.rule               = {FREESTYLE, GameRule::FREEOPEN};
    // One natural candidate plus one cell far from every stone.
    opts.rootMovesWhitelist = {Pos(6, 6), Pos(1, 1)};

    // Act
    std::vector<Pos> heads = generatedRootPvHeads(*board, opts);

    // Assert
    assert(heads == opts.rootMovesWhitelist);
}

void generateRootMoves_whitelistWithSymmetricEntries_keepsAll()
{
    // Arrange: empty board -> full 8-fold symmetry, so the normal path would
    // collapse a mirrored pair. (5,7) and (9,7) mirror across the x = 7 axis.
    auto          board = makeBoard(FREESTYLE, {});
    SearchOptions opts;
    opts.rule               = {FREESTYLE, GameRule::FREEOPEN};
    opts.rootMovesWhitelist = {Pos(5, 7), Pos(9, 7)};

    // Act
    std::vector<Pos> heads = generatedRootPvHeads(*board, opts);

    // Assert: both mirror entries survive.
    assert(heads.size() == 2);
    assert(contains(heads, Pos(5, 7)));
    assert(contains(heads, Pos(9, 7)));
}

void generateRootMoves_whitelistContainsBlockedMove_stillIncludesIt()
{
    // Arrange
    auto          board = makeBoard(FREESTYLE, {Pos(7, 7)});
    SearchOptions opts;
    opts.rule               = {FREESTYLE, GameRule::FREEOPEN};
    opts.blockMoves         = {Pos(6, 7)};
    opts.rootMovesWhitelist = {Pos(6, 7), Pos(8, 8)};

    // Act
    std::vector<Pos> heads = generatedRootPvHeads(*board, opts);

    // Assert: the blocked move is honoured because the user listed it explicitly.
    assert(heads == opts.rootMovesWhitelist);
    assert(contains(heads, Pos(6, 7)));
}

void generateRootMoves_whitelistOrderPreserved_matchesInputOrder()
{
    // Arrange
    auto          board = makeBoard(FREESTYLE, {});
    SearchOptions opts;
    opts.rule               = {FREESTYLE, GameRule::FREEOPEN};
    opts.rootMovesWhitelist = {Pos(9, 9), Pos(3, 3), Pos(7, 7), Pos(11, 2)};

    // Act (root moves are inspected before the search value sort)
    std::vector<Pos> heads = generatedRootPvHeads(*board, opts);

    // Assert: exact input order, no reordering.
    assert(heads == opts.rootMovesWhitelist);
}

}  // namespace

void runGenerateRootMovesTests()
{
    std::printf("-- generateRootMoves --\n");
    RUN_TEST(generateRootMoves_whitelistEmpty_fallsBackToMovePicker);
    RUN_TEST(generateRootMoves_whitelistNonEmpty_rootMovesEqualWhitelistExactly);
    RUN_TEST(generateRootMoves_whitelistWithSymmetricEntries_keepsAll);
    RUN_TEST(generateRootMoves_whitelistContainsBlockedMove_stillIncludesIt);
    RUN_TEST(generateRootMoves_whitelistOrderPreserved_matchesInputOrder);
}
