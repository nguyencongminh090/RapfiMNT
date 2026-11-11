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

#include "../../core/pos.h"
#include "../../core/types.h"
#include "../../game/board.h"
#include "../searchcommon.h"
#include "../searchoutput.h"
#include "../searchthread.h"
#include "../timecontrol.h"
#include "../vcn.h"
#include "membudget.h"
#include "node.h"
#include "nodetable.h"
#include "reclaim.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>

namespace Search::MCTS {

/// VCN level of the node at `board` (VCN-03 slice 2). Every defender pass since the search root
/// raises the level by one, capped at VC5, so the level is a function of the board and needs no
/// Node field: level = min(VC5, rootLevel + passes(defender) - rootDefenderPasses).
/// Only meaningful when VCN mode is enabled.
inline VCNLevel mctsNodeVcnLevel(const Board         &board,
                                 const SearchOptions &options,
                                 int                  rootDefenderPasses)
{
    assert(options.vcnMode.enabled());
    const int newPasses = board.passMoveCountOfSide(~options.vcnMode.attacker) - rootDefenderPasses;
    assert(newPasses >= 0);
    const int level = static_cast<int>(options.vcnMode.n) + newPasses;
    return static_cast<VCNLevel>(std::min(level, static_cast<int>(VCNLevel::VC5)));
}

/// Key of the MCTS graph node at `board`: the zobrist key, XORed with the VCN level/attacker hash
/// in VCN mode so VCN nodes never mix with ordinary nodes, other levels or the other attacker.
/// Equal to board.zobristKey() when VCN is off.
inline HashKey mctsNodeKey(const Board &board, const SearchOptions &options, int rootDefenderPasses)
{
    if (!options.vcnMode.enabled())
        return board.zobristKey();
    return board.zobristKey()
           ^ Vcn::hashXor(options, mctsNodeVcnLevel(board, options, rootDefenderPasses));
}

class MCTSSearcher : public Searcher
{
public:
    /// The node table for storing and finding all transposition nodes.
    /// Per-game memory: fully cleared only by clear(clearAllMemory=true).
    std::unique_ptr<NodeTable> nodeTable;

    /// Cross-search graph-reuse state. The node table is transposition-aware,
    /// so this is a Monte-Carlo search *graph*, not a tree.
    /// Lifecycle (preserved semantics): clear() nulls root only; globalNodeAge
    /// resets only with clearAllMemory; previousPosition is never reset - a
    /// stale one can skip recycleOldNodes on the first search of a new game
    /// (documented quirk, queued as a behavior-change candidate).
    struct GraphState
    {
        /// The root node of the MCTS search graph
        Node *root = nullptr;
        /// The searched position of last root node
        std::vector<Pos> previousPosition;
        /// The node key of the last root node (VCN-03 slice 2): the same moves searched in
        /// another VCN mode (off, level, attacker) give another root key and a new root.
        HashKey previousRootKey = 0;
        /// The global node age to synchronize the node table
        uint32_t globalNodeAge = 0;
    } graph;

    /// Per-search scratch, reset at the top of searchMain.
    struct SearchScratch
    {
        /// The number of selectable root moves, set by updateRootMovesData().
        uint32_t numSelectableRootMoves;
        /// The last number of nodes that we have printed search outputs
        uint64_t lastOutputNodes;
        /// The last time that we have printed search outputs
        Time lastOutputTime;
        /// Passes made by the VCN defender on the root board (VCN-03 slice 2), the base of the
        /// per-node level in mctsNodeKey(). Written in searchMain before the workers start.
        int rootDefenderPasses;
    } scratch;

    /// Total requested MCTS search-memory budget in KiB, as passed to
    /// setMemoryLimit (0 = no budget set: graph unbounded, VCF TT at its
    /// default maximum). Written only between searches, like TT resizing.
    size_t memoryLimitKB = 0;

    /// Graph memory budget in bytes. Derived from memoryLimitKB by setMemoryLimit, or, when no
    /// limit was set, from the available RAM at the start of each search (SEARCH-03).
    /// May legitimately be zero (tiny budgets); "unbounded" is signalled by
    /// graphBounded == false, never by this field.
    size_t graphMemoryLimit = 0;

    /// Whether graphMemoryLimit applies. Written only between searches.
    bool graphBounded = false;

    /// Whether setMemoryLimit has sized the VCF TT at least once.
    bool memoryLimitApplied = false;

