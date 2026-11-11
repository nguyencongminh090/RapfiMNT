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

// SEARCH-16 tests (docs/todo/SEARCH-16-mcts-node-reclaim.md): reclaim inside a search. When the
// graph closes, the search threads stop at a safe point, low-visit nodes are turned back into
// leaves and the nodes nothing reaches any more are erased; no edge array of a kept node changes.

#include "testutil.h"

#include "../eval/evaluator.h"
#include "../game/board.h"
#include "../search/mcts/node.h"
#include "../search/mcts/nodetable.h"
#include "../search/mcts/reclaim.h"
#include "../search/mcts/searcher.h"
#include "../search/searchcommon.h"
#include "../search/searchconfig.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace Search::MCTS;

namespace {

// ----------------------------------------------------------------------------------------------
// visitBucket / bucketMaxVisits

void visitBucket_belowEight_isTheVisitCount()
{
    for (uint64_t v = 0; v < 8; v++)
        assert(visitBucket(v) == static_cast<int>(v));
}

void visitBucket_oneToManyVisits_monotonicWithoutGaps()
{
    int previous = visitBucket(0);
    for (uint64_t v = 1; v < 100000; v++) {
        int bucket = visitBucket(v);
        assert(bucket == previous || bucket == previous + 1);
        previous = bucket;
    }
}

void bucketMaxVisits_everyBucket_isTheLargestVisitCountOfTheBucket()
{
    for (int b = 0; b < NumVisitBuckets - 1; b++) {
        uint64_t top = bucketMaxVisits(b);
        assert(visitBucket(top) == b);
        assert(visitBucket(top + 1) == b + 1);
    }
}

void visitBucket_largestVisitCount_insideTheHistogram()
{
    assert(visitBucket(Edge::VisitMask) == NumVisitBuckets - 1);
    assert(visitBucket(~uint64_t(0)) == NumVisitBuckets - 1);
    assert(bucketMaxVisits(NumVisitBuckets - 1) == Edge::VisitMask);
}

// ----------------------------------------------------------------------------------------------
// ReclaimHistogram

void thresholdFor_emptyHistogram_zero()
{
    ReclaimHistogram h;
    assert(h.thresholdFor(1000) == 0);
}

void thresholdFor_zeroNeed_zero()
{
    ReclaimHistogram h;
    h.add(2, 500);
    assert(h.thresholdFor(0) == 0);
}

void thresholdFor_needMetByFirstBucket_thatBucketBound()
{
    ReclaimHistogram h;
    h.add(2, 500);
    h.add(40, 500);
    assert(h.thresholdFor(500) == 2);
}

void thresholdFor_needSpansBuckets_smallestSufficientBound()
{
    ReclaimHistogram h;
    h.add(2, 500);
    h.add(5, 300);
    h.add(9, 300);  // bucket of 8..9
    h.add(40, 500);
    assert(h.thresholdFor(501) == 5);
    assert(h.thresholdFor(801) == 9);
}

void thresholdFor_needAboveTotal_boundCoveringThreeQuartersOfTheBytes()
{
    // 750 of the 1000 bytes are reached in the bucket of 5 visits; the most visited nodes stay.
    ReclaimHistogram h;
    h.add(2, 600);
    h.add(5, 300);
    h.add(40, 100);
    assert(h.thresholdFor(5000) == 5);
    assert(h.thresholdFor(751) == 5);
}

void thresholdFor_needAboveTotalAndBytesInTopBucket_topBucketBound()
{
    ReclaimHistogram h;
    h.add(2, 500);
    h.add(40, 500);  // bucket of 40..47
    assert(h.thresholdFor(5000) == 47);
}

void merge_twoHistograms_sumsBuckets()
{
    ReclaimHistogram a, b;
    a.add(2, 100);
    a.add(6, 10);
    b.add(2, 50);
    b.add(100, 7);

    a.merge(b);

    assert(a.bytes[visitBucket(2)] == 150);
    assert(a.bytes[visitBucket(6)] == 10);
    assert(a.bytes[visitBucket(100)] == 7);
}

// ----------------------------------------------------------------------------------------------
// reclaimTargetBytes

void reclaimTargetBytes_fiftyPercent_halfOfSoftLimit()
{
    assert(reclaimTargetBytes(1000000, 50) == softGraphLimit(1000000) / 2);
}

void reclaimTargetBytes_nonPositivePercent_zero()
{
    assert(reclaimTargetBytes(1000000, 0) == 0);
    assert(reclaimTargetBytes(1000000, -5) == 0);
}

void reclaimTargetBytes_percentAbove100_softLimit()
{
    assert(reclaimTargetBytes(1000000, 250) == softGraphLimit(1000000));
}

// ----------------------------------------------------------------------------------------------
// Node::releaseEdges / nodeTrimmable

const Pos   kMoves[]  = {Pos(0, 0), Pos(1, 0), Pos(2, 0)};
const float kPolicy[] = {0.5f, 0.3f, 0.2f};

std::unique_ptr<Node> makeExpandedNode(uint64_t visits)
{
    auto node = std::make_unique<Node>(HashKey(7), 0);
    node->setNonTerminal(0.25f, 0.1f);
    node->createEdgesFrom(kMoves, kPolicy, 3, {}, nullptr);
    if (visits > 1)
        node->incrementVisits(visits - 1);
    return node;
}

/// Gives the node a child that is lost for the side to move there, so the node is a proven win.
std::unique_ptr<Node> attachLostChild(Node &node)
{
    auto child = std::make_unique<Node>(HashKey(8), 0);
    child->setTerminal(mated_in(0));
    Edge &edge = (*node.getEdges())[0];
    edge.setChild(child.get());
    edge.addVisits(1);
    node.updateStats();
    return child;
}

void releaseEdges_expandedNode_leafAgainAndReturnsFootprint()
{
    auto node = makeExpandedNode(5);

    size_t freed = node->releaseEdges();

    assert(node->isLeaf());
    assert(freed == edgeArrayFootprint(3));
}

void releaseEdges_leaf_returnsZero()
{
    Node node(HashKey(7), 0);
    node.setNonTerminal(0.25f, 0.1f);

    size_t freed = node.releaseEdges();

    assert(freed == 0);
    assert(node.isLeaf());
}

void releaseEdges_expandedNode_keepsValueVisitsAndBound()
{
    auto       node  = makeExpandedNode(5);
    auto       child = attachLostChild(*node);
    float      q     = node->getQ();
    float      d     = node->getD();
    ValueBound bound = node->getBound();

    node->releaseEdges();

    assert(node->getVisits() == 5);
    assert(node->getQ() == q);
    assert(node->getD() == d);
    assert(node->getEvalUtility() == 0.25f);
    assert(node->getBound().lower == bound.lower && node->getBound().upper == bound.upper);
}

void releaseEdges_thenCreateEdgesFrom_expandsAgain()
{
    auto                node = makeExpandedNode(5);
    std::atomic<size_t> counter {0};
    node->releaseEdges();

    bool noMoves = node->createEdgesFrom(kMoves, kPolicy, 3, {}, &counter);

    assert(!noMoves);
    assert(!node->isLeaf());
    assert(node->getEdges()->numEdges == 3);
    assert((*node->getEdges())[0].getVisits() == 0);
    assert((*node->getEdges())[0].getChild() == nullptr);
    assert(counter.load() == edgeArrayFootprint(3));
}

void nodeTrimmable_leaf_false()
{
    Node node(HashKey(7), 0);
    node.setNonTerminal(0.25f, 0.1f);
    assert(!nodeTrimmable(node, 100));
}

void nodeTrimmable_visitsAtMax_true()
{
    auto node = makeExpandedNode(5);
    assert(nodeTrimmable(*node, 5));
}

void nodeTrimmable_visitsAboveMax_false()
{
    auto node = makeExpandedNode(6);
    assert(!nodeTrimmable(*node, 5));
}

void nodeTrimmable_provenBound_false()
{
    auto node  = makeExpandedNode(5);
    auto child = attachLostChild(*node);
    assert(node->getBound().lower > -VALUE_INFINITE);  // the fixture proves a win
    assert(!nodeTrimmable(*node, 100));
}

void nodeTrimmable_terminal_false()
{
    auto node = makeExpandedNode(5);
    assert(node->tryMarkTerminal(mated_in(0)));
    assert(!nodeTrimmable(*node, 100));
}

// ----------------------------------------------------------------------------------------------
// SafepointGate

using namespace std::chrono_literals;

/// Worker threads that behave like search threads: enter, loop over safe points, leave.
struct GateWorkers
{
    SafepointGate           &gate;
    std::atomic<bool>        stop {false};
    std::atomic<int>         numInWork {0};  ///< threads between two safe points
    std::atomic<int>         numEntered {0};
    std::vector<std::thread> threads;

