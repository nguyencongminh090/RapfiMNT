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

// PERF-01 tests (docs/todo/PERF-01-movepick-branch-miss.md): radixSortByScore must reproduce
// std::sort + ScoreComparator byte for byte, and fastPartialSort / MovePicker must keep the exact
// order they had before the change. The fastPartialSort reference is a frozen copy of the
// pre-PERF-01 code, because its insertion and partial_sort branches have implementation-defined
// orders that are part of the search behaviour.
//
// PERF-02 tests (docs/todo/PERF-02-insertion-branch-sort.md): on AVX-512 builds the insertion
// branch under ScoreComparator runs insertionBranchByScore, which must equal the frozen loop byte
// for byte, including the order of the below-limit tail and of duplicate pos (stability).

#include "testutil.h"

#include "../core/pos.h"
#include "../core/random.h"
#include "../core/types.h"
#include "../game/board.h"
#include "../game/movegen.h"
#include "../search/movepick.h"
#include "../search/movesort.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

using Search::MovePicker;

namespace {

/// Frozen copy of fastPartialSort as of commit 962fedd (before PERF-01). Do not edit: it is the
/// behavioural reference the live implementation must keep matching.
template <typename Comparator>
void frozenFastPartialSort(ScoredMove *begin, ScoredMove *end, Score limit, Comparator comp)
{
    constexpr size_t InsertionSortLimit = MAX_MOVES / 4;
    constexpr size_t SortLimit          = MAX_MOVES * 2 / 3;

    size_t nMoves = end - begin;
    if (nMoves <= InsertionSortLimit) {
        for (ScoredMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
            if (p->score >= limit) {
                ScoredMove tmp = *p, *q;
                *p             = *++sortedEnd;
                for (q = sortedEnd; q != begin && comp(tmp, *(q - 1)); --q)
                    *q = *(q - 1);
                *q = tmp;
            }
    }
    else if (nMoves <= SortLimit) {
        std::sort(begin, end, comp);
    }
    else {
        std::partial_sort(begin, begin + SortLimit, end, comp);
    }
}

constexpr size_t InsertionSortLimit = MAX_MOVES / 4;      // 121
constexpr size_t SortLimit          = MAX_MOVES * 2 / 3;  // 323

uint32_t unionBits(const ScoredMove &m)
{
    uint32_t u;
    std::memcpy(&u, reinterpret_cast<const char *>(&m) + 4, 4);
    return u;
}

void setUnionBits(ScoredMove &m, uint32_t u)
{
    std::memcpy(reinterpret_cast<char *>(&m) + 4, &u, 4);
}

ScoredMove makeMove(int pos, int score, uint32_t bits)
{
    ScoredMove m;
    std::memset(&m, 0, sizeof(m));
    m.pos   = Pos(int16_t(pos));
    m.score = Score(score);
    setUnionBits(m, bits);
    return m;
}

bool sameBytes(const std::vector<ScoredMove> &a, const std::vector<ScoredMove> &b)
{
    return a.size() == b.size()
           && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(ScoredMove)) == 0);
}

/// `n` distinct pos values drawn from the whole int16 range, the extremes included when n >= 2.
std::vector<int> distinctPositions(PRNG &prng, size_t n)
{
    std::vector<int>        out;
    std::unordered_set<int> seen;
    if (n >= 2) {
        out = {-32768, 32767};
        seen.insert(out.begin(), out.end());
    }
    while (out.size() < n) {
        int p = int(int16_t(uint16_t(prng())));
        if (seen.insert(p).second)
            out.push_back(p);
    }
    std::shuffle(out.begin(), out.end(), prng);
    out.resize(n);
    return out;
}

enum class ScoreRange { Full, Narrow };

/// Random move list with distinct pos. Full: scores over all of int16 with the extremes forced
/// in; Narrow: scores in [-4, 4] so ties (broken by pos) are common and negatives are present.
std::vector<ScoredMove> randomScoredList(PRNG &prng, size_t n, ScoreRange range)
{
    std::vector<int>        pos = distinctPositions(prng, n);
    std::vector<ScoredMove> list;
    for (size_t i = 0; i < n; i++) {
        int score = range == ScoreRange::Full ? int(int16_t(uint16_t(prng())))
                                              : int(prng() % 9) - 4;
        if (range == ScoreRange::Full && i == 0)
            score = -32768;
        if (range == ScoreRange::Full && i == 1)
            score = 32767;
        list.push_back(makeMove(pos[i], score, uint32_t(prng())));
    }
    return list;
}

