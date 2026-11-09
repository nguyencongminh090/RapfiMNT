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
#include "../movepick.h"
#include "parameter.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace Search::MCTS {

class Node;
class NodeTable;

/// An edge in the MCTS graph.
/// It contains the move, policy and num visits of this edge.
/// Layout (SEARCH-08): two 64-bit words, 16 bytes in total.
///   visitWord: bits 0..47 = edge visits, bits 48..63 = quantized policy.
///   childWord: bits 0..47 = child node pointer, bits 48..63 = move (Pos as int16).
/// Visits never carry into the policy bits: the search stops at SearchVisitGuard (2^46).
class Edge
{
public:
    Edge(Pos move, float p)
        : visitWord(uint64_t(quantizePolicy(p)) << VisitBits)
        , childWord(uint64_t(static_cast<uint16_t>(static_cast<int>(move))) << PointerBits)
    {}

    /// Get the move of this edge.
    /// The move bits are written once by the constructor, before the edge array is published.
    Pos getMove() const
    {
        uint64_t word = childWord.load(std::memory_order_relaxed);
        return Pos(static_cast<int16_t>(static_cast<uint16_t>(word >> PointerBits)));
    }

    /// Get the normalized policy of this edge.
    float getP() const
    {
        uint16_t policy = static_cast<uint16_t>(visitWord.load(std::memory_order_relaxed) >> VisitBits);
        uint32_t bits   = (static_cast<uint32_t>(policy) << 12) | (3 << 28);
        float    p;  // TODO: change to std::bit_cast in C++20
        std::memcpy(&p, &bits, sizeof(uint32_t));
        return p;
    }

    /// Get the number of edge visits of this edge.
    uint64_t getVisits() const { return visitWord.load(std::memory_order_acquire) & VisitMask; }

    /// Increment the number of edge visits by delta atomically.
    /// @param delta The number of edge visits to change.
    /// @return The number of edge visits before the change.
    uint64_t addVisits(uint64_t delta)
    {
        return visitWord.fetch_add(delta, std::memory_order_acq_rel) & VisitMask;
    }

    /// Get the child node of this edge.
    /// @note when `getVisits() > 0`, the child node should be guaranteed to be non-null.
    Node *getChild() const
    {
        uint64_t word = childWord.load(std::memory_order_acquire);
        return reinterpret_cast<Node *>(static_cast<uintptr_t>(word & PointerMask));
    }

    /// Set the child node of this edge, keeping the move bits.
    /// Concurrent callers store the same pointer (one node per hash in the node table).
    void setChild(Node *node)
    {
        uint64_t ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node));
        if (ptr & ~PointerMask)
            pointerTooWide(node);
        uint64_t moveBits = childWord.load(std::memory_order_relaxed) & ~PointerMask;
        childWord.store(moveBits | ptr, std::memory_order_release);
    }

    /// Number of low bits of the visit word that hold the visit count.
    static constexpr int      VisitBits = 48;
    static constexpr uint64_t VisitMask = (uint64_t(1) << VisitBits) - 1;
    /// Number of low bits of the child word that hold the child pointer.
    static constexpr int      PointerBits = 48;
    static constexpr uint64_t PointerMask = (uint64_t(1) << PointerBits) - 1;

private:
    /// Converts a float32 policy in [0, 1] to a quantized 16bit policy.
    /// Constructor-only: after the edge array is published, other threads fetch_add the word
    /// that holds the policy bits.
    /// @see
    /// https://github.com/LeelaChessZero/lc0/blob/51f93b7c49720ee100d24aac54193a88ba98219a/src/mcts/node.cc#L130-L167
    static uint16_t quantizePolicy(float p)
    {
        assert(0.0f <= p && p <= 1.0f);
        constexpr int32_t roundings = (1 << 11) - (3 << 28);
        int32_t           bits;  // TODO: change to std::bit_cast in C++20
        std::memcpy(&bits, &p, sizeof(float));
        bits += roundings;
        return (bits < 0) ? 0 : static_cast<uint16_t>(bits >> 12);
    }

    /// Logs and aborts: the node pointer does not fit in 48 bits (see the SEARCH-08 audit).
    [[noreturn]] static void pointerTooWide(const Node *node);

    /// Edge visits (bits 0..47) and quantized policy (bits 48..63).
    std::atomic<uint64_t> visitWord;

    /// Child node pointer (bits 0..47) and move (bits 48..63).
    std::atomic<uint64_t> childWord;
};

static_assert(sizeof(Edge) == 16, "Edge must stay 16 bytes (edge-array memory budget, SEARCH-08)");

/// Represents an allocated array of edges.
struct alignas(alignof(Edge)) EdgeArray
{
    uint32_t numEdges;
    /// Number of generated moves before the policy-mass cut (SEARCH-09). Lives in the padding
    /// before edges[]; written once before the array is published, immutable afterwards.
    uint32_t numMoves;
    Edge     edges[];

