/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2024  Rapfi developers
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

#include <cstdint>

/// Thin test-only wrappers over helpers that live in search.cpp's anonymous namespace
/// (SEARCH-08 tests). They forward unchanged and add no behaviour.
namespace Search::MCTS::Testing {

/// Forwards to cpuctExplorationFactor() in search.cpp.
float cpuctExplorationFactor(uint64_t parentVisits);

/// Forwards to puctSelectionValue() in search.cpp.
float puctSelectionValue(float    childUtility,
                         float    childDraw,
                         float    parentDraw,
                         float    childPolicy,
                         uint64_t childVisits,
                         uint32_t childVirtualVisits,
                         float    cpuctExploration);

}  // namespace Search::MCTS::Testing