    GateWorkers(SafepointGate &g, int n) : gate(g)
    {
        for (int i = 0; i < n; i++)
            threads.emplace_back([this] {
                gate.enter();
                numEntered++;
                while (!stop.load()) {
                    if (gate.requested())
                        gate.park();
                    numInWork++;
                    std::this_thread::yield();
                    numInWork--;
                }
                gate.leave();
            });
        while (numEntered.load() < n)
            std::this_thread::yield();
    }

    ~GateWorkers()
    {
        stop = true;
        for (auto &t : threads)
            t.join();
    }
};

void park_nothingRequested_returnsAtOnce()
{
    SafepointGate gate;
    gate.enter();

    gate.park();

    assert(!gate.requested());
    gate.leave();
}

void tryBegin_singleActiveThread_trueAtOnce()
{
    SafepointGate gate;
    gate.enter();

    bool began = gate.tryBegin();

    assert(began);
    assert(gate.requested());
    gate.end();
    assert(!gate.requested());
    gate.leave();
}

void tryBegin_secondCallerWhileHeld_false()
{
    SafepointGate gate;
    gate.enter();
    assert(gate.tryBegin());
    bool second = true;

    std::thread other([&] { second = gate.tryBegin(); });
    other.join();

    assert(!second);
    gate.end();
    gate.leave();
}

void tryBegin_otherThreadsRunning_returnsOnlyWhenAllParked()
{
    SafepointGate gate;
    gate.enter();
    GateWorkers workers(gate, 4);

    for (int round = 0; round < 200; round++) {
        bool began = gate.tryBegin();

        assert(began);
        assert(workers.numInWork.load() == 0);  // every worker is parked
        gate.end();
    }
    gate.leave();
}

void tryBegin_otherThreadLeaves_isNotWaitedFor()
{
    SafepointGate     gate;
    std::atomic<bool> entered {false};
    gate.enter();
    // A thread that never reaches a safe point again: it only leaves.
    std::thread other([&] {
        gate.enter();
        entered = true;
        std::this_thread::sleep_for(20ms);
        gate.leave();
    });
    while (!entered.load())
        std::this_thread::yield();

    bool began = gate.tryBegin();

    assert(began);
    gate.end();
    gate.leave();
    other.join();
}

void enter_whileStopped_blocksUntilEnd()
{
    SafepointGate     gate;
    std::atomic<bool> entered {false};
    gate.enter();
    assert(gate.tryBegin());

    std::thread other([&] {
        gate.enter();
        entered = true;
        gate.leave();
    });
    std::this_thread::sleep_for(30ms);
    bool enteredWhileStopped = entered.load();
    gate.end();
    other.join();

    assert(!enteredWhileStopped);
    assert(entered.load());
    gate.leave();
}

void runJob_parkedThreads_everyThreadRunsItOnceWithDistinctIndex()
{
    SafepointGate gate;
    gate.enter();
    GateWorkers workers(gate, 4);
    assert(gate.tryBegin());
    std::mutex            m;
    std::multiset<size_t> indices;
    std::set<std::thread::id> ids;

    gate.runJob([&](size_t worker) {
        std::lock_guard lock(m);
        indices.insert(worker);
        ids.insert(std::this_thread::get_id());
    });

    gate.end();
    assert(indices == std::multiset<size_t>({0, 1, 2, 3, 4}));
    assert(ids.size() == 5);
    gate.leave();
}

void end_parkedThreads_resume()
{
    SafepointGate gate;
    gate.enter();
    GateWorkers workers(gate, 2);
    assert(gate.tryBegin());
    assert(workers.numInWork.load() == 0);

    gate.end();

    // The workers run again: they pass through their work section.
    bool resumed = false;
    for (int i = 0; i < 2000 && !resumed; i++) {
        resumed = workers.numInWork.load() > 0;
        std::this_thread::sleep_for(1ms);
    }
    assert(resumed);
    gate.leave();
}

void tryBegin_manyRoundsFromCompetingThreads_oneCoordinatorAtATime()
{
    SafepointGate            gate;
    std::atomic<int>         numCoordinators {0}, numRunning {0};
    std::atomic<int>         numPasses {0}, numJobRuns {0};
    std::atomic<bool>        overlap {false};
    std::vector<std::thread> threads;
    const int                NumThreads = 4, PassesWanted = 300;

    for (int t = 0; t < NumThreads; t++)
        threads.emplace_back([&, t] {
            gate.enter();
            uint64_t counter = t;
            while (numPasses.load() < PassesWanted) {
                // The safe point of a search thread
                if (gate.requested())
                    gate.park();
                else if (++counter % 7 == 0) {
                    if (!gate.tryBegin())
                        gate.park();
                    else {
                        if (numCoordinators.fetch_add(1) != 0 || numRunning.load() != 0)
                            overlap = true;
                        gate.runJob([&](size_t) { numJobRuns++; });
                        numPasses++;
                        numCoordinators--;
                        gate.end();
                    }
                }
                numRunning++;
                if (numCoordinators.load() != 0)
                    overlap = true;
                numRunning--;
            }
            gate.leave();
        });
    for (auto &t : threads)
        t.join();

    assert(!overlap.load());
    assert(numPasses.load() >= PassesWanted);
    assert(numJobRuns.load() >= numPasses.load());
}

// ----------------------------------------------------------------------------------------------
// MCTSSearcher::reclaimNodes on a hand-built graph
//
//   root (100) -+- high (60) -+- highLeaf (5)
//               |             +- shared (3)
//               +- low (4) ---+- lowLeafA (1)
//               |             +- lowLeafB (1)
//               |             +- shared (3)
//               +- rootLeaf (1)

const MCTSSearcher::ParallelRun SingleRun = [](const SafepointGate::Job &job) { job(0); };

struct Graph
{
    MCTSSearcher s;
    Node        *root, *high, *low, *rootLeaf, *highLeaf, *shared, *lowLeafA, *lowLeafB;

