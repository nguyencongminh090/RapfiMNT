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

// PERF-03 phase 2 tests (docs/todo/PERF-03-mcts-node-table-footprint.md, "Test list"):
// NodeTable as per-shard open addressing of tagged Node pointers plus a per-shard slab pool.

#include "testutil.h"

#include "../eval/evaluator.h"
#include "../game/board.h"
#include "../search/mcts/node.h"
#include "../search/mcts/nodetable.h"
#include "../search/mcts/searcher.h"
#include "../search/searchcommon.h"
#include "../search/searchengine.h"
#include "../search/searchthread.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace Search::MCTS;

namespace {

/// Distinct pseudo-random hashes from a fixed seed.
std::vector<HashKey> makeHashes(size_t n, uint64_t seed)
{
    std::mt19937_64              rng(seed);
    std::unordered_set<HashKey>  seen;
    std::vector<HashKey>         hashes;
    while (hashes.size() < n) {
        HashKey h = rng();
        if (seen.insert(h).second)
            hashes.push_back(h);
    }
    return hashes;
}

size_t countNodes(NodeTable &table)
{
    size_t count = 0;
    for (size_t i = 0; i < table.getNumShards(); i++)
        table.forEachNode(table.getShardByShardIndex(i), [&](Node &) { count++; });
    return count;
}

const Pos   kMoves[]  = {Pos(0, 0), Pos(1, 0), Pos(2, 0)};
const float kPolicy[] = {0.5f, 0.3f, 0.2f};

// ---- lookup and insert ----

void findNode_emptyTable_returnsNull()
{
    NodeTable table(0);

    Node *node = table.findNode(HashKey(0x1234));

    assert(node == nullptr);
}

void findNode_missingHashInNonEmptyShard_returnsNull()
{
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(101, 11);
    for (size_t i = 0; i < 100; i++)
        table.tryEmplaceNode(hashes[i], 0u);

    Node *node = table.findNode(hashes[100]);

    assert(node == nullptr);
}

void tryEmplaceNode_newHash_insertsOnceAndFindReturnsSamePointer()
{
    NodeTable table(0);

    auto [node, inserted] = table.tryEmplaceNode(HashKey(0xdeadbeef), 0u);

    assert(inserted);
    assert(node != nullptr);
    assert(table.findNode(HashKey(0xdeadbeef)) == node);
    assert(countNodes(table) == 1);
}

void tryEmplaceNode_existingHash_returnsSameNodeNotInserted()
{
    NodeTable table(0);
    auto [first, firstInserted] = table.tryEmplaceNode(HashKey(42), 1u);

    auto [second, secondInserted] = table.tryEmplaceNode(HashKey(42), 7u);

    assert(firstInserted);
    assert(!secondInserted);
    assert(second == first);
    assert(second->getAgeRef().load() == 1u);
    assert(countNodes(table) == 1);
}

void tryEmplaceNode_newHash_constructsNodeWithHashAndAge()
{
    NodeTable table(3);

    auto [node, inserted] = table.tryEmplaceNode(HashKey(0x0123456789abcdefULL), 5u);

    assert(inserted);
    assert(node->getHash() == HashKey(0x0123456789abcdefULL));
    assert(node->getAgeRef().load() == 5u);
    assert(node->isLeaf());
    assert(node->getVisits() == 0);
}

void tryEmplaceNode_sameTagDifferentHash_bothFoundSeparately()
{
    // Arrange: same upper 16 bits (tag) and same low bits (probe start), different middle bits.
    NodeTable     table(0);
    const HashKey a = 0xabcd000000000005ULL;
    const HashKey b = 0xabcd000100000005ULL;
    auto [nodeA, insertedA] = table.tryEmplaceNode(a, 0u);

    Node *missB                = table.findNode(b);
    auto [nodeB, insertedB]    = table.tryEmplaceNode(b, 0u);

    assert(insertedA);
    assert(missB == nullptr);
    assert(insertedB);
    assert(nodeB != nodeA);
    assert(table.findNode(a) == nodeA && nodeA->getHash() == a);
    assert(table.findNode(b) == nodeB && nodeB->getHash() == b);
}

// ---- growth and pointer stability ----

void tryEmplaceNode_manyInsertsOneShard_slotArrayGrowsAndHeldPointersStayValid()
{
    // Arrange: hold pointers to the first 100 nodes.
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(20000, 21);
    std::vector<Node *>  held;
    for (size_t i = 0; i < 100; i++)
        held.push_back(table.tryEmplaceNode(hashes[i], 0u).first);
    size_t capacityBefore = table.getShardStats(table.getShardByShardIndex(0)).slotCapacity;

    // Act: force several rehashes.
    for (size_t i = 100; i < hashes.size(); i++)
        table.tryEmplaceNode(hashes[i], 0u);

    // Assert
    size_t capacityAfter = table.getShardStats(table.getShardByShardIndex(0)).slotCapacity;
    assert(capacityAfter >= 4 * capacityBefore);
    for (size_t i = 0; i < held.size(); i++) {
        assert(held[i]->getHash() == hashes[i]);
        assert(table.findNode(hashes[i]) == held[i]);
    }
    for (HashKey h : hashes)
        assert(table.findNode(h) && table.findNode(h)->getHash() == h);
}

void tryEmplaceNode_manyInsertsOneShard_slabsGrowAndHeldPointersStayValid()
{
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(10 * NodeTable::SlabNodes + 3, 22);
    std::vector<Node *>  held;
    for (size_t i = 0; i < NodeTable::SlabNodes; i++)
        held.push_back(table.tryEmplaceNode(hashes[i], 0u).first);
    size_t slabsBefore = table.getShardStats(table.getShardByShardIndex(0)).numSlabs;

    for (size_t i = NodeTable::SlabNodes; i < hashes.size(); i++)
        table.tryEmplaceNode(hashes[i], 0u);

    NodeTable::ShardStats stats = table.getShardStats(table.getShardByShardIndex(0));
    assert(slabsBefore == 1);
    assert(stats.numSlabs == 11);
    assert(stats.slabNodeCapacity == 11 * NodeTable::SlabNodes);
    for (size_t i = 0; i < held.size(); i++) {
        assert(held[i]->getHash() == hashes[i]);
        assert(table.findNode(hashes[i]) == held[i]);
    }
}

void tryEmplaceNode_manyInserts_loadFactorStaysAtMostThreeQuarters()
{
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(5000, 23);

    bool ok = true;
    for (HashKey h : hashes) {
        table.tryEmplaceNode(h, 0u);
        NodeTable::ShardStats s = table.getShardStats(table.getShardByShardIndex(0));
        ok = ok && 4 * s.numNodes <= 3 * s.slotCapacity;
    }

    assert(ok);
}

// ---- concurrency ----

void tryEmplaceNode_sameHashFromManyThreads_exactlyOneInsertsAllGetSamePointer()
{
    // Arrange: 8 threads walk the same hash list in the same order, as allocateOrFindNode does
    // (find, then emplace on a miss).
    constexpr int        NumThreads = 8;
    NodeTable            table(2);
    std::vector<HashKey> hashes = makeHashes(4000, 31);
    std::vector<std::vector<Node *>> ptrs(NumThreads, std::vector<Node *>(hashes.size()));
    std::vector<std::vector<char>>   ins(NumThreads, std::vector<char>(hashes.size()));
    std::atomic<bool>                go {false};

    // Act
    std::vector<std::thread> threads;
    for (int t = 0; t < NumThreads; t++)
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (size_t i = 0; i < hashes.size(); i++) {
                Node *node     = table.findNode(hashes[i]);
                bool  inserted = false;
                if (!node)
                    std::tie(node, inserted) = table.tryEmplaceNode(hashes[i], 0u);
                ptrs[t][i] = node;
                ins[t][i]  = inserted;
            }
        });
    go.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();

    // Assert
    for (size_t i = 0; i < hashes.size(); i++) {
        int numInserted = 0;
        for (int t = 0; t < NumThreads; t++) {
            numInserted += ins[t][i];
            assert(ptrs[t][i] == ptrs[0][i]);
        }
        assert(numInserted == 1);
        assert(ptrs[0][i]->getHash() == hashes[i]);
    }
    assert(countNodes(table) == hashes.size());
}