/// Random list with distinct pos and policy values from a small set (ties are common).
std::vector<ScoredMove> randomPolicyList(PRNG &prng, size_t n)
{
    std::vector<int>        pos = distinctPositions(prng, n);
    std::vector<ScoredMove> list;
    for (size_t i = 0; i < n; i++) {
        ScoredMove m = makeMove(pos[i], int(int16_t(uint16_t(prng()))), 0);
        m.policy     = float(prng() % 16) / 16.0f;
        list.push_back(m);
    }
    return list;
}

std::vector<ScoredMove> stdSorted(std::vector<ScoredMove> list)
{
    std::sort(list.begin(), list.end(), ScoredMove::ScoreComparator {});
    return list;
}

// ---------------------------------------------------------------------------
// radixSortByScore
// ---------------------------------------------------------------------------

void radixSortByScore_randomLists_matchStdSortWithScoreComparator()
{
    for (ScoreRange range : {ScoreRange::Full, ScoreRange::Narrow})
        for (size_t n : {size_t(2), size_t(3), size_t(122), size_t(200), size_t(323), size_t(485)})
            for (uint64_t seed = 1; seed <= 8; seed++) {
                // Arrange
                PRNG                    prng(seed * 1000 + n);
                std::vector<ScoredMove> list     = randomScoredList(prng, n, range);
                std::vector<ScoredMove> expected = stdSorted(list);

                // Act
                Search::radixSortByScore(list.data(), list.size());

                // Assert
                assert(sameBytes(list, expected));
            }
}

void radixSortByScore_int16Extremes_orderedByScoreDescThenPosAsc()
{
    // Arrange: every boundary of the key map (score and signed pos at both int16 ends, -1/0).
    std::vector<ScoredMove> list = {
        makeMove(0, 0, 1),           makeMove(32767, -32768, 2), makeMove(-32768, 32767, 3),
        makeMove(-1, -1, 4),         makeMove(5, 32767, 5),      makeMove(-32768, -32768, 6),
        makeMove(32767, 0, 7),       makeMove(-5, 0, 8),         makeMove(1, -1, 9),
    };
    const std::vector<ScoredMove> expected = {
        makeMove(-32768, 32767, 3),  makeMove(5, 32767, 5),      makeMove(-5, 0, 8),
        makeMove(0, 0, 1),           makeMove(32767, 0, 7),      makeMove(-1, -1, 4),
        makeMove(1, -1, 9),          makeMove(-32768, -32768, 6), makeMove(32767, -32768, 2),
    };

    // Act
    Search::radixSortByScore(list.data(), list.size());

    // Assert
    assert(sameBytes(list, expected));
}

void radixSortByScore_allEqualScores_orderedByPosAscending()
{
    // Arrange
    PRNG                    prng(42);
    std::vector<ScoredMove> list = randomScoredList(prng, 200, ScoreRange::Narrow);
    for (ScoredMove &m : list)
        m.score = Score(-7);

    // Act
    Search::radixSortByScore(list.data(), list.size());

    // Assert
    for (size_t i = 0; i + 1 < list.size(); i++)
        assert(list[i].pos < list[i + 1].pos);
    assert(sameBytes(list, stdSorted(list)));
}

void radixSortByScore_unionBits_preservedPerMove()
{
    // Arrange: union bits include quiet/signalling NaN, infinities and -0.0f patterns.
    PRNG                    prng(7);
    std::vector<ScoredMove> list = randomScoredList(prng, 250, ScoreRange::Narrow);
    const uint32_t special[] = {0x7FC00000u, 0x7FA00001u, 0xFFC00000u, 0x7F800000u,
                                0xFF800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu};
    for (size_t i = 0; i < std::size(special); i++)
        setUnionBits(list[i], special[i]);
    std::vector<std::pair<int, uint32_t>> before;
    for (const ScoredMove &m : list)
        before.emplace_back(int(m.pos), unionBits(m));
    std::sort(before.begin(), before.end());

    // Act
    Search::radixSortByScore(list.data(), list.size());

    // Assert
    std::vector<std::pair<int, uint32_t>> after;
    for (const ScoredMove &m : list)
        after.emplace_back(int(m.pos), unionBits(m));
    std::sort(after.begin(), after.end());
    assert(after == before);
    assert(sameBytes(list, stdSorted(list)));
}