    Node *addNode(HashKey hash, uint64_t visits, float utility)
    {
        auto [node, inserted] = s.nodeTable->tryEmplaceNode(hash, s.graph.globalNodeAge);
        assert(inserted);
        s.graphMemoryUsed.fetch_add(nodeFootprint());
        node->setNonTerminal(utility, 0.1f);
        if (visits > 1)
            node->incrementVisits(visits - 1);
        return node;
    }

    void expand(Node *node) { node->createEdgesFrom(kMoves, kPolicy, 3, {}, &s.graphMemoryUsed); }

    static void link(Node *parent, uint32_t index, Node *child, uint64_t visits)
    {
        Edge &edge = (*parent->getEdges())[index];
        edge.setChild(child);
        edge.addVisits(visits);
    }

    explicit Graph(uint64_t rootVisits = 100, uint64_t highVisits = 60)
    {
        root     = addNode(1, rootVisits, 0.0f);
        high     = addNode(2, highVisits, 0.3f);
        low      = addNode(3, 4, -0.2f);
        rootLeaf = addNode(4, 1, 0.1f);
        highLeaf = addNode(5, 5, 0.4f);
        shared   = addNode(6, 3, -0.5f);
        lowLeafA = addNode(7, 1, 0.6f);
        lowLeafB = addNode(8, 1, -0.7f);
        expand(root);
        expand(high);
        expand(low);
        link(root, 0, high, highVisits);
        link(root, 1, low, 4);
        link(root, 2, rootLeaf, 1);
        link(high, 0, highLeaf, 5);
        link(high, 1, shared, 3);
        link(low, 0, lowLeafA, 1);
        link(low, 1, lowLeafB, 1);
        link(low, 2, shared, 1);
        low->updateStats();
        high->updateStats();
        root->updateStats();
        s.graph.root = root;
    }