    /// Whether some generated moves were cut and have no edge (SEARCH-02/05 policy-mass cut).
    bool edgesCut() const { return numEdges < numMoves; }

    /// Get the edge reference at the given index.
    Edge &operator[](uint32_t index)
    {
        assert(index < numEdges);
        return edges[index];
    }

    /// Get the const edge reference at the given index.
    const Edge &operator[](uint32_t index) const
    {
        assert(index < numEdges);
        return edges[index];
    }
};

static_assert(alignof(EdgeArray) == alignof(Edge), "Sanity check on EdgeArray's alignment");
static_assert(sizeof(EdgeArray) == 8, "EdgeArray header must stay 8 bytes (allocation math, SEARCH-09)");
static_assert(sizeof(Edge) % sizeof(EdgeArray) == 0, "Sanity check on Edge's size");

/// Edge arrays replaced by Node::replaceEdgesWithFull (SEARCH-10). A search thread may still hold
/// an `EdgeArray &` or `Edge *` into a replaced array, so it stays allocated (and counted in the
/// graph memory) until a quiescent point where no search thread runs: see
/// MCTSSearcher::freeRetiredEdgeArrays(). retire() is thread-safe; freeAll() and the destructor
/// must only run when no search thread can read the retired arrays.
class RetiredEdgeArrays
{
public:
    RetiredEdgeArrays() = default;
    ~RetiredEdgeArrays() { freeAll(); }

    RetiredEdgeArrays(const RetiredEdgeArrays &)            = delete;
    RetiredEdgeArrays &operator=(const RetiredEdgeArrays &) = delete;

    /// Takes ownership of an edge array that is no longer reachable from its node.
    void retire(EdgeArray *edgeArray);

    /// Frees every retired array.
    /// @return The sum of edgeArrayFootprint() of the freed arrays.
    size_t freeAll();

    /// Number of arrays currently retired.
    size_t count() const;

    /// Sum of edgeArrayFootprint() of the arrays currently retired.
    size_t bytes() const;

private:
    mutable std::mutex       mutex;
    std::vector<EdgeArray *> arrays;
    size_t                   totalBytes = 0;
};

/// Limits on how many of the generated moves become edges of a node (SEARCH-02).
/// The default keeps every move.
struct EdgeLimits
{
    /// Keep the leading edges until their cumulative policy reaches this mass.
    /// A value >= 1 disables the cut.
    float massKeep = 1.0f;
    /// Never keep fewer than this many edges (at least 1 when there is any move).
    uint32_t minEdges = 8;
};

/// Number of leading moves to keep as edges. Policies must be sorted in descending order.
/// @param policy Normalised policies of the generated moves.
/// @param numMoves Number of generated moves.
/// @param massKeep Cumulative policy mass to reach; >= 1 keeps every move.
/// @param minEdges Lower bound of the result (clamped to [1, numMoves]).
uint32_t selectEdgeCount(const float *policy, uint32_t numMoves, float massKeep, uint32_t minEdges);

/// Whether a leaf that has been visited `visits` times may be expanded.
/// A non-positive minVisits means no delay.
inline bool readyToExpand(uint64_t visits, int minVisits)
{
    return minVisits <= 1 || visits >= static_cast<uint64_t>(minVisits);
}

/// Represents the value bound of a node.
struct ValueBound
{
    Eval lower, upper;

    ValueBound() : lower(-VALUE_INFINITE), upper(VALUE_INFINITE) {};
    ValueBound(Value terminalValue) : lower(terminalValue), upper(terminalValue) {}

    /// Returns whether this bound is terminal.
    bool isTerminal() const { return lower == upper; }
    /// Returns the lower bound of this child node's bound.
    Value childLowerBound() const { return static_cast<Value>(-upper); }
    /// Returns the upper bound of this child node's bound.
    Value childUpperBound() const { return static_cast<Value>(-lower); }
    /// Add a child node's bound to this parent node's bound.
    ValueBound &operator|=(ValueBound childBound)
    {
        lower = std::max<Eval>(lower, -childBound.upper);
        upper = std::max<Eval>(upper, -childBound.lower);
        return *this;
    }
};

static_assert(std::atomic<ValueBound>::is_always_lock_free,
              "std::atomic<ValueBound> should be a lock free atomic variable");

/// A node in the MCTS graph.
/// It contains the edges, children, and statistics of this node.
class Node
{
public:
    /// Constructs a new unevaluated node with no children edges.
    /// @param hash The graph hash key of this node.
    /// @param age The initial age of this node.
    explicit Node(HashKey hash, uint32_t age);
    ~Node();