void radixSortByScore_singleDigitKeys_skippedPassesStillSorted()
{
    // Arrange: one score and pos inside one low byte, so three of four key digits are constant.
    PRNG                    prng(11);
    std::vector<ScoredMove> list;
    for (int p = 0; p < 200; p++)
        list.push_back(makeMove(p, 3, uint32_t(prng())));
    std::shuffle(list.begin(), list.end(), prng);

    // Act
    Search::radixSortByScore(list.data(), list.size());

    // Assert
    for (size_t i = 0; i < list.size(); i++)
        assert(int(list[i].pos) == int(i));
    assert(sameBytes(list, stdSorted(list)));
}

void radixSortByScore_emptyOrSingle_isNoop()
{
    // Arrange
    ScoredMove sentinel = makeMove(123, 45, 0xDEADBEEFu);
    ScoredMove one      = makeMove(-9, -32768, 0x7FC00000u);
    ScoredMove oneCopy  = one;

    // Act
    Search::radixSortByScore(&sentinel, 0);
    Search::radixSortByScore(&one, 1);

    // Assert
    ScoredMove sentinelCopy = makeMove(123, 45, 0xDEADBEEFu);
    assert(std::memcmp(&sentinel, &sentinelCopy, sizeof(ScoredMove)) == 0);
    assert(std::memcmp(&one, &oneCopy, sizeof(ScoredMove)) == 0);
}

// ---------------------------------------------------------------------------
// fastPartialSort (regression guards against the frozen pre-PERF-01 copy)
// ---------------------------------------------------------------------------

constexpr size_t ThresholdSizes[] = {0, 1, 2, 120, 121, 122, 200, 322, 323, 324, 485};

void fastPartialSort_scoreComparatorAcrossSizeThresholds_matchFrozenReference()
{
    for (ScoreRange range : {ScoreRange::Full, ScoreRange::Narrow})
        for (size_t n : ThresholdSizes)
            for (uint64_t seed = 1; seed <= 4; seed++) {
                // Arrange
                PRNG                    prng(seed * 7919 + n);
                std::vector<ScoredMove> list     = randomScoredList(prng, n, range);
                std::vector<ScoredMove> expected = list;
                frozenFastPartialSort(expected.data(),
                                      expected.data() + n,
                                      0,
                                      ScoredMove::ScoreComparator {});

                // Act
                Search::fastPartialSort(list.data(),
                                        list.data() + n,
                                        0,
                                        ScoredMove::ScoreComparator {});

                // Assert
                assert(sameBytes(list, expected));
            }
}

void fastPartialSort_policyComparatorAcrossSizeThresholds_matchFrozenReference()
{
    for (size_t n : ThresholdSizes)
        for (uint64_t seed = 1; seed <= 4; seed++) {
            // Arrange
            PRNG                    prng(seed * 104729 + n);
            std::vector<ScoredMove> list     = randomPolicyList(prng, n);
            std::vector<ScoredMove> expected = list;
            frozenFastPartialSort(expected.data(),
                                  expected.data() + n,
                                  0,
                                  ScoredMove::PolicyComparator {});

            // Act
            Search::fastPartialSort(list.data(),
                                    list.data() + n,
                                    0,
                                    ScoredMove::PolicyComparator {});

            // Assert
            assert(sameBytes(list, expected));
        }
}

void fastPartialSort_middleBranch_matchesStdSortWithScoreComparator()
{
    for (size_t n : {InsertionSortLimit + 1, size_t(200), SortLimit}) {
        // Arrange
        PRNG                    prng(n);
        std::vector<ScoredMove> list     = randomScoredList(prng, n, ScoreRange::Full);
        std::vector<ScoredMove> expected = stdSorted(list);

        // Act
        Search::fastPartialSort(list.data(), list.data() + n, 0, ScoredMove::ScoreComparator {});

        // Assert
        assert(sameBytes(list, expected));
    }
}

void fastPartialSort_middleBranchAllEqualScores_orderedByPos()
{
    // Arrange
    PRNG                    prng(99);
    std::vector<ScoredMove> list = randomScoredList(prng, 200, ScoreRange::Narrow);
    for (ScoredMove &m : list)
        m.score = Score(-1);

    // Act
    Search::fastPartialSort(list.data(),
                            list.data() + list.size(),
                            0,
                            ScoredMove::ScoreComparator {});

    // Assert
    for (size_t i = 0; i + 1 < list.size(); i++)
        assert(list[i].pos < list[i + 1].pos);
}