    /// The target that makes the pass trim exactly the nodes with at most 4 visits (`low`).
    size_t targetTrimmingLow() const
    {
        return s.graphMemoryUsed.load() - (edgeArrayFootprint(3) + 3 * nodeFootprint());
    }

    size_t liveFootprint() const
    {
        size_t sum = 0;
        for (size_t i = 0; i < s.nodeTable->getNumShards(); i++)
            s.nodeTable->forEachNode(s.nodeTable->getShardByShardIndex(i), [&](Node &n) {
                sum += nodeFootprint();
                if (const EdgeArray *edges = n.getEdges())
                    sum += edgeArrayFootprint(edges->numEdges);
            });
        return sum;
    }

    size_t numNodes() const
    {
        size_t count = 0;
        for (size_t i = 0; i < s.nodeTable->getNumShards(); i++)
            s.nodeTable->forEachNode(s.nodeTable->getShardByShardIndex(i), [&](Node &) { count++; });
        return count;
    }
};

void reclaimNodes_usedAtOrBelowTarget_changesNothing()
{
    Graph  g;
    size_t used = g.s.graphMemoryUsed.load();

    MCTSSearcher::ReclaimStats stats = g.s.reclaimNodes(used, SingleRun);

    assert(stats.maxVisits == 0 && stats.numTrimmed == 0 && stats.numErased == 0);
    assert(g.numNodes() == 8);
    assert(!g.low->isLeaf());
    assert(g.s.graphMemoryUsed.load() == used);
}

void reclaimNodes_lowVisitNode_trimmedToLeafKeepingStats()
{
    Graph g;
    float q = g.low->getQ();

    MCTSSearcher::ReclaimStats stats = g.s.reclaimNodes(g.targetTrimmingLow(), SingleRun);

    assert(stats.maxVisits == 4);
    assert(stats.numTrimmed == 1);
    assert(g.s.nodeTable->findNode(3) == g.low);
    assert(g.low->isLeaf());
    assert(g.low->getVisits() == 4);
    assert(g.low->getQ() == q);
    assert(g.low->getEvalUtility() == -0.2f);
}

void reclaimNodes_childOnlyUnderTrimmedNode_erased()
{
    Graph g;

    MCTSSearcher::ReclaimStats stats = g.s.reclaimNodes(g.targetTrimmingLow(), SingleRun);

    assert(stats.numErased == 2);
    assert(g.s.nodeTable->findNode(7) == nullptr);
    assert(g.s.nodeTable->findNode(8) == nullptr);
    assert(g.numNodes() == 6);
}

void reclaimNodes_childSharedWithKeptNode_kept()
{
    Graph g;

    g.s.reclaimNodes(g.targetTrimmingLow(), SingleRun);

    assert(g.s.nodeTable->findNode(6) == g.shared);
    assert(g.shared->getVisits() == 3);
    assert((*g.high->getEdges())[1].getChild() == g.shared);
}

void reclaimNodes_rootWithFewVisits_keepsEdges()
{
    // Arrange: a root with fewer visits than its child (a transposition root); the target asks
    // for more than the graph can give, so every trimmable node is trimmed.
    Graph g(2, 60);

    MCTSSearcher::ReclaimStats stats = g.s.reclaimNodes(0, SingleRun);

    assert(stats.maxVisits >= 60);
    assert(!g.root->isLeaf());
    assert(g.high->isLeaf() && g.low->isLeaf());
    assert((*g.root->getEdges())[0].getChild() == g.high);
    assert(g.numNodes() == 4);  // root and its three children
}

void reclaimNodes_provenLowVisitNode_keepsEdges()
{
    // Arrange: `low` is a proven win (its first child is lost for the side to move there).
    Graph g;
    assert(g.lowLeafA->tryMarkTerminal(mated_in(0)));
    g.low->updateStats();
    assert(g.low->getBound().lower > -VALUE_INFINITE);

    g.s.reclaimNodes(0, SingleRun);

    assert(!g.low->isLeaf());
    assert(g.s.nodeTable->findNode(7) == g.lowLeafA);
    assert(g.s.nodeTable->findNode(8) == g.lowLeafB);
    assert(g.high->isLeaf());  // the unproven node is trimmed
}

void reclaimNodes_keptParent_edgesAndStatsUnchanged()
{
    Graph      g;
    float      q     = g.root->getQ();
    float      d     = g.root->getD();
    EdgeArray *edges = g.root->getEdges();

    g.s.reclaimNodes(g.targetTrimmingLow(), SingleRun);

    // The edge array of the kept parent is the same array with the same content, so the visited
    // edges are still a prefix and the statistics recomputed from them do not move.
    assert(g.root->getEdges() == edges);
    assert((*edges)[0].getChild() == g.high && (*edges)[0].getVisits() == 60);
    assert((*edges)[1].getChild() == g.low && (*edges)[1].getVisits() == 4);
    assert((*edges)[2].getChild() == g.rootLeaf && (*edges)[2].getVisits() == 1);
    g.root->updateStats();
    assert(g.root->getQ() == q);
    assert(g.root->getD() == d);
    assert(g.root->getVisits() == 100);
}

void reclaimNodes_afterPass_graphMemoryUsedEqualsLiveFootprint()
{
    Graph  g;
    size_t before = g.s.graphMemoryUsed.load();

    MCTSSearcher::ReclaimStats stats = g.s.reclaimNodes(g.targetTrimmingLow(), SingleRun);

    assert(g.s.graphMemoryUsed.load() == g.liveFootprint());
    assert(stats.bytesBefore == before);
    assert(stats.bytesAfter == g.liveFootprint());
    assert(before - stats.bytesAfter == edgeArrayFootprint(3) + 2 * nodeFootprint());
}

void reclaimNodes_retiredEdgeArrays_freedAndUncounted()
{
    // Arrange: widen a cut node, which retires its old edge array (SEARCH-10).
    Graph       g;
    const Pos   moves[]  = {Pos(0, 1), Pos(1, 1), Pos(2, 1), Pos(3, 1)};
    const float policy[] = {0.7f, 0.1f, 0.1f, 0.1f};
    Node       *cut      = g.addNode(9, 1, 0.0f);
    cut->createEdgesFrom(moves, policy, 4, EdgeLimits {0.5f, 1}, &g.s.graphMemoryUsed);
    Graph::link(cut, 0, g.rootLeaf, 1);  // a widen needs every kept edge visited
    bool replaced = cut->replaceEdgesWithFull(cut->getEdges(),
                                              moves,
                                              policy,
                                              4,
                                              &g.s.graphMemoryUsed,
                                              g.s.retiredEdgeArrays);
    assert(replaced && g.s.retiredEdgeArrays.count() == 1);

    g.s.reclaimNodes(g.s.graphMemoryUsed.load(), SingleRun);  // target not below: only step 1

    assert(g.s.retiredEdgeArrays.count() == 0);
    assert(g.s.graphMemoryUsed.load() == g.liveFootprint());
}

/// A deterministic layered graph with transpositions: `width` nodes per layer, every node of a
/// layer points to three nodes of the next one, visits fall with the depth.
struct LayeredGraph
{
    MCTSSearcher s;