void tryEmplaceNode_disjointHashesFromManyThreads_nothingLost()
{
    constexpr int        NumThreads = 8;
    constexpr size_t     PerThread  = 3000;
    NodeTable            table(2);
    std::vector<HashKey> hashes = makeHashes(NumThreads * PerThread, 32);
    std::atomic<int>     numInserted {0};
    std::atomic<bool>    go {false};

    std::vector<std::thread> threads;
    for (int t = 0; t < NumThreads; t++)
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (size_t i = t * PerThread; i < (t + 1) * PerThread; i++)
                if (table.tryEmplaceNode(hashes[i], 0u).second)
                    numInserted.fetch_add(1, std::memory_order_relaxed);
        });
    go.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();

    assert(numInserted.load() == int(hashes.size()));
    assert(countNodes(table) == hashes.size());
    for (HashKey h : hashes)
        assert(table.findNode(h) && table.findNode(h)->getHash() == h);
}

// ---- recycle ----

void eraseNodesIf_staleAge_removesStaleAndKeepsReachedAtSameAddress()
{
    // Arrange: even index = reached (age 1), odd index = stale (age 0).
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(1000, 41);
    std::vector<Node *>  ptrs;
    for (size_t i = 0; i < hashes.size(); i++)
        ptrs.push_back(table.tryEmplaceNode(hashes[i], uint32_t(i % 2 == 0)).first);
    NodeTable::Shard shard = table.getShardByShardIndex(0);

    // Act
    size_t numErased = table.eraseNodesIf(
        shard,
        [](Node &node) { return node.getAgeRef().load() != 1u; });

    // Assert
    assert(numErased == 500);
    assert(countNodes(table) == 500);
    for (size_t i = 0; i < hashes.size(); i++) {
        if (i % 2 == 0) {
            assert(table.findNode(hashes[i]) == ptrs[i]);
            assert(ptrs[i]->getHash() == hashes[i]);
        }
        else
            assert(table.findNode(hashes[i]) == nullptr);
    }
}