    /// Estimated bytes of live graph memory (nodes + edge arrays +
    /// per-allocation overhead constants). Updated at node insertion, edge
    /// creation, and the recycle sweep; survives clear(false) together with
    /// the node table contents; zeroed by clear(true).
    std::atomic<size_t> graphMemoryUsed {0};

    /// Edge arrays replaced during the current search (SEARCH-10). Still counted in
    /// graphMemoryUsed; freed by freeRetiredEdgeArrays() once no search thread runs.
    RetiredEdgeArrays retiredEdgeArrays;

    /// What one reclaim pass did (SEARCH-16).
    struct ReclaimStats
    {
        /// Nodes with at most this many visits were trimmed (0 = the pass did nothing).
        uint64_t maxVisits = 0;
        /// Nodes turned back into leaves.
        size_t numTrimmed = 0;
        /// Nodes erased because nothing kept reaches them any more.
        size_t numErased = 0;
        /// graphMemoryUsed before and after the pass.
        size_t bytesBefore = 0, bytesAfter = 0;
    };

    /// Runs a job on every thread that takes part in a reclaim pass and waits for all of them.
    using ParallelRun = std::function<void(const SafepointGate::Job &)>;

    /// Stops the search threads at the top of their playout loop for a reclaim pass (SEARCH-16).
    SafepointGate safepoint;

    /// Set when a reclaim pass cannot help the running search any more (it freed too little, or
    /// the turn time left is too short): the graph then stays closed. Reset by searchMain.
    std::atomic<bool> reclaimExhausted {false};

    /// Number of reclaim passes of the last search. Reset by searchMain.
    std::atomic<uint32_t> numReclaimPasses {0};

    /// Measured cost of the last reclaim pass in thread-nanoseconds per byte of graph, used to
    /// tell whether the next pass fits in the turn time. Written only inside a pass.
    double reclaimThreadNsPerByte = 16.0;

    MCTSSearcher();
    ~MCTSSearcher() = default;

    /// Whether the graph has reached the soft limit and must stop adding nodes and edge
    /// arrays (SEARCH-03). Always false for an unbounded graph.
    bool graphClosed() const
    {
        return graphInsertionClosed(graphBounded,
                                    graphMemoryUsed.load(std::memory_order_relaxed),
                                    graphMemoryLimit);
    }

    std::unique_ptr<SearchData> makeSearchData(SearchThread &th) override { return nullptr; }

    /// Set the memory size limit of the search.
    void setMemoryLimit(size_t memorySizeKB) override;

    /// Get the current memory size limit of the search.
    size_t getMemoryLimit() const override;

    /// Clear the state of the searcher between two different games
    void clear(SearchEngine &pool, bool clearAllMemory) override;

    /// The algorithm middle of one search session, called by the session
    /// driver on the main search thread. Launches the worker threads and
    /// returns the top-ranked root move (or nullptr on early-outs).
    const RootMove *searchMain(SearchThread &th) override;

    /// The main best first search loop. It calls search() repeatedly until the stop
    /// condition is reached. Results are updated to thread bounded with the board.
    void search(SearchThread &th) override;

    /// Frees the retired edge arrays and subtracts their footprint from graphMemoryUsed.
    /// Only call when no search thread runs: after the workers of searchMain have returned, in
    /// clear(true), or in the destructor (SEARCH-10).
    void freeRetiredEdgeArrays();

    /// Checks if current search reaches timeup condition.
    bool checkTimeupCondition(const TimeControl &timectl) override;

    /// One reclaim pass (SEARCH-16): turns low-visit nodes back into leaves and erases the nodes
    /// that nothing kept reaches any more, until about targetBytes of graph memory are left.
    /// The root, terminal nodes and nodes with a proven bound keep their edges; no edge array of
    /// a kept node is written. Also frees the retired edge arrays.
    /// Only call while no thread searches the graph (the callers of `run` included).
    /// @param targetBytes The graph size to shrink to; nothing happens at or below it.
    /// @param run Runs each phase on the participating threads.
    ReclaimStats reclaimNodes(size_t targetBytes, const ParallelRun &run);

private:
    /// Setup root node for the search
    void setupRootNode(SearchThread &th);

    /// Garbage collect all old nodes in the node table
    void recycleOldNodes(SearchThread &th);

    /// Called by a search thread at its safe point when the graph has closed: stops the other
    /// search threads and runs a reclaim pass, or parks when another thread is doing that.
    void tryReclaim(SearchThread &th);

    /// Rank the root moves and update PV, then print all root moves
    void updateRootMovesData(SearchThread &th);
};

}  // namespace Search::MCTS