    LayeredGraph(int layers, int width)
    {
        std::vector<std::vector<Node *>> nodes(layers);
        HashKey                          hash = 1;
        for (int l = 0; l < layers; l++)
            for (int i = 0; i < (l == 0 ? 1 : width); i++) {
                auto [node, inserted] = s.nodeTable->tryEmplaceNode(hash++, 0u);
                s.graphMemoryUsed.fetch_add(nodeFootprint());
                node->setNonTerminal(float((l * 7 + i * 3) % 11) * 0.1f - 0.5f, 0.1f);
                node->incrementVisits(uint64_t(1) << (layers - l));
                nodes[l].push_back(node);
            }
        for (int l = 0; l + 1 < layers; l++)
            for (size_t i = 0; i < nodes[l].size(); i++) {
                Node *node = nodes[l][i];
                node->createEdgesFrom(kMoves, kPolicy, 3, {}, &s.graphMemoryUsed);
                for (uint32_t e = 0; e < 3; e++) {
                    // Some nodes of each layer are reached by nobody
                    Node *child = nodes[l + 1][(i * 2 + e) % (nodes[l + 1].size() * 3 / 4 + 1)];
                    Edge &edge  = (*node->getEdges())[e];
                    edge.setChild(child);
                    edge.addVisits(1 + e);
                }
            }
        s.graph.root = nodes[0][0];
    }