void eraseNodesIf_predicateSeesEdgesBeforeDestruction()
{
    // Arrange: one node with a 3-edge array.
    NodeTable table(0);
    Node     *node = table.tryEmplaceNode(HashKey(77), 0u).first;
    node->createEdgesFrom(kMoves, kPolicy, 3, EdgeLimits {}, nullptr);
    NodeTable::Shard shard     = table.getShardByShardIndex(0);
    uint32_t         seenEdges = 0;

    // Act
    size_t numErased = table.eraseNodesIf(shard, [&](Node &n) {
        if (const EdgeArray *edges = n.getEdges())
            seenEdges = edges->numEdges;
        return true;
    });

    // Assert
    assert(numErased == 1);
    assert(seenEdges == 3);
    assert(table.findNode(HashKey(77)) == nullptr);
}

void eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab()
{
    // Arrange: fill, then free 100 nodes.
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(500, 42);
    std::set<Node *>     freedAddrs;
    for (size_t i = 0; i < 400; i++) {
        Node *n = table.tryEmplaceNode(hashes[i], uint32_t(i < 100 ? 0 : 1)).first;
        if (i < 100)
            freedAddrs.insert(n);
    }
    NodeTable::Shard shard = table.getShardByShardIndex(0);
    table.eraseNodesIf(shard, [](Node &n) { return n.getAgeRef().load() == 0u; });
    NodeTable::ShardStats before = table.getShardStats(shard);

    // Act: insert 100 new nodes.
    std::vector<Node *> fresh;
    for (size_t i = 400; i < 500; i++)
        fresh.push_back(table.tryEmplaceNode(hashes[i], 1u).first);

    // Assert
    NodeTable::ShardStats after = table.getShardStats(shard);
    assert(before.numFreeNodes == 100);
    assert(after.numFreeNodes == 0);
    assert(after.numSlabs == before.numSlabs);
    for (Node *n : fresh)
        assert(freedAddrs.count(n) == 1);
    for (size_t i = 400; i < 500; i++)
        assert(table.findNode(hashes[i]) == fresh[i - 400]);
}

void eraseNodesIf_manyRecycleRounds_probeLengthStaysBounded()
{
    // Arrange/Act: 20 rounds; each inserts 400 nodes with age = round and erases nodes older
    // than the previous round (about 800 live nodes stay).
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(20 * 400, 43);
    NodeTable::Shard     shard = table.getShardByShardIndex(0);
    for (uint32_t round = 0; round < 20; round++) {
        for (size_t i = round * 400; i < (round + 1) * 400; i++)
            table.tryEmplaceNode(hashes[i], round);
        table.eraseNodesIf(shard, [round](Node &n) { return n.getAgeRef().load() + 1 < round; });
    }

    // Assert
    NodeTable::ShardStats s = table.getShardStats(shard);
    assert(s.numNodes == 800);
    assert(4 * s.numNodes <= 3 * s.slotCapacity);
    assert(s.meanProbeLength <= 2.5);
    assert(s.maxProbeLength <= 32);
    for (size_t i = 18 * 400; i < hashes.size(); i++)
        assert(table.findNode(hashes[i]) != nullptr);
    for (size_t i = 0; i < 18 * 400; i++)
        assert(table.findNode(hashes[i]) == nullptr);
}

