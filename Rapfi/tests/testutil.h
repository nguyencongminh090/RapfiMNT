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

#pragma once

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

extern int testsRun;
extern int testsPassed;

// Run one AAA-structured test function. The function body uses assert() for its
// checks; reaching the end without aborting is a pass.
#define RUN_TEST(fn)                                    \
    do {                                               \
        ++testsRun;                                     \
        std::fflush(stdout);                            \
        fn();                                          \
        ++testsPassed;                                  \
        std::printf("[PASS] %s\n", #fn);               \
        std::fflush(stdout);                            \
    } while (0)

/// RAII redirect of std::cin from a fixed string, restored on scope exit. Used by
/// the protocol-level parsing tests to feed a "YXANALZ ..." token stream.
struct ScopedCin
{
    std::istringstream iss;
    std::streambuf    *saved;

    explicit ScopedCin(std::string s) : iss(std::move(s)), saved(std::cin.rdbuf(iss.rdbuf())) {}
    ~ScopedCin() { std::cin.rdbuf(saved); }
};

/// RAII capture of everything written to std::cout during the scope. The captured
/// text is available via str(); std::cout is restored on scope exit.
struct ScopedCoutCapture
{
    std::ostringstream oss;
    std::streambuf    *saved;

    ScopedCoutCapture() : saved(std::cout.rdbuf(oss.rdbuf())) {}
    ~ScopedCoutCapture() { std::cout.rdbuf(saved); }

    std::string str() const { return oss.str(); }
};