    /// (hash, is leaf) of every live node, ordered.
    std::set<std::pair<HashKey, bool>> snapshot() const
    {
        std::set<std::pair<HashKey, bool>> out;
        for (size_t i = 0; i < s.nodeTable->getNumShards(); i++)
            s.nodeTable->forEachNode(s.nodeTable->getShardByShardIndex(i), [&](Node &n) {
                out.insert({n.getHash(), n.isLeaf()});
            });
        return out;
    }
};

void reclaimNodes_parallelRun_sameResultAsSingleThread()
{
    // Arrange: the same graph twice; the second pass runs every phase on four threads.
    LayeredGraph single(8, 64), parallel(8, 64);
    size_t       target = single.s.graphMemoryUsed.load() / 2;
    const MCTSSearcher::ParallelRun fourThreads = [](const SafepointGate::Job &job) {
        std::vector<std::thread> threads;
        for (size_t i = 1; i < 4; i++)
            threads.emplace_back([&job, i] { job(i); });
        job(0);
        for (auto &t : threads)
            t.join();
    };

    MCTSSearcher::ReclaimStats a = single.s.reclaimNodes(target, SingleRun);
    MCTSSearcher::ReclaimStats b = parallel.s.reclaimNodes(target, fourThreads);

    assert(a.numTrimmed > 0 && a.numErased > 0);
    assert(a.maxVisits == b.maxVisits);
    assert(a.numTrimmed == b.numTrimmed && a.numErased == b.numErased);
    assert(a.bytesAfter == b.bytesAfter);
    assert(single.snapshot() == parallel.snapshot());
}

// ----------------------------------------------------------------------------------------------
// Search integration

/// Deterministic stand-in evaluator (the test binary loads no network).
class FixedEvaluator : public Evaluation::Evaluator
{
public:
    FixedEvaluator(int boardSize, Rule rule) : Evaluator(boardSize, rule) {}
    void initEmptyBoard() override {}

    Evaluation::ValueType evaluateValue(const Board &board, Evaluation::AccLevel) override
    {
        HashKey h    = board.zobristKey();
        float   win  = float(h & 0xff) / 256.0f;
        float   loss = float((h >> 8) & 0xff) / 256.0f;
        return Evaluation::ValueType(win, loss, -1.0f);
    }

    void evaluatePolicy(const Board              &board,
                        Evaluation::PolicyBuffer &policyBuffer,
                        Evaluation::AccLevel) override
    {
        for (int i = 0; i < boardSize * boardSize; i++)
            if (policyBuffer.getComputeFlag(i))
                policyBuffer(i) = float((i * 37) % 11) * 0.1f;
    }
};

struct ReclaimEngineFixture
{
    size_t savedThreads;
    int    savedPercent;