// ---- clear and iteration ----

void clearShard_nonEmpty_emptiesShardAndReleasesStorage()
{
    // Arrange
    NodeTable            table(0);
    std::vector<HashKey> hashes = makeHashes(300, 51);
    for (HashKey h : hashes)
        table.tryEmplaceNode(h, 0u);
    table.findNode(hashes[0])->createEdgesFrom(kMoves, kPolicy, 3, EdgeLimits {}, nullptr);
    NodeTable::Shard shard = table.getShardByShardIndex(0);

    // Act
    table.clearShard(shard);

    // Assert
    NodeTable::ShardStats s = table.getShardStats(shard);
    assert(s.numNodes == 0);
    assert(s.slotCapacity == 0);
    assert(s.numSlabs == 0);
    assert(s.numFreeNodes == 0);
    assert(table.heapBytes() == 0);
    assert(table.findNode(hashes[0]) == nullptr);
    assert(countNodes(table) == 0);
    assert(table.tryEmplaceNode(hashes[1], 0u).second);
}

void forEachNode_afterInsertsAndErase_visitsEachLiveNodeOnce()
{
    NodeTable            table(1);
    std::vector<HashKey> hashes = makeHashes(600, 52);
    for (size_t i = 0; i < hashes.size(); i++)
        table.tryEmplaceNode(hashes[i], uint32_t(i % 3 == 0));
    for (size_t s = 0; s < table.getNumShards(); s++)
        table.eraseNodesIf(table.getShardByShardIndex(s),
                           [](Node &n) { return n.getAgeRef().load() == 1u; });

    std::multiset<HashKey> visited;
    for (size_t s = 0; s < table.getNumShards(); s++)
        table.forEachNode(table.getShardByShardIndex(s),
                          [&](Node &n) { visited.insert(n.getHash()); });

    assert(visited.size() == 400);
    for (size_t i = 0; i < hashes.size(); i++)
        assert(visited.count(hashes[i]) == (i % 3 == 0 ? 0u : 1u));
}

void heapBytes_afterInserts_equalsSlotsPlusSlabs()
{
    NodeTable            table(2);
    std::vector<HashKey> hashes = makeHashes(3000, 53);
    for (HashKey h : hashes)
        table.tryEmplaceNode(h, 0u);

    size_t bytes = table.heapBytes();

    size_t expected = 0;
    for (size_t s = 0; s < table.getNumShards(); s++) {
        NodeTable::ShardStats st = table.getShardStats(table.getShardByShardIndex(s));
        expected += st.slotCapacity * sizeof(uint64_t) + st.numSlabs * NodeTable::SlabBytes;
    }
    assert(bytes == expected);
    assert(bytes > hashes.size() * sizeof(Node));
}

// ---- end to end through the engine ----

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

    void evaluatePolicy(const Board &board,
                        Evaluation::PolicyBuffer &policyBuffer,
                        Evaluation::AccLevel) override
    {
        for (int i = 0; i < boardSize * boardSize; i++)
            if (policyBuffer.getComputeFlag(i))
                policyBuffer(i) = float((i * 37) % 11) * 0.1f;
    }
};

struct NodeTableEngineFixture
{
    size_t savedThreads;

    NodeTableEngineFixture()
    {
        Search::Engine.waitForIdle();
        savedThreads = Search::Engine.size();
        Search::Engine.setNumThreads(1);
        Search::Engine.setupEvaluator([](int boardSize, Rule rule, Numa::NumaNodeId) {
            return std::make_unique<FixedEvaluator>(boardSize, rule);
        });
        Search::Engine.setupSearcher(Search::createSearcher("mcts"));
        Search::Engine.clear(true);
    }

    ~NodeTableEngineFixture()
    {
        Search::Engine.waitForIdle();
        Search::Engine.setupSearcher(Search::createSearcher(""));
        Search::Engine.setupEvaluator(nullptr);
        Search::Engine.setNumThreads(savedThreads);
        Search::Engine.clear(true);
    }

    MCTSSearcher &searcher() { return static_cast<MCTSSearcher &>(*Search::Engine.searcher()); }
};

void runSearch(const Board &board, uint64_t maxNodes)
{
    Search::SearchOptions options;
    options.rule                = {FREESTYLE, GameRule::FREEOPEN};
    options.disableOpeningQuery = true;
    options.maxNodes            = maxNodes;

    ScopedCoutCapture cap;
    Search::Engine.startThinking(board, options);
    Search::Engine.waitForIdle();
}

