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

// CMD-01: protocol-level tests for the hand-off between think()'s completion
// callback (main search thread) and runProtocol() (protocol thread). A client
// may answer the engine's move at once with the next command; that command must
// not be dropped as "received while thinking".
//
// Seam: a std::streambuf installed on std::cout. The move line is flushed with
// std::endl inside the callback, which calls sync() on the main search thread
// while it holds protocolMutex. sync() is therefore a deterministic point
// "after the move is visible to the client, before the callback returns", with
// no hook in the engine and no sleeps or timeouts.

#include "testutil.h"

#include "../core/pos.h"
#include "../core/types.h"
#include "../game/board.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <regex>
#include <streambuf>
#include <string>
#include <vector>

// Symbols defined in command/gomocup.cpp (namespace, external linkage).
namespace Command::GomocupProtocol {
extern std::unique_ptr<Board> board;
extern Search::SearchOptions  options;
extern std::atomic_bool       thinking;
bool                          runProtocol();
}  // namespace Command::GomocupProtocol

namespace GP = Command::GomocupProtocol;

namespace {

/// Output sink that plays the role of a fast client. It records every complete
/// output line, and when the first move line ("x,y") is flushed it records the
/// value of `thinking` at that moment and signals the test thread.
/// If `holdWhileThinking` is set and `thinking` is still true at the flush, it
/// keeps the callback inside sync() until the test thread calls release(): this
/// forces the interleaving where the client's next command is read before the
/// callback clears the flag. If `thinking` is already false, it never blocks
/// (the test thread's runProtocol() then waits on protocolMutex instead).
class ClientSink : public std::streambuf
{
public:
    explicit ClientSink(bool holdWhileThinking) : holdWhileThinking(holdWhileThinking) {}

    /// Block until the first move line has been flushed.
    void waitFirstMove()
    {
        std::unique_lock<std::mutex> lk(gateMutex);
        gateCv.wait(lk, [&] { return firstMoveSeen; });
    }

    /// Let a held callback continue.
    void release()
    {
        std::lock_guard<std::mutex> lk(gateMutex);
        released = true;
        gateCv.notify_all();
    }

    bool thinkingAtFirstMove() const { return thinkingAtFirstMoveValue; }

    std::vector<std::string> lines()
    {
        std::lock_guard<std::mutex> lk(textMutex);
        return completeLines;
    }

    std::vector<std::string> moveLines()
    {
        std::vector<std::string> moves;
        for (const auto &l : lines())
            if (isMoveLine(l))
                moves.push_back(l);
        return moves;
    }

    static bool isMoveLine(const std::string &l)
    {
        static const std::regex moveRe("^[0-9]+,[0-9]+$");
        return std::regex_match(l, moveRe);
    }

protected:
    int_type overflow(int_type c) override
    {
        if (c != traits_type::eof()) {
            std::lock_guard<std::mutex> lk(textMutex);
            append(static_cast<char>(c));
        }
        return traits_type::not_eof(c);
    }

    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        std::lock_guard<std::mutex> lk(textMutex);
        for (std::streamsize i = 0; i < n; i++)
            append(s[i]);
        return n;
    }

    int sync() override
    {
        bool newFirstMove = false;
        {
            std::lock_guard<std::mutex> lk(textMutex);
            if (!firstMoveFlushed)
                for (const auto &l : completeLines)
                    if (isMoveLine(l)) {
                        firstMoveFlushed = newFirstMove = true;
                        break;
                    }
        }
        if (!newFirstMove)
            return 0;

        // The move is now visible to the client. Sample the engine flag.
        bool stillThinking       = GP::thinking.load();
        thinkingAtFirstMoveValue = stillThinking;

        std::unique_lock<std::mutex> lk(gateMutex);
        firstMoveSeen = true;
        gateCv.notify_all();
        if (holdWhileThinking && stillThinking)
            gateCv.wait(lk, [&] { return released; });
        return 0;
    }

private:
    void append(char c)
    {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            completeLines.push_back(current);
            current.clear();
        }
        else
            current.push_back(c);
    }

    const bool holdWhileThinking;

    std::mutex               textMutex;
    std::string              current;
    std::vector<std::string> completeLines;
    bool                     firstMoveFlushed = false;

    std::mutex              gateMutex;
    std::condition_variable gateCv;
    bool                    firstMoveSeen            = false;
    bool                    released                 = false;
    std::atomic_bool        thinkingAtFirstMoveValue = false;
};

/// RAII install of a streambuf on std::cout.
struct ScopedCoutBuf
{
    std::streambuf *saved;
    explicit ScopedCoutBuf(std::streambuf *b) : saved(std::cout.rdbuf(b)) {}
    ~ScopedCoutBuf() { std::cout.rdbuf(saved); }
};

/// Feed `input` to runProtocol() until it is used up.
void runProtocolOn(const std::string &input)
{
    ScopedCin cin(input);
    while (!GP::runProtocol()) {}
}

