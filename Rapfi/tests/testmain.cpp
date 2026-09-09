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

// Hand-rolled assert()-based test runner for the YXANALZ user-root-candidates
// slice. No GTest/Catch2 (see CLAUDE.md "Verification without a UI"). This target
// is compiled with -UNDEBUG regardless of build type so every assert() below and
// every engine invariant assert is actually evaluated.

#include "testutil.h"

#include "../command/command.h"
#include "../config.h"
#include "../search/searchengine.h"

#include <cassert>
#include <cstdio>

void runGenerateRootMovesTests();
void runAnalyzeMovesTests();
void runSimdLinearTests();

int testsRun    = 0;
int testsPassed = 0;

int main(int argc, char *argv[])
{
    // Prove to the reader (and to CI) that asserts are live in this binary: a
    // deliberately false assert must abort. If NDEBUG had leaked in, this would
    // fall through and the message below would print.
    if (argc > 1 && std::string(argv[1]) == "--check-asserts-live") {
        std::fprintf(stderr, "asserts are live: about to trip a false assert\n");
        assert(false && "sentinel: this assert must fire");
        std::fprintf(stderr, "BUG: assert(false) did not abort - NDEBUG leaked in\n");
        return 2;
    }

    Command::CommandLine::init(argc, argv);
    if (!Command::loadConfig()) {
        std::fprintf(stderr, "failed to load config\n");
        return 1;
    }

    runGenerateRootMovesTests();
    runAnalyzeMovesTests();
    runSimdLinearTests();

    std::printf("\n%d/%d tests passed\n", testsPassed, testsRun);
    return testsPassed == testsRun ? 0 : 1;
}
