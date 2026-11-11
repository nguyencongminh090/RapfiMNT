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

#include "../../core/types.h"
#include "../searchcommon.h"

#include <cstdint>

class Board;
struct Pos;

namespace Search::MCTS {
class Node;
}

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

/// Forwards to generateNodeMoves() in search.cpp: the move list (and normalised policies) a
/// non-root node gets from expandNode<false> and widenNode, before the edge cut (VCN-03 slice 3).
/// The board must be bound to a search thread with an evaluator. Returns the number of moves.
uint32_t generateNodeMoves(const Board         &board,
                           const SearchOptions &options,
                           Pos                 *moveList,
                           float               *policyList);

/// Forwards to evaluateNode<false>() in search.cpp: the first evaluation of a non-root node,
/// terminal rules included (VCN-03 slice 4). In VCN mode the node level is read from
/// scratch.rootDefenderPasses of the engine's MCTS searcher. The board must be bound to a search
/// thread (with an evaluator if the node can fall through to the network value).
void evaluateNode(Node &node, const SearchOptions &options, Board &board, int ply);

/// Runs SimpleVCF::vcf() in search.cpp on `board` with the VCN context of `level` for the
/// attacker in `options` (VCNLevel::None, or VCN off in `options`: the plain VCF, as for an
/// ordinary search). Returns the VCF value (VALUE_ZERO when nothing is found by the plain VCF).
Value simpleVcf(Board &board, const SearchOptions &options, VCNLevel level, int ply);

/// Answer of the VCN pass test (VCN-04): "after a null move by the defender, does the attacker win
/// one level up?". Only NoThreat ends a node; Threat and Unknown leave it an ordinary node.
enum class PassTestResult : uint8_t {
    Unknown,   ///< Not decided (no budget, budget exhausted, or outside the static rule)
    Threat,    ///< Proven: the attacker wins at the next level
    NoThreat,  ///< Proven: the attacker does not win at the next level
};

/// The static necessary condition of the VC2 pass test (VCN-04 stage 1) for the attacker in
/// `options`: NoThreat when the attacker has no cell that makes a three or a four, else Unknown.
/// Reads the board only; the side to move does not matter.
PassTestResult vcnPassTestStatic(const Board &board, const SearchOptions &options);

/// The bounded VCT solver of the VC2 pass test (VCN-04 stage 2): does the attacker in `options`,
/// to move on `board` at level VC3, have a forced win? `nodeBudget` is the work the solver may
/// do: stones it places plus nodes of the VCF searches it runs (0: Unknown at once). `nodesUsed`,
/// when given, receives the work done; it can pass the budget by one VCF search.
/// The board must be bound to a search thread; it is restored on return.
PassTestResult vctSolve(Board               &board,
                        const SearchOptions &options,
                        uint32_t             nodeBudget,
                        uint32_t            *nodesUsed = nullptr);

/// The attacker moves the solver searches at `board` (attacker to move, level VC3): every legal
/// move that is a four or after which the exact VC4 VCF exists once the defender passes. Written
/// to `moveList` (room for MAX_MOVES); returns the count. The board is restored on return.
uint32_t vctAdmissibleMoves(Board &board, const SearchOptions &options, Pos *moveList);

/// The TT key under which vctSolve caches the proven answer of `board` (attacker to move).
HashKey vctCacheKey(const Board &board, const SearchOptions &options);

}  // namespace Search::MCTS::Testing