enum class InsertionScheme { NarrowTies, FullInt16, ExtremesOnly, AllNegative, NoneNegative };

/// Random list for the insertion branch. Unlike randomScoredList, pos may repeat when
/// `duplicatePos` is set (real DEFENDFOUR/DEFENDB4F3 lists do that), with random union bits.
std::vector<ScoredMove>
randomInsertionList(PRNG &prng, size_t n, InsertionScheme scheme, bool duplicatePos)
{
    std::vector<ScoredMove> list;
    for (size_t i = 0; i < n; i++) {
        int score = 0;
        switch (scheme) {
        case InsertionScheme::NarrowTies: score = int(prng() % 9) - 4; break;
        case InsertionScheme::FullInt16: score = int(int16_t(uint16_t(prng()))); break;
        case InsertionScheme::ExtremesOnly: {
            const int v[] = {-32768, -1, 0, 32767};
            score         = v[prng() % 4];
            break;
        }
        case InsertionScheme::AllNegative: score = -1 - int(prng() % 32768); break;
        case InsertionScheme::NoneNegative: score = int(prng() % 32768); break;
        }
        const int pos = duplicatePos ? int(prng() % 6) - 3 : int(int16_t(uint16_t(prng())));
        list.push_back(makeMove(pos, score, uint32_t(prng())));
    }
    return list;
}

std::vector<ScoredMove> frozenInsertion(std::vector<ScoredMove> list, Score limit)
{
    assert(list.size() <= InsertionSortLimit);
    frozenFastPartialSort(list.data(),
                          list.data() + list.size(),
                          limit,
                          ScoredMove::ScoreComparator {});
    return list;
}

constexpr size_t InsertionSizes[] = {0, 1, 2, 3, 8, 9, 15, 16, 17, 64, 120, 121};
constexpr InsertionScheme AllInsertionSchemes[] = {InsertionScheme::NarrowTies,
                                                   InsertionScheme::FullInt16,
                                                   InsertionScheme::ExtremesOnly,
                                                   InsertionScheme::AllNegative,
                                                   InsertionScheme::NoneNegative};

void fastPartialSort_insertionBranchDuplicatePos_matchesFrozenReference()
{
    for (InsertionScheme scheme : AllInsertionSchemes)
        for (size_t n : InsertionSizes)
            for (uint64_t seed = 1; seed <= 8; seed++) {
                // Arrange
                PRNG                    prng(seed * 31337 + n * 17 + size_t(scheme));
                std::vector<ScoredMove> list     = randomInsertionList(prng, n, scheme, true);
                std::vector<ScoredMove> expected = frozenInsertion(list, 0);

                // Act
                Search::fastPartialSort(list.data(),
                                        list.data() + n,
                                        0,
                                        ScoredMove::ScoreComparator {});

                // Assert
                assert(sameBytes(list, expected));
            }
}

// ---------------------------------------------------------------------------
// insertionBranchByScore (PERF-02, AVX-512 builds only)
// ---------------------------------------------------------------------------

#ifdef USE_AVX512

void insertionBranchByScore_randomLists_matchFrozenReference()
{
    for (bool dup : {false, true})
        for (InsertionScheme scheme : AllInsertionSchemes)
            for (size_t n : InsertionSizes)
                for (uint64_t seed = 1; seed <= 16; seed++) {
                    // Arrange
                    PRNG prng(seed * 6007 + n * 101 + size_t(scheme) * 7 + size_t(dup));
                    std::vector<ScoredMove> list     = randomInsertionList(prng, n, scheme, dup);
                    std::vector<ScoredMove> expected = frozenInsertion(list, 0);

                    // Act
                    Search::insertionBranchByScore(list.data(), n, 0);

                    // Assert
                    assert(sameBytes(list, expected));
                }
}