// Engine (self, 1) has four in a row on row 7 (x = 7..10) and is to move:
// the only sensible answer is an immediate five at 6,7 or 11,7, which
// tryTrivialBestmove returns without a search.
const std::string kBoardFirst = "BOARD\n"
                                "7,7,1\n0,0,2\n8,7,1\n0,2,2\n9,7,1\n0,4,2\n10,7,1\n0,6,2\n"
                                "DONE\n";
// The client's reply, a different position: five at 2,3 or 7,3.
const std::string kBoardSecond = "BOARD\n"
                                 "3,3,1\n14,14,2\n4,3,1\n14,12,2\n5,3,1\n14,10,2\n6,3,1\n14,8,2\n"
                                 "DONE\n";

// Reset the protocol globals into a known, cheap state.
void resetProtocolState()
{
    Search::Engine.waitForIdle();
    Search::Engine.clear(true);

    GP::options                     = Search::SearchOptions {};
    GP::options.rule                = {Rule::FREESTYLE, GameRule::FREEOPEN};
    GP::options.disableOpeningQuery = true;
    GP::options.maxDepth            = 4;  // in case no trivial move is found
    GP::options.pondering           = false;

    GP::board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    GP::board->newGame(GP::options.rule);
    GP::thinking = false;
}

// ---------------------------------------------------------------------------
// think (completion callback): the flag is cleared before the move is visible.

void test_think_moveFlushedToClient_thinkingAlreadyCleared()
{
    // Arrange
    resetProtocolState();
    ClientSink sink(/*holdWhileThinking=*/false);

    // Act
    {
        ScopedCoutBuf out(&sink);
        runProtocolOn(kBoardFirst);
        sink.waitFirstMove();
        Search::Engine.waitForIdle();
    }

    // Assert
    auto moves = sink.moveLines();
    assert(moves.size() == 1);
    assert(moves[0] == "6,7" || moves[0] == "11,7");
    assert(!sink.thinkingAtFirstMove());
    assert(!GP::thinking.load());
}

// ---------------------------------------------------------------------------
// runProtocol: a BOARD sent right after the move is processed, not dropped.

void test_runProtocol_boardSentRightAfterMove_isProcessed()
{
    // Arrange: first BOARD starts a think; the sink holds the callback inside
    // the move's flush if the flag is still set (the HEAD interleaving).
    resetProtocolState();
    ClientSink sink(/*holdWhileThinking=*/true);
    std::vector<std::string> lines;

    {
        ScopedCoutBuf out(&sink);
        runProtocolOn(kBoardFirst);
        sink.waitFirstMove();

        // Act: the client answers the move at once with the next BOARD.
        {
            ScopedCin cin(kBoardSecond);
            GP::runProtocol();  // the BOARD command and its position block
            sink.release();
            Search::Engine.waitForIdle();
            while (!GP::runProtocol()) {}  // whatever is left of the input
        }
        Search::Engine.waitForIdle();
        lines = sink.lines();
    }

    // Assert: both positions were answered, in order, and no token of the
    // second BOARD block was parsed as a command.
    std::vector<std::string> moves;
    for (const auto &l : lines) {
        assert(l.find("Unknown command") == std::string::npos);
        if (ClientSink::isMoveLine(l))
            moves.push_back(l);
    }
    assert(moves.size() == 2);
    assert(moves[0] == "6,7" || moves[0] == "11,7");
    assert(moves[1] == "2,3" || moves[1] == "7,3");
    assert(!GP::thinking.load());
}

// ---------------------------------------------------------------------------
// runProtocol: a command read while a result is still pending is dropped with
// the rest of its line (existing contract, the other side of the boundary).

void test_runProtocol_commandWhileThinking_isDropped()
{
    // Arrange: a result is pending (flag set, nothing published yet).
    resetProtocolState();
    ClientSink sink(/*holdWhileThinking=*/false);
    GP::thinking = true;

    // Act
    std::string next;
    bool        exitRequested;
    {
        ScopedCoutBuf out(&sink);
        ScopedCin     cin("BOARD 1,1,1 2,2,2\nABOUT\n");
        exitRequested = GP::runProtocol();
        std::cin >> next;
    }
    GP::thinking = false;

    // Assert: nothing ran, nothing was printed, and the rest of the dropped
    // line was discarded so the next token is the next line's command.
    assert(!exitRequested);
    assert(sink.lines().empty());
    assert(GP::board->ply() == 0);
    assert(next == "ABOUT");
}

}  // namespace

void runGomocupTests()
{
    RUN_TEST(test_runProtocol_commandWhileThinking_isDropped);
    RUN_TEST(test_think_moveFlushedToClient_thinkingAlreadyCleared);
    RUN_TEST(test_runProtocol_boardSentRightAfterMove_isProcessed);
}