    // Disallow copy and move. We need node's address to have pointer stability.
    Node(const Node &rhs)            = delete;
    Node &operator=(const Node &rhs) = delete;
    Node(Node &&rhs)                 = delete;
    Node &operator=(Node &&rhs)      = delete;

    /// Set this node to be a terminal node and set num visits to 1.
    /// Must only be called by the evaluating thread on a node that has not
    /// been published yet (num visits is still zero); use tryMarkTerminal()
    /// for a node that may already have accumulated visits.
    /// @param value The terminal value of this node.
    void setTerminal(Value value);

    /// One-shot leaf-to-terminal transition for an already-published node.
    /// Several threads may discover concurrently that this node has no valid
    /// move; only the thread that wins the claim on the terminal value writes
    /// the terminal statistics, and the visit count is left untouched so that
    /// visits accumulated by concurrent threads are preserved.
    /// @param value The terminal value of this node.
    /// @return Whether this thread won the transition.
    bool tryMarkTerminal(Value value);

    /// Set this node to be a non-terminal node and set num visits to 1.
    /// @param utility The raw utility value of this node.
    /// @param drawRate The raw draw probability of this node.
    void setNonTerminal(float utility, float drawRate);

    /// Initializes the edges of this node from the given move picker.
    /// @param movePicker The move picker to generate the edges.
    /// @param allocCounter If non-null, edgeArrayFootprint(numEdges) is added
    ///   to it when this call wins the edge-array allocation race (a losing
    ///   concurrent expansion adds nothing).
    /// @return Whether this node has no valid edges. If true,
    ///   this node is a terminal node that has been mated.
    bool createEdges(MovePicker &movePicker,
                     std::atomic<size_t> *allocCounter = nullptr,
                     const EdgeLimits    &limits       = {});

    /// Initializes the edges from an already generated move list sorted by descending policy.
    /// The kept prefix is chosen by selectEdgeCount() and renormalised to sum to 1 when
    /// anything was cut. Same return value and allocCounter contract as createEdges().
    bool createEdgesFrom(const Pos          *moves,
                         const float        *policy,
                         uint32_t            numMoves,
                         const EdgeLimits   &limits,
                         std::atomic<size_t> *allocCounter = nullptr);

    /// Replaces a cut edge array with one that holds every generated move (SEARCH-10).
    /// The new array keeps the old edges first, in their old order, with their visits and child
    /// pointers copied, then appends the moves the old array cut, unvisited, in the given order.
    /// Every edge takes its policy from the given full list. The new array is built completely
    /// before it is published with a CAS against expectedOld; afterwards, visits added to the old
    /// edges in the meantime are carried over.
    /// @param expectedOld The edge array the caller read and wants replaced.
    /// @param moves, policy The full move list of this node, sorted by descending policy.
    /// @param numMoves Number of moves in the full list.
    /// @param allocCounter If non-null, edgeArrayFootprint(numMoves) is added on success.
    /// @param retired The old array goes here on success; its footprint stays counted until freed.
    /// @return Whether this call replaced the array. False when another thread replaced it first
    ///   or when an old edge's move is missing from the full list or is unvisited; nothing
    ///   changes then.
    bool replaceEdgesWithFull(EdgeArray           *expectedOld,
                              const Pos           *moves,
                              const float         *policy,
                              uint32_t             numMoves,
                              std::atomic<size_t> *allocCounter,
                              RetiredEdgeArrays   &retired);

    /// Returns the graph hash key of this node.
    HashKey getHash() const { return hash; }

    /// Returns whether this node has no children.
    /// Acquire pairs with the release CAS in createEdges() so that a non-null
    /// edge array is always observed fully constructed before being dereferenced.
    bool isLeaf() const { return edges.load(std::memory_order_acquire) == nullptr; }

    /// Returns the edge array at the given index.
    /// If this node has no edges, returns nullptr.
    EdgeArray       *getEdges() { return edges.load(std::memory_order_acquire); }
    const EdgeArray *getEdges() const { return edges.load(std::memory_order_acquire); }

    /// Returns the average utility value of this node.
    float getQ() const { return q.load(std::memory_order_relaxed); }

    /// Returns the estimated sample variance of utility value.
    float getQVar(float priorVar = 1.0f, float priorWeight = 1.0f) const;

    /// Returns the average draw rate of this node.
    float getD() const { return d.load(std::memory_order_relaxed); }

    /// Returns the total visits of this node.
    uint64_t getVisits() const { return visitWord.load(std::memory_order_acquire) & VisitMask; }

    /// Returns the total virtual visits of this node.
    uint32_t getVirtualVisits() const { return nVirtual.load(std::memory_order_acquire); }

    /// Returns the reference to the age of this node.
    std::atomic<uint32_t> &getAgeRef() { return age; }

    /// Returns the evaluated utility value of this node.
    float getEvalUtility() const { return utility; }

    /// Returns the evaluated draw rate of this node.
    float getEvalDrawRate() const { return drawRate; }