void insertionBranchByScore_nonZeroLimit_matchesFrozenReference()
{
    for (int limit : {-32768, -5, -1, 1, 3, 100, 32767})
        for (InsertionScheme scheme : AllInsertionSchemes)
            for (size_t n : InsertionSizes)
                for (uint64_t seed = 1; seed <= 4; seed++) {
                    // Arrange
                    PRNG prng(seed * 7001 + n * 13 + size_t(scheme) + size_t(limit & 0xFFFF));
                    std::vector<ScoredMove> list = randomInsertionList(prng, n, scheme, seed & 1);
                    std::vector<ScoredMove> expected = frozenInsertion(list, Score(limit));

                    // Act
                    Search::insertionBranchByScore(list.data(), n, Score(limit));

                    // Assert
                    assert(sameBytes(list, expected));
                }
}

void insertionBranchByScore_duplicatePos_stableByArrival()
{
    // Arrange: three moves share pos 5 and score 2 but carry different union bits; arrival order
    // A, B, C must survive. A negative move between them exercises the partition too.
    const std::vector<ScoredMove> input = {
        makeMove(5, 2, 0xA), makeMove(9, -3, 0x1), makeMove(5, 2, 0xB),
        makeMove(1, 2, 0x2), makeMove(5, 2, 0xC),
    };
    const std::vector<ScoredMove> expected = {
        makeMove(1, 2, 0x2), makeMove(5, 2, 0xA), makeMove(5, 2, 0xB),
        makeMove(5, 2, 0xC), makeMove(9, -3, 0x1),
    };
    std::vector<ScoredMove> list = input;

    // Act
    Search::insertionBranchByScore(list.data(), list.size(), 0);

    // Assert
    assert(sameBytes(list, expected));
    assert(sameBytes(frozenInsertion(input, 0), expected));
}

void insertionBranchByScore_negativeFirstMove_keptLastInPrefix()
{
    // Arrange: begin[0] is negative, yet the loop never tests it, so it stays in the sorted
    // prefix and sorts behind the non-negative moves (but ahead of the untested tail).
    const std::vector<ScoredMove> input = {
        makeMove(0, -10, 1), makeMove(1, 5, 2), makeMove(2, -1, 3), makeMove(3, 7, 4),
    };
    const std::vector<ScoredMove> expected = {
        makeMove(3, 7, 4), makeMove(1, 5, 2), makeMove(0, -10, 1), makeMove(2, -1, 3),
    };
    std::vector<ScoredMove> list = input;

    // Act
    Search::insertionBranchByScore(list.data(), list.size(), 0);

    // Assert
    assert(sameBytes(list, expected));
    assert(sameBytes(frozenInsertion(input, 0), expected));
}

void insertionBranchByScore_negativeTail_isFifoQueueOrder()
{
    // Arrange: input  P0 N1 N2 Q3 N4 Q5 (P = prefix, N = negative, Q = qualifying).
    // Queue of negatives: [N1] [N1 N2]; Q3 rotates N1 to the back: [N2 N1]; [N2 N1 N4];
    // Q5 rotates N2: [N1 N4 N2]. The prefix {P0, Q3, Q5} is sorted by score.
    const std::vector<ScoredMove> input = {
        makeMove(10, 1, 0), makeMove(11, -1, 1), makeMove(12, -2, 2),
        makeMove(13, 3, 3), makeMove(14, -4, 4), makeMove(15, 2, 5),
    };
    const std::vector<ScoredMove> expected = {
        makeMove(13, 3, 3),  makeMove(15, 2, 5),  makeMove(10, 1, 0),
        makeMove(11, -1, 1), makeMove(14, -4, 4), makeMove(12, -2, 2),
    };
    std::vector<ScoredMove> list = input;

    // Act
    Search::insertionBranchByScore(list.data(), list.size(), 0);

    // Assert
    assert(sameBytes(list, expected));
    assert(sameBytes(frozenInsertion(input, 0), expected));
}

void insertionBranchByScore_unionBits_preservedPerMove()
{
    // Arrange
    PRNG                    prng(77);
    std::vector<ScoredMove> list = randomInsertionList(prng, 121, InsertionScheme::NarrowTies, false);
    const uint32_t special[] = {0x7FC00000u, 0x7FA00001u, 0xFFC00000u, 0x7F800000u,
                                0xFF800000u, 0x80000000u, 0x00000000u, 0xFFFFFFFFu};
    for (size_t i = 0; i < std::size(special); i++)
        setUnionBits(list[i * 7], special[i]);
    std::vector<std::pair<int, uint32_t>> before;
    for (const ScoredMove &m : list)
        before.emplace_back(int(m.pos), unionBits(m));
    std::sort(before.begin(), before.end());
    const std::vector<ScoredMove> expected = frozenInsertion(list, 0);

    // Act
    Search::insertionBranchByScore(list.data(), list.size(), 0);

    // Assert
    std::vector<std::pair<int, uint32_t>> after;
    for (const ScoredMove &m : list)
        after.emplace_back(int(m.pos), unionBits(m));
    std::sort(after.begin(), after.end());
    assert(after == before);
    assert(sameBytes(list, expected));
}

