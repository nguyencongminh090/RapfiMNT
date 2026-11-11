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

#include "node.h"

#include "../../core/iohelper.h"
#include "../../eval/scoretables.h"
#include "nodetable.h"

#include <algorithm>
#include <cstdlib>
#include <type_traits>

namespace Search::MCTS {

namespace {

/// Raw storage type backing every EdgeArray allocation. The flexible-length
/// edge array is allocated as an array of this type, so all deallocation
/// paths must free it as an array of this same type.
using EdgeArrayAlloc = std::aligned_storage_t<sizeof(EdgeArray), alignof(EdgeArray)>;

}  // namespace

void Edge::pointerTooWide(const Node *node)
{
    ERRORL("MCTS: node pointer " << static_cast<const void *>(node)
                                 << " does not fit in 48 bits; packed Edge cannot store it.");
    std::abort();
}

Node::Node(HashKey hash, uint32_t age)
    : hash(hash)
    , edges(nullptr)
    , visitWord(terminalTag(VALUE_NONE))
    , nVirtual(0)
    , q(1.0f)  // Default as a losing node for the parent's side of view
    , qSqr(1.0f)
    , d(0.0f)
    , age(age)
    , bound()
{}

Node::~Node()
{
    EdgeArray *edgeArray = edges.exchange(nullptr, std::memory_order_relaxed);
    if (edgeArray)
        delete[] reinterpret_cast<EdgeArrayAlloc *>(edgeArray);
}

size_t Node::releaseEdges()
{
    EdgeArray *edgeArray = edges.exchange(nullptr, std::memory_order_acq_rel);
    if (!edgeArray)
        return 0;
    size_t footprint = edgeArrayFootprint(edgeArray->numEdges);
    delete[] reinterpret_cast<EdgeArrayAlloc *>(edgeArray);
    return footprint;
}

void Node::writeTerminalStats(Value value)
{
    if (value >= VALUE_MATE_IN_MAX_PLY) {
        utility  = 1.0;
        drawRate = 0.0f;
    }
    else if (value <= VALUE_MATED_IN_MAX_PLY) {
        utility  = -1.0;
        drawRate = 0.0f;
    }
    else {
        utility  = Evaluation::valueToWinRate(value) * 2 - 1.0f;
        drawRate = 1.0f;
    }

    assert(utility >= -1.0f && utility <= 1.0f);
    assert(drawRate >= 0.0f && drawRate <= 1.0f);

    q.store(utility, std::memory_order_relaxed);
    qSqr.store(utility * utility, std::memory_order_relaxed);
    d.store(drawRate, std::memory_order_relaxed);
    bound.store(ValueBound {value}, std::memory_order_relaxed);
}

void Node::setTerminal(Value value)
{
    assert(value != VALUE_NONE);
    assert(-VALUE_INFINITE <= value && value <= VALUE_INFINITE);
    // This publication path is reserved for the single evaluating thread:
    // other threads discard their visits until the release store below.
    assert(getVisits() == 0);
    writeTerminalStats(value);
    visitWord.store(terminalTag(value) | 1, std::memory_order_release);
}

bool Node::tryMarkTerminal(Value value)
{
    assert(value != VALUE_NONE);
    assert(-VALUE_INFINITE <= value && value <= VALUE_INFINITE);

    // Invariant: after publication, terminalValue moves away from VALUE_NONE
    // at most once in a node's lifetime. Claiming the transition with this
    // CAS makes the winner the only writer of the terminal statistics, and
    // the visit count is deliberately left untouched so visits accumulated
    // by concurrent playouts are preserved (a reader may transiently observe
    // a terminal node whose mean stats still hold the evaluated values; the
    // parent's next updateStats() recomputes from the terminal stats).
    // The tag shares its word with the visit count, so a concurrent fetch_add can make the CAS
    // fail; retry until the tag is seen as already set or the claim succeeds.
    uint64_t word = visitWord.load(std::memory_order_relaxed);
    do {
        if (wordTerminalValue(word) != VALUE_NONE)
            return false;
    } while (!visitWord.compare_exchange_weak(word,
                                              (word & VisitMask) | terminalTag(value),
                                              std::memory_order_relaxed));

    writeTerminalStats(value);
    return true;
}

void Node::setNonTerminal(float utility, float drawRate)
{
    assert(utility >= -1.0f && utility <= 1.0f);
    assert(drawRate >= 0.0f && drawRate <= 1.0f);

    this->utility  = utility;
    this->drawRate = drawRate;
    q.store(utility, std::memory_order_relaxed);
    qSqr.store(utility * utility, std::memory_order_relaxed);
    d.store(drawRate, std::memory_order_relaxed);
    visitWord.store(terminalTag(VALUE_NONE) | 1, std::memory_order_release);
}

uint32_t selectEdgeCount(const float *policy, uint32_t numMoves, float massKeep, uint32_t minEdges)
{
    if (numMoves == 0)
        return 0;

    uint32_t floorEdges = std::clamp<uint32_t>(minEdges, 1, numMoves);
    if (massKeep >= 1.0f)
        return numMoves;

    float mass = 0.0f;
    for (uint32_t i = 0; i < numMoves; i++) {
        mass += policy[i];
        if (mass >= massKeep)
            return std::max(i + 1, floorEdges);
    }
    return numMoves;
}

bool Node::createEdges(MovePicker &movePicker,
                       std::atomic<size_t> *allocCounter,
                       const EdgeLimits    &limits)
{
    Pos      moveList[MAX_MOVES];
    float    policyList[MAX_MOVES];
    uint32_t numEdges = 0;
    // Moves from the move picker should be sorted
    while (Pos move = movePicker()) {
        assert(movePicker.hasPolicyScore());
        assert(movePicker.hasNormalizedPolicy());
        moveList[numEdges]   = move;
        policyList[numEdges] = movePicker.curMoveNormalizePolicy();
        numEdges++;
    }

    return createEdgesFrom(moveList, policyList, numEdges, limits, allocCounter);
}

bool Node::createEdgesFrom(const Pos          *moveList,
                           const float        *policyList,
                           uint32_t            numMoves,
                           const EdgeLimits   &limits,
                           std::atomic<size_t> *allocCounter)
{
    // If no valid edges, then this node is a (mated) terminal node
    if (numMoves == 0)
        return true;

    uint32_t numEdges = selectEdgeCount(policyList, numMoves, limits.massKeep, limits.minEdges);

    // Renormalise only when moves were cut, so the default path stays bit-identical
    float scale = 1.0f;
    if (numEdges < numMoves) {
        float kept = 0.0f;
        for (uint32_t i = 0; i < numEdges; i++)
            kept += policyList[i];
        if (kept > 0.0f)
            scale = 1.0f / kept;
    }

    size_t     numAllocs = (sizeof(EdgeArray) + numEdges * sizeof(Edge)) / sizeof(EdgeArrayAlloc);
    EdgeArray *tempEdges = reinterpret_cast<EdgeArray *>(new EdgeArrayAlloc[numAllocs]);

    // Copy the move and policy array to the allocated edge array
    tempEdges->numEdges = numEdges;
    tempEdges->numMoves = numMoves;
    for (uint32_t i = 0; i < numEdges; i++)
        new (&tempEdges->edges[i]) Edge(moveList[i], std::min(policyList[i] * scale, 1.0f));

    EdgeArray *expected = nullptr;
    bool       suc = edges.compare_exchange_strong(expected, tempEdges, std::memory_order_release);
    // If we are not the one that sets the edge array, then we need to delete the temp edge array
    if (!suc)
        delete[] reinterpret_cast<EdgeArrayAlloc *>(tempEdges);
    else if (allocCounter)
        allocCounter->fetch_add(edgeArrayFootprint(numEdges), std::memory_order_relaxed);

    return false;
}

float Node::getQVar(float priorVar, float priorWeight) const
{
    uint64_t visits = visitWord.load(std::memory_order_relaxed) & VisitMask;
    if (visits < 2)
        return priorVar;

    float weight        = visits;
    float utilityAvg    = q.load(std::memory_order_relaxed);
    float utilitySqrAvg = qSqr.load(std::memory_order_relaxed);

    // sample_var = (utilitySqrAvg - utilityAvg * utilityAvg) * weight / (weight - 1.0f)
    // regula_var = (sample_var * (weight-1) + varPrior * priorWeight) / (weight-1 + priorWeight)
    float sampleVariance = std::max(utilitySqrAvg - utilityAvg * utilityAvg, 0.0f);
    float regularizedVariance =
        (sampleVariance * weight + priorVar * priorWeight) / (weight + priorWeight - 1.0f);
    return regularizedVariance;
}

bool Node::updateStats()
{
    // Concurrency note: child visit counts and values are read with relaxed
    // atomics while other threads may be updating them. The aggregation is
    // statistically tolerant by design - a slightly stale or torn-across-
    // children snapshot only perturbs this node's running averages, which the
    // next updateStats() call recomputes from scratch.
    const EdgeArray *edgeArray = getEdges();
    // If this node is not expanded, then we do not need to update any stats
    if (!edgeArray)
        return false;

    uint64_t   nSum     = 1;
    float      qSum     = utility;
    float      qSqrSum  = utility * utility;
    float      dSum     = drawRate;
    ValueBound maxBound = ValueBound {-VALUE_INFINITE};
    for (uint32_t i = 0; i < edgeArray->numEdges; i++) {
        const Edge &edge   = (*edgeArray)[i];
        uint64_t    childN = edge.getVisits();

        // No need to read from child node if it has zero edge visits
        if (childN == 0) {
            // All unvisited children all have uninitialized bound (-inf, inf)
            maxBound |= ValueBound {};
            break;
        }

        Node *childNode = edge.getChild();
        assert(childNode);  // child node should be guaranteed to be non-null

        float childQ    = childNode->q.load(std::memory_order_relaxed);
        float childQSqr = childNode->qSqr.load(std::memory_order_relaxed);
        float childD    = childNode->d.load(std::memory_order_relaxed);
        nSum += childN;
        qSum += childN * (-childQ);  // Flip side for child's utility
        qSqrSum += childN * childQSqr;
        dSum += childN * childD;
        maxBound |= childNode->bound.load(std::memory_order_relaxed);
    }

    // Every kept child is a proven win for the opponent: only a cut move can still save this
    // node, so the caller should add the cut moves back (SEARCH-10).
    bool widenNeeded = false;

    // Cut moves (SEARCH-09) count as one unexplored child with an unknown bound: the kept
    // children alone can prove a win (lower) but never a loss or a draw cap (upper).
    if (edgeArray->edgesCut()) {
        widenNeeded = maxBound.upper <= VALUE_MATED_IN_MAX_PLY;
        maxBound |= ValueBound {};
    }

    float norm = 1.0f / nSum;
    q.store(qSum * norm, std::memory_order_relaxed);
    qSqr.store(qSqrSum * norm, std::memory_order_relaxed);
    d.store(dSum * norm, std::memory_order_relaxed);

    // Only update bound if this is not a terminal node
    if (isTerminal())
        return false;
    bound.store(maxBound, std::memory_order_relaxed);
    return widenNeeded;
}

bool Node::replaceEdgesWithFull(EdgeArray           *expectedOld,
                                const Pos           *moves,
                                const float         *policy,
                                uint32_t             numMoves,
                                std::atomic<size_t> *allocCounter,
                                RetiredEdgeArrays   &retired)
{
    assert(expectedOld);
    const uint32_t numKept = expectedOld->numEdges;

    // Match every old edge by move in the full list. A missing move should never happen for the
    // same board; if it does, keep the old array.
    int  fullIndexOf[MAX_MOVES];
    bool isKept[MAX_MOVES] = {};
    if (numMoves > MAX_MOVES || numKept > numMoves)
        return false;
    for (uint32_t i = 0; i < numKept; i++) {
        Pos move  = expectedOld->edges[i].getMove();
        int found = -1;
        for (uint32_t j = 0; j < numMoves; j++)
            if (moves[j] == move) {
                found = int(j);
                break;
            }
        if (found < 0 || isKept[found])
            return false;
        fullIndexOf[i] = found;
        isKept[found]  = true;
    }

    size_t     numAllocs = (sizeof(EdgeArray) + numMoves * sizeof(Edge)) / sizeof(EdgeArrayAlloc);
    EdgeArray *tempEdges = reinterpret_cast<EdgeArray *>(new EdgeArrayAlloc[numAllocs]);
    tempEdges->numEdges  = numMoves;
    tempEdges->numMoves  = numMoves;

    // Old edges first, in their old order, so the visited edges stay a prefix. Copy the visits
    // and children into the private array before it is published.
    uint64_t copiedVisits[MAX_MOVES];
    for (uint32_t i = 0; i < numKept; i++) {
        const Edge &oldEdge = expectedOld->edges[i];
        Edge *edge = new (&tempEdges->edges[i]) Edge(oldEdge.getMove(),
                                                     std::min(policy[fullIndexOf[i]], 1.0f));
        // Visits first (acquire): a visited edge had its child set before its first visit.
        copiedVisits[i] = oldEdge.getVisits();
        Node *child     = oldEdge.getChild();
        // Every old edge must be visited (the widen trigger needs every kept child proven), so
        // that the appended unvisited edges keep the visited edges a prefix.
        if (copiedVisits[i] == 0 || !child) {
            delete[] reinterpret_cast<EdgeArrayAlloc *>(tempEdges);
            return false;
        }
        edge->setChild(child);
        edge->addVisits(copiedVisits[i]);
    }
    // Then the cut moves, unvisited, in the order of the full list (descending policy).
    for (uint32_t j = 0, k = numKept; j < numMoves; j++)
        if (!isKept[j])
            new (&tempEdges->edges[k++]) Edge(moves[j], std::min(policy[j], 1.0f));

    EdgeArray *expected = expectedOld;
    bool       suc = edges.compare_exchange_strong(expected, tempEdges, std::memory_order_acq_rel);
    if (!suc) {
        // Another thread replaced the array first: drop ours, retire nothing.
        delete[] reinterpret_cast<EdgeArrayAlloc *>(tempEdges);
        return false;
    }

    if (allocCounter)
        allocCounter->fetch_add(edgeArrayFootprint(numMoves), std::memory_order_relaxed);

    // Catch up: visits added to the old edges since the copy would be lost otherwise. Threads
    // that still hold an old edge may add more later; those few visits are dropped.
    for (uint32_t i = 0; i < numKept; i++) {
        uint64_t now = expectedOld->edges[i].getVisits();
        if (now > copiedVisits[i])
            tempEdges->edges[i].addVisits(now - copiedVisits[i]);
    }

    // Other threads may still read the old array: free it only at the search boundary.
    retired.retire(expectedOld);
    return true;
}

void RetiredEdgeArrays::retire(EdgeArray *edgeArray)
{
    std::lock_guard<std::mutex> lock(mutex);
    arrays.push_back(edgeArray);
    totalBytes += edgeArrayFootprint(edgeArray->numEdges);
}

size_t RetiredEdgeArrays::freeAll()
{
    std::lock_guard<std::mutex> lock(mutex);
    for (EdgeArray *edgeArray : arrays)
        delete[] reinterpret_cast<EdgeArrayAlloc *>(edgeArray);
    arrays.clear();
    size_t freed = totalBytes;
    totalBytes   = 0;
    return freed;
}

size_t RetiredEdgeArrays::count() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return arrays.size();
}

size_t RetiredEdgeArrays::bytes() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return totalBytes;
}

}  // namespace Search::MCTS