    /// Returns the propogated value bound of this node.
    ValueBound getBound() const { return bound.load(std::memory_order_relaxed); }

    /// Returns if this node is terminal.
    bool isTerminal() const
    {
        return wordTerminalValue(visitWord.load(std::memory_order_relaxed)) != VALUE_NONE;
    }

    /// Update the average utility and the average draw rate from childrens.
    /// @return Whether the edge list was cut and every kept child is a proven win for the
    ///   opponent, so only a cut move can still save this node: the caller should replace the
    ///   edges with the full move list (SEARCH-10). Callers may ignore it.
    bool updateStats();

    /// Begin the visit of this node.
    void beginVisit(uint32_t newVisits)
    {
        nVirtual.fetch_add(newVisits, std::memory_order_acq_rel);
    }

    /// Finish the visit of this node by incrementing the total visits of this node.
    void finishVisit(uint32_t newVisits, uint32_t actualNewVisits)
    {
        if (actualNewVisits)
            visitWord.fetch_add(actualNewVisits, std::memory_order_acq_rel);
        nVirtual.fetch_add(-newVisits, std::memory_order_release);
    }

    /// Directly increment the total visits of this node by delta.
    /// Usually used for visiting a terminal node.
    void incrementVisits(uint64_t delta)
    {
        visitWord.fetch_add(delta, std::memory_order_acq_rel);
    }

    /// Number of low bits of the visit word that hold the visit count (SEARCH-08).
    static constexpr int      VisitBits = 48;
    static constexpr uint64_t VisitMask = (uint64_t(1) << VisitBits) - 1;

private:
    /// The terminal-value tag (bits 48..63) of a visit word.
    static uint64_t terminalTag(Value value)
    {
        return uint64_t(static_cast<uint16_t>(static_cast<Eval>(value))) << VisitBits;
    }
    /// The terminal value stored in a visit word (VALUE_NONE for a non-terminal node).
    static Value wordTerminalValue(uint64_t word)
    {
        return static_cast<Value>(static_cast<Eval>(static_cast<uint16_t>(word >> VisitBits)));
    }

    /// The graph hash of this node.
    const HashKey hash;

    /// The edge array of this node, edges are sorted by normalized policy.
    std::atomic<EdgeArray *> edges;

    /// Bits 0..47: total visits under this node's subgraph.
    /// For leaf node, this indicates if the node's value has been evaluated.
    /// For non-leaf node, this is the sum of all children's edge visits plus 1.
    /// Bits 48..63: for terminal node, the theoretical value (from current side to move),
    /// including the game ply to mate/mated; VALUE_NONE (as int16) for a non-terminal node.
    /// After the node has been published, the tag transitions at most once from VALUE_NONE to a
    /// terminal value (see tryMarkTerminal()). Sharing one word keeps sizeof(Node) at 56 bytes
    /// (SEARCH-08). Visits never carry into the tag: the search stops at SearchVisitGuard (2^46).
    std::atomic<uint64_t> visitWord;

    /// Total started but not finished visits under this node's subgraph,
    /// mainly for computing virtual loss when multi-threading is used.
    std::atomic<uint32_t> nVirtual;

    /// Average utility (from current side to move) of this node, in [-1,1].
    std::atomic<float> q;

    /// Average squared utility of this node, for computing utility variance.
    std::atomic<float> qSqr;

    /// Average draw rate of this node in [0,1]. Not flipped when changing side.
    std::atomic<float> d;

    /// The age of this node, used to find and recycle unused nodes.
    std::atomic<uint32_t> age;

    /// The propogated terminal value bound of this node.
    std::atomic<ValueBound> bound;

    /// For non-terminal node, this stores the node's raw utility value in [-1,1].
    /// Higher values means better position from current side to move.
    float utility;

    /// For non-terminal node, this stores the node's raw draw probability in [0,1].
    /// (from current side to move).
    float drawRate;

    /// Write the terminal statistics (utility, draw rate, mean value stats
    /// and bound) derived from the given terminal value. Does not touch the
    /// visit count or terminalValue itself.
    void writeTerminalStats(Value value);
};

static_assert(sizeof(void *) != 8 || sizeof(Node) == 56,
              "Node must stay 56 bytes on 64-bit targets (std::set node size class, SEARCH-08)");

/// Estimated heap footprint in bytes of one node stored in the node table.
inline size_t nodeFootprint()
{
    return sizeof(Node) + NodeTableEntryOverhead;
}

/// Estimated heap footprint in bytes of the EdgeArray allocation made by
/// Node::createEdges for the given number of edges.
inline size_t edgeArrayFootprint(uint32_t numEdges)
{
    return sizeof(EdgeArray) + numEdges * sizeof(Edge) + EdgeArrayAllocOverhead;
}

}  // namespace Search::MCTS