void insertionBranchByScore_emptyOrSingle_isNoop()
{
    // Arrange
    ScoredMove sentinel = makeMove(123, 45, 0xDEADBEEFu);
    ScoredMove one      = makeMove(-9, -32768, 0x7FC00000u);
    ScoredMove oneCopy  = one;

    // Act
    Search::insertionBranchByScore(&sentinel, 0, 0);
    Search::insertionBranchByScore(&one, 1, 0);

    // Assert
    ScoredMove sentinelCopy = makeMove(123, 45, 0xDEADBEEFu);
    assert(std::memcmp(&sentinel, &sentinelCopy, sizeof(ScoredMove)) == 0);
    assert(std::memcmp(&one, &oneCopy, sizeof(ScoredMove)) == 0);
}

#endif  // USE_AVX512

// ---------------------------------------------------------------------------
// MovePicker (MAIN): drained sequence unchanged vs goldens from the pre-PERF-01 picker
// ---------------------------------------------------------------------------

struct PickerFixture
{
    const char    *name;
    Rule           rule;
    int            boardSize;
    CandidateRange range;
    size_t         numStones;           ///< prefix of fixtureMoves() played on the board
    size_t         minMoves, maxMoves;  ///< fastPartialSort branch the fixture must hit
    size_t         goldenCount;
    uint64_t       goldenPrefixHash;  ///< hash of the first SortLimit moves (portable)
    uint64_t       goldenFullHash;    ///< hash of the whole sequence under libstdc++
};

std::vector<Pos> fixtureMoves(int boardSize)
{
    const int c = boardSize / 2;
    return {Pos(c, c),
            Pos(c - 3, c - 4),
            Pos(c + 1, c),
            Pos(c + 4, c - 2),
            Pos(c, c + 2),
            Pos(c - 2, c + 3),
            Pos(c + 2, c + 1)};
}

