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

#include <cstddef>
#include <cstdint>

namespace Search::MCTS {

constexpr float MaxNewVisitsProp = 0.36f;

constexpr float CpuctExploration     = 0.35f;
constexpr float CpuctExplorationLog  = 1.02f;
constexpr float CpuctExplorationBase = 328;

constexpr float CpuctUtilityStdevScale     = 0.05f;
constexpr float CpuctUtilityVarPrior       = 0.15f;
constexpr float CpuctUtilityVarPriorWeight = 1.80f;

constexpr float FpuReductionMax     = 0.055f;
constexpr float FpuLossProp         = 0.0008f;
constexpr float RootFpuReductionMax = 0.07f;
constexpr float RootFpuLossProp     = 0.003f;
constexpr float FpuUtilityBlendPow  = 0.75f;

constexpr uint32_t MinTranspositionSkipVisits = 11;

/// Root visit count at which the MCTS search stops (SEARCH-08). Visit counters keep 48 bits in a
/// packed word whose top 16 bits hold the policy (Edge) or the terminal value (Node), so this
/// guard stays far below 2^48 (about 136 days at 6M visits/s).
constexpr uint64_t SearchVisitGuard = uint64_t(1) << 46;

constexpr bool  UseLCBForBestmoveSelection = true;
constexpr float LCBStdevs                  = 6.28f;
constexpr float LCBMinVisitProp            = 0.1f;

/// Intended policy softmax temperatures. NOTE: currently ignored by MovePicker
/// (see the normalizedPolicyTemp bug note in movepick.h) - kept until wired.
constexpr float PolicyTemperature     = 0.90f;
constexpr float RootPolicyTemperature = 1.05f;

/// Per-node heap overhead of the NodeTable beyond sizeof(Node), measured for the PERF-03 table
/// (8-byte tagged slots at load factor 3/8..3/4, plus slab slack and slab headers): the heap
/// grows by 74.9 B per node at 511683 nodes in 1024 shards (mallinfo2, glibc 2.43), so 75 - 56.
/// It moves between about 70 and 81 B with the load factor and the shard size (PERF-03 todo).
constexpr size_t NodeTableEntryOverhead = 19;

/// Estimated allocator header overhead of one EdgeArray heap allocation.
constexpr size_t EdgeArrayAllocOverhead = 16;

}  // namespace Search::MCTS