    ReclaimEngineFixture(size_t numThreads, int reclaimPercent, size_t memoryKB)
    {
        Search::Engine.waitForIdle();
        savedThreads = Search::Engine.size();
        savedPercent = Search::SearchCfg.mctsReclaimTargetPercent;
        Search::SearchCfg.mctsReclaimTargetPercent = reclaimPercent;
        Search::Engine.setNumThreads(numThreads);
        Search::Engine.setupEvaluator([](int boardSize, Rule rule, Numa::NumaNodeId) {
            return std::make_unique<FixedEvaluator>(boardSize, rule);
        });
        Search::Engine.setupSearcher(Search::createSearcher("mcts"));
        Search::Engine.clear(true);
        searcher().setMemoryLimit(memoryKB);
    }

    ~ReclaimEngineFixture()
    {
        Search::Engine.waitForIdle();
        searcher().setMemoryLimit(0);  // the engine copies the limit into the next searcher
        Search::SearchCfg.mctsReclaimTargetPercent = savedPercent;
        Search::Engine.setupSearcher(Search::createSearcher(""));
        Search::Engine.setupEvaluator(nullptr);
        Search::Engine.setNumThreads(savedThreads);
        Search::Engine.clear(true);
    }

    MCTSSearcher &searcher() { return static_cast<MCTSSearcher &>(*Search::Engine.searcher()); }

    void search(uint64_t maxNodes)
    {
        Board board(15, CandidateRange::SQUARE3_LINE4);
        board.newGame(FREESTYLE);
        for (Pos p : {Pos(7, 7), Pos(8, 8), Pos(7, 8), Pos(6, 9)})
            board.move(FREESTYLE, p);

        Search::SearchOptions options;
        options.rule                = {FREESTYLE, GameRule::FREEOPEN};
        options.disableOpeningQuery = true;
        options.maxNodes            = maxNodes;

        ScopedCoutCapture cap;
        Search::Engine.startThinking(board, options);
        Search::Engine.waitForIdle();
    }

    size_t liveFootprint()
    {
        NodeTable &table = *searcher().nodeTable;
        size_t     sum   = 0;
        for (size_t i = 0; i < table.getNumShards(); i++)
            table.forEachNode(table.getShardByShardIndex(i), [&](Node &n) {
                sum += nodeFootprint();
                if (const EdgeArray *edges = n.getEdges())
                    sum += edgeArrayFootprint(edges->numEdges);
            });
        return sum;
    }
};

void search_reclaimOffSmallBudget_noPassAndGraphStaysClosed()
{
    ReclaimEngineFixture fx(1, 0, 512);

    fx.search(50000);

    assert(fx.searcher().numReclaimPasses.load() == 0);
    assert(fx.searcher().graphClosed());
    assert(fx.searcher().graphMemoryUsed.load() == fx.liveFootprint());
}

void search_reclaimOnSmallBudget_passRunsAndCountersMatchLiveFootprint()
{
    ReclaimEngineFixture fx(1, 50, 512);

    fx.search(50000);

    MCTSSearcher &s = fx.searcher();
    assert(s.numReclaimPasses.load() >= 1);
    assert(s.graphMemoryUsed.load() == fx.liveFootprint());
    assert(s.graphMemoryUsed.load() < s.graphMemoryLimit);
    assert(!s.graph.root->isLeaf());
    assert(s.graph.root->getVisits() >= 50000);
}

void search_reclaimOnSmallBudgetFourThreads_passRunsAndCountersMatchLiveFootprint()
{
    ReclaimEngineFixture fx(4, 50, 512);

    fx.search(200000);

    MCTSSearcher &s = fx.searcher();
    assert(s.numReclaimPasses.load() >= 1);
    assert(s.retiredEdgeArrays.count() == 0);
    assert(s.graphMemoryUsed.load() == fx.liveFootprint());
    assert(!s.graph.root->isLeaf());
}

void search_reclaimOnUnboundedGraph_noPass()
{
    // Arrange: a budget far above what the search can use.
    ReclaimEngineFixture fx(1, 50, 1024 * 1024);

    fx.search(20000);

    assert(fx.searcher().numReclaimPasses.load() == 0);
    assert(!fx.searcher().graphClosed());
}

void search_reclaimOnLongSearch_graphRefillsAndSecondPassRuns()
{
    // A second pass can only run when the graph grew again after the first one, that is when
    // trimmed nodes were expanded again and new nodes were added.
    ReclaimEngineFixture fx(1, 50, 512);

    fx.search(300000);

    MCTSSearcher &s = fx.searcher();
    assert(s.numReclaimPasses.load() >= 2);
    assert(s.graphMemoryUsed.load() == fx.liveFootprint());
}

void searchConfig_defaultReclaimTargetPercent_isZero()
{
    Search::SearchConfig cfg;
    assert(cfg.mctsReclaimTargetPercent == 0);
}

}  // namespace