/// Sum of nodeFootprint() plus edge-array footprint over every node in the table.
size_t liveFootprint(NodeTable &table)
{
    size_t sum = 0;
    for (size_t i = 0; i < table.getNumShards(); i++)
        table.forEachNode(table.getShardByShardIndex(i), [&](Node &n) {
            sum += nodeFootprint();
            if (const EdgeArray *edges = n.getEdges())
                sum += edgeArrayFootprint(edges->numEdges);
        });
    return sum;
}

std::unique_ptr<Board> makeBoard()
{
    auto board = std::make_unique<Board>(15, CandidateRange::SQUARE3_LINE4);
    board->newGame(FREESTYLE);
    for (Pos p : {Pos(7, 7), Pos(8, 8), Pos(7, 8), Pos(6, 9)})
        board->move(FREESTYLE, p);
    return board;
}

void recycleOldNodes_afterMoveForward_graphMemoryUsedEqualsLiveFootprints()
{
    // Arrange: search, then go forward two moves so the next search recycles.
    NodeTableEngineFixture fx;
    auto                   board = makeBoard();
    runSearch(*board, 3000);
    size_t nodesBefore = countNodes(*fx.searcher().nodeTable);
    board->move(FREESTYLE, Pos(9, 9));
    board->move(FREESTYLE, Pos(5, 10));

    // Act
    runSearch(*board, 3000);

    // Assert
    MCTSSearcher &s          = fx.searcher();
    size_t        nodesAfter = countNodes(*s.nodeTable);
    assert(nodesBefore > 100);
    assert(nodesAfter > 0);
    assert(s.graphMemoryUsed.load() == liveFootprint(*s.nodeTable));
}

void clear_afterSearch_tableEmptyAndGraphMemoryUsedZero()
{
    NodeTableEngineFixture fx;
    auto                   board = makeBoard();
    runSearch(*board, 2000);
    assert(countNodes(*fx.searcher().nodeTable) > 0);

    Search::Engine.clear(true);

    MCTSSearcher &s = fx.searcher();
    assert(countNodes(*s.nodeTable) == 0);
    assert(s.graphMemoryUsed.load() == 0);
    assert(s.graph.root == nullptr);
}

}  // namespace

void runMctsNodeTableTests()
{
    std::printf("\n== PERF-03 MCTS node table ==\n");
    RUN_TEST(findNode_emptyTable_returnsNull);
    RUN_TEST(findNode_missingHashInNonEmptyShard_returnsNull);
    RUN_TEST(tryEmplaceNode_newHash_insertsOnceAndFindReturnsSamePointer);
    RUN_TEST(tryEmplaceNode_existingHash_returnsSameNodeNotInserted);
    RUN_TEST(tryEmplaceNode_newHash_constructsNodeWithHashAndAge);
    RUN_TEST(tryEmplaceNode_sameTagDifferentHash_bothFoundSeparately);
    RUN_TEST(tryEmplaceNode_manyInsertsOneShard_slotArrayGrowsAndHeldPointersStayValid);
    RUN_TEST(tryEmplaceNode_manyInsertsOneShard_slabsGrowAndHeldPointersStayValid);
    RUN_TEST(tryEmplaceNode_manyInserts_loadFactorStaysAtMostThreeQuarters);
    RUN_TEST(tryEmplaceNode_sameHashFromManyThreads_exactlyOneInsertsAllGetSamePointer);
    RUN_TEST(tryEmplaceNode_disjointHashesFromManyThreads_nothingLost);
    RUN_TEST(eraseNodesIf_staleAge_removesStaleAndKeepsReachedAtSameAddress);
    RUN_TEST(eraseNodesIf_predicateSeesEdgesBeforeDestruction);
    RUN_TEST(eraseNodesIf_thenInsert_reusesFreedStorageWithoutNewSlab);
    RUN_TEST(eraseNodesIf_manyRecycleRounds_probeLengthStaysBounded);
    RUN_TEST(clearShard_nonEmpty_emptiesShardAndReleasesStorage);
    RUN_TEST(forEachNode_afterInsertsAndErase_visitsEachLiveNodeOnce);
    RUN_TEST(heapBytes_afterInserts_equalsSlotsPlusSlabs);
    RUN_TEST(recycleOldNodes_afterMoveForward_graphMemoryUsedEqualsLiveFootprints);
    RUN_TEST(clear_afterSearch_tableEmptyAndGraphMemoryUsedZero);
}