uint64_t fnv1a(const std::vector<Pos> &seq, size_t limit = SIZE_MAX)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < std::min(limit, seq.size()); i++) {
        const Pos p = seq[i];
        uint16_t v = uint16_t(int16_t(int(p)));
        for (int b = 0; b < 2; b++) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

void movePickerMain_listSizesAcrossSortBranches_sequenceUnchangedVsBaseline()
{
    // Goldens captured 2026-11-04 from the pre-PERF-01 picker (std::sort middle branch), gcc /
    // libstdc++. The first SortLimit moves have a unique order on every toolchain (insertion
    // branch is our own code, sorted prefixes are a strict total order). Beyond SortLimit the
    // partial_sort branch leaves a heap-ordered tail that differs between standard libraries, so
    // the full-sequence hash is checked under libstdc++ only; it is what catches an n > 323 list
    // being routed through another sort.
    const PickerFixture fixtures[] = {
        {"15x15 SQUARE3_LINE4 freestyle (insertion branch)", FREESTYLE, 15,
         CandidateRange::SQUARE3_LINE4, 2, 1, InsertionSortLimit, 86, 0x51c5863317165833ull,
         0x51c5863317165833ull},
        {"15x15 FULL_BOARD freestyle (middle branch)", FREESTYLE, 15, CandidateRange::FULL_BOARD, 7,
         InsertionSortLimit + 1, SortLimit, 218, 0x11eb728ab7eb0b06ull,
         0x11eb728ab7eb0b06ull},
        {"15x15 FULL_BOARD renju (middle branch)", RENJU, 15, CandidateRange::FULL_BOARD, 7,
         InsertionSortLimit + 1, SortLimit, 218, 0x53c89631f0cfe30aull,
         0x53c89631f0cfe30aull},
        {"20x20 FULL_BOARD freestyle (partial_sort branch)", FREESTYLE, 20,
         CandidateRange::FULL_BOARD, 7, SortLimit + 1, MAX_MOVES, 393, 0x4ebe29ff80de942full,
         0xa47c780877b6cb5cull},
    };

    std::vector<std::vector<Pos>> sequences;
    for (const PickerFixture &f : fixtures) {
        // Arrange
        Board board(f.boardSize, f.range);
        board.newGame(f.rule);
        const std::vector<Pos> stones = fixtureMoves(f.boardSize);
        for (size_t i = 0; i < f.numStones; i++)
            board.move(f.rule, stones[i]);
        const Color self = board.sideToMove(), oppo = ~self;
        assert(board.p4Count(self, A_FIVE) + board.p4Count(oppo, A_FIVE)
                       + board.p4Count(oppo, B_FLEX4) + board.p4Count(oppo, C_BLOCK4_FLEX3)
                   == 0
               && "fixture: quiet position, the picker sorts once in its MAIN_MOVES stage");

        // Act
        MovePicker mp(f.rule,
                      board,
                      MovePicker::ExtraArgs<MovePicker::MAIN> {Pos::NONE,
                                                               Search::MoveHistoryScoring {}});
        std::vector<Pos> seq;
        while (Pos m = mp())
            seq.push_back(m);
        sequences.push_back(std::move(seq));
    }

    // Assert (all values are printed first so a mismatch shows every fixture).
    for (size_t i = 0; i < std::size(fixtures); i++)
        std::printf("    %s: count=%zu prefix=0x%016llx full=0x%016llx\n",
                    fixtures[i].name,
                    sequences[i].size(),
                    (unsigned long long)fnv1a(sequences[i], SortLimit),
                    (unsigned long long)fnv1a(sequences[i]));
    std::fflush(stdout);
    for (size_t i = 0; i < std::size(fixtures); i++) {
        const PickerFixture    &f   = fixtures[i];
        const std::vector<Pos> &seq = sequences[i];
        assert(seq.size() >= f.minMoves && seq.size() <= f.maxMoves
               && "fixture: list size selects the intended fastPartialSort branch");
        assert(seq.size() == f.goldenCount);
        assert(fnv1a(seq, SortLimit) == f.goldenPrefixHash);
#ifdef __GLIBCXX__
        assert(fnv1a(seq) == f.goldenFullHash);
#endif
    }
}

}  // namespace

void runMoveSortTests()
{
    std::printf("\n== PERF-01 move sort ==\n");
    RUN_TEST(fastPartialSort_scoreComparatorAcrossSizeThresholds_matchFrozenReference);
    RUN_TEST(fastPartialSort_policyComparatorAcrossSizeThresholds_matchFrozenReference);
    RUN_TEST(fastPartialSort_middleBranch_matchesStdSortWithScoreComparator);
    RUN_TEST(fastPartialSort_middleBranchAllEqualScores_orderedByPos);
    RUN_TEST(movePickerMain_listSizesAcrossSortBranches_sequenceUnchangedVsBaseline);
    RUN_TEST(radixSortByScore_emptyOrSingle_isNoop);
    RUN_TEST(radixSortByScore_int16Extremes_orderedByScoreDescThenPosAsc);
    RUN_TEST(radixSortByScore_allEqualScores_orderedByPosAscending);
    RUN_TEST(radixSortByScore_unionBits_preservedPerMove);
    RUN_TEST(radixSortByScore_singleDigitKeys_skippedPassesStillSorted);
    RUN_TEST(radixSortByScore_randomLists_matchStdSortWithScoreComparator);

    std::printf("\n== PERF-02 insertion branch ==\n");
    RUN_TEST(fastPartialSort_insertionBranchDuplicatePos_matchesFrozenReference);
#ifdef USE_AVX512
    RUN_TEST(insertionBranchByScore_emptyOrSingle_isNoop);
    RUN_TEST(insertionBranchByScore_negativeFirstMove_keptLastInPrefix);
    RUN_TEST(insertionBranchByScore_negativeTail_isFifoQueueOrder);
    RUN_TEST(insertionBranchByScore_duplicatePos_stableByArrival);
    RUN_TEST(insertionBranchByScore_unionBits_preservedPerMove);
    RUN_TEST(insertionBranchByScore_randomLists_matchFrozenReference);
    RUN_TEST(insertionBranchByScore_nonZeroLimit_matchesFrozenReference);
#else
    std::printf("    (insertionBranchByScore cases skipped: not an AVX-512 build)\n");
#endif
}