void runMctsReclaimTests()
{
    std::printf("\n== SEARCH-16 MCTS reclaim inside a search ==\n");
    RUN_TEST(visitBucket_belowEight_isTheVisitCount);
    RUN_TEST(visitBucket_oneToManyVisits_monotonicWithoutGaps);
    RUN_TEST(bucketMaxVisits_everyBucket_isTheLargestVisitCountOfTheBucket);
    RUN_TEST(visitBucket_largestVisitCount_insideTheHistogram);
    RUN_TEST(thresholdFor_emptyHistogram_zero);
    RUN_TEST(thresholdFor_zeroNeed_zero);
    RUN_TEST(thresholdFor_needMetByFirstBucket_thatBucketBound);
    RUN_TEST(thresholdFor_needSpansBuckets_smallestSufficientBound);
    RUN_TEST(thresholdFor_needAboveTotal_boundCoveringThreeQuartersOfTheBytes);
    RUN_TEST(thresholdFor_needAboveTotalAndBytesInTopBucket_topBucketBound);
    RUN_TEST(merge_twoHistograms_sumsBuckets);
    RUN_TEST(reclaimTargetBytes_fiftyPercent_halfOfSoftLimit);
    RUN_TEST(reclaimTargetBytes_nonPositivePercent_zero);
    RUN_TEST(reclaimTargetBytes_percentAbove100_softLimit);
    RUN_TEST(releaseEdges_expandedNode_leafAgainAndReturnsFootprint);
    RUN_TEST(releaseEdges_leaf_returnsZero);
    RUN_TEST(releaseEdges_expandedNode_keepsValueVisitsAndBound);
    RUN_TEST(releaseEdges_thenCreateEdgesFrom_expandsAgain);
    RUN_TEST(nodeTrimmable_leaf_false);
    RUN_TEST(nodeTrimmable_visitsAtMax_true);
    RUN_TEST(nodeTrimmable_visitsAboveMax_false);
    RUN_TEST(nodeTrimmable_provenBound_false);
    RUN_TEST(nodeTrimmable_terminal_false);
    RUN_TEST(park_nothingRequested_returnsAtOnce);
    RUN_TEST(tryBegin_singleActiveThread_trueAtOnce);
    RUN_TEST(tryBegin_secondCallerWhileHeld_false);
    RUN_TEST(tryBegin_otherThreadsRunning_returnsOnlyWhenAllParked);
    RUN_TEST(tryBegin_otherThreadLeaves_isNotWaitedFor);
    RUN_TEST(enter_whileStopped_blocksUntilEnd);
    RUN_TEST(runJob_parkedThreads_everyThreadRunsItOnceWithDistinctIndex);
    RUN_TEST(end_parkedThreads_resume);
    RUN_TEST(tryBegin_manyRoundsFromCompetingThreads_oneCoordinatorAtATime);
    RUN_TEST(reclaimNodes_usedAtOrBelowTarget_changesNothing);
    RUN_TEST(reclaimNodes_lowVisitNode_trimmedToLeafKeepingStats);
    RUN_TEST(reclaimNodes_childOnlyUnderTrimmedNode_erased);
    RUN_TEST(reclaimNodes_childSharedWithKeptNode_kept);
    RUN_TEST(reclaimNodes_rootWithFewVisits_keepsEdges);
    RUN_TEST(reclaimNodes_provenLowVisitNode_keepsEdges);
    RUN_TEST(reclaimNodes_keptParent_edgesAndStatsUnchanged);
    RUN_TEST(reclaimNodes_afterPass_graphMemoryUsedEqualsLiveFootprint);
    RUN_TEST(reclaimNodes_retiredEdgeArrays_freedAndUncounted);
    RUN_TEST(reclaimNodes_parallelRun_sameResultAsSingleThread);
    RUN_TEST(search_reclaimOffSmallBudget_noPassAndGraphStaysClosed);
    RUN_TEST(search_reclaimOnSmallBudget_passRunsAndCountersMatchLiveFootprint);
    RUN_TEST(search_reclaimOnSmallBudgetFourThreads_passRunsAndCountersMatchLiveFootprint);
    RUN_TEST(search_reclaimOnUnboundedGraph_noPass);
    RUN_TEST(search_reclaimOnLongSearch_graphRefillsAndSecondPassRuns);
    RUN_TEST(searchConfig_defaultReclaimTargetPercent_isZero);
}
