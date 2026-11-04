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

#include "../core/pos.h"
#include "../game/movegen.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>

namespace Search {

/// Sort `n` moves best-first under ScoredMove::ScoreComparator (score descending, then pos
/// ascending) with an LSD radix sort on a packed integer key. Every byte of each ScoredMove,
/// including the rawScore/policy union, travels with its move. On a list of distinct `pos` the
/// result is byte-identical to std::sort with ScoreComparator. Requires n <= MAX_MOVES; n <= 1
/// is a no-op. Defined NO_INLINE in movepick.cpp so its stack arrays stay out of search frames.
void radixSortByScore(ScoredMove *begin, size_t n);

/// Partial sort the move list up to the score limit. It dynamiclly decides
/// which sorting algorithm to use based on how many moves are in the list.
template <typename Comparator>
void fastPartialSort(ScoredMove *begin, ScoredMove *end, Score limit, Comparator comp)
{
    // heuristic values
    constexpr size_t InsertionSortLimit = MAX_MOVES / 4;
    constexpr size_t SortLimit          = MAX_MOVES * 2 / 3;

    size_t nMoves = end - begin;
    if (nMoves <= InsertionSortLimit) {
        // Sorts moves in descending order up to and including a given limit.
        // The order of moves smaller than the limit is left unspecified.
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
        // PERF-01: same unique order as std::sort (strict total order on distinct pos), but
        // without introsort's mispredicted partition branches. PolicyComparator keeps std::sort:
        // its float key (+0/-0, NaN) has no equivalent integer packing.
        if constexpr (std::is_same_v<Comparator, ScoredMove::ScoreComparator>)
            radixSortByScore(begin, nMoves);
        else
            std::sort(begin, end, comp);
    }
    else {
        std::partial_sort(begin, begin + SortLimit, end, comp);
    }
}

}  // namespace Search
