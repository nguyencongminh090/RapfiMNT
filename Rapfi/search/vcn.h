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

// VCN helpers shared by the alpha-beta and MCTS searchers. Moved unchanged out of the anonymous
// namespace of search/ab/search.cpp (VCN-03 slice 2).

#pragma once

#include "../core/hash.h"
#include "../core/types.h"
#include "../game/board.h"
#include "searchcommon.h"

namespace Search {

/// VCN (Victory by Continuous N-level attack) helpers.
namespace Vcn {

/// Move the attacker one level up (after a defender pass), capped at VC5.
inline VCNLevel levelIncrement(VCNLevel level)
{
    return level < VCNLevel::VC5 ? static_cast<VCNLevel>(static_cast<int>(level) + 1)
                                 : VCNLevel::VC5;
}

/// Level of the node reached by playing `move` from a node of level `level`. Only a pass played
/// by the defender raises the level (the attacker never passes).
inline VCNLevel childLevel(VCNLevel level, bool selfIsAttacker, Pos move)
{
    return !selfIsAttacker && move == Pos::PASS ? levelIncrement(level) : level;
}

/// At VC5 the attacker must already have a five on the board, otherwise they lose at once.
inline bool vc5AttackerLoses(const Board &board, Color attacker)
{
    return board.p4Count(attacker, A_FIVE) == 0;
}

/// Hash XORed into TT keys so VCN results never mix with ordinary results or with other levels
/// (the same board position can be searched at different levels). Zero when VCN is off.
inline HashKey hashXor(const SearchOptions &opts, VCNLevel level)
{
    if (!opts.vcnMode.enabled())
        return 0;
    return Hash::LCHash(static_cast<uint64_t>(static_cast<int>(level))
                        ^ ((static_cast<uint64_t>(opts.vcnMode.attacker) + 1) << 32));
}

}  // namespace Vcn

}  // namespace Search
