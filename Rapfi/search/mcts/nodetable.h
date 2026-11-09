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
#include "node.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <utility>
#include <vector>

// Under AddressSanitizer, dead node storage is poisoned (test aid, PERF-04): a read of a recycled
// node through a stale pointer is reported even while its slab is still owned by the table.
#if defined(__SANITIZE_ADDRESS__)
    #define RAPFI_NODETABLE_ASAN 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define RAPFI_NODETABLE_ASAN 1
    #endif
#endif
#ifdef RAPFI_NODETABLE_ASAN
    #include <sanitizer/asan_interface.h>
#endif

namespace Search::MCTS {

/// Sharded hash table of MCTS nodes (PERF-03).
///
/// Each shard is an open-addressing table of 8-byte slots plus a slab pool that owns the nodes.
/// A slot holds a Node pointer in its low 48 bits and the upper 16 bits of the node's hash as a
/// tag; 0 is the empty slot. Linear probing, power-of-two capacity, load factor at most 3/4.
/// Nodes are built in place in fixed-size slabs and never move while they are in the table:
/// growing or rebuilding a shard moves only the slot words. Freed nodes go on a per-shard LIFO
/// free list. Erasing rebuilds the shard's slot array from the survivors, so there are no
/// tombstones, and then frees every slab of the shard that holds no live node (PERF-04), so every
/// slab holds at least one live node at all times. Slabs that keep a survivor stay, with their
/// free slots on the free list: the table heap is bounded by the slot arrays plus one slab per
/// live node, not by the live nodes alone.
///
/// Locking is unchanged from the std::set table: one std::shared_mutex per shard, shared for
/// findNode, unique for tryEmplaceNode. The hooks eraseNodesIf, clearShard and forEachNode
/// expect the caller to hold the shard's lock (processAllShards does).
class NodeTable
{
public:
    /// Number of nodes per slab.
    static constexpr size_t SlabNodes = 32;
    /// Slot capacity of a shard on its first insert.
    static constexpr size_t InitialSlots = 16;

private:
    static constexpr int      PointerBits = 48;
    static constexpr uint64_t PointerMask = (uint64_t(1) << PointerBits) - 1;

    struct Slab
    {
        Slab *next;
        alignas(Node) unsigned char storage[SlabNodes * sizeof(Node)];
    };

    /// Per-shard storage. Only NodeTable reads or writes it.
    class ShardData
    {
    public:
        ShardData() = default;
        ShardData(const ShardData &)            = delete;
        ShardData &operator=(const ShardData &) = delete;
        ~ShardData() { NodeTable::releaseAll(*this); }

    private:
        friend class NodeTable;

        uint64_t *slots    = nullptr;  ///< capacity slot words, or null when capacity is 0
        size_t    capacity = 0;        ///< 0 or a power of two
        size_t    numNodes = 0;        ///< live nodes
        Slab     *slabs    = nullptr;  ///< singly linked, newest first
        size_t    numSlabs = 0;
        size_t    slabUsed = SlabNodes;  ///< bump index into the newest slab
        void     *freeList = nullptr;    ///< LIFO list threaded through dead node storage
        size_t    numFree  = 0;
    };

public:
    /// Bytes of one slab allocation (requested size, without the allocator's header).
    static constexpr size_t SlabBytes = sizeof(Slab);

    struct Shard
    {
        size_t             index;
        ShardData         &data;
        std::shared_mutex &mutex;
    };

    /// Statistics of one shard, for tests and measurement.
    struct ShardStats
    {
        size_t numNodes;          ///< live nodes
        size_t slotCapacity;      ///< slot array length (0 before the first insert)
        size_t numSlabs;          ///< slabs owned by the shard
        size_t slabNodeCapacity;  ///< numSlabs * SlabNodes
        size_t numFreeNodes;      ///< node storage on the free list
        size_t maxProbeLength;    ///< longest probe of a live node (1 = home slot)
        double meanProbeLength;   ///< mean probe of the live nodes (0 when empty)
    };

    NodeTable(size_t numShardsPowerOfTwo)
        : numShards(size_t(1) << numShardsPowerOfTwo)
        , mask(numShards - 1)
        , shardBits(numShardsPowerOfTwo)
        , shards(std::make_unique<ShardData[]>(numShards))
        , mutexes(std::make_unique<std::shared_mutex[]>(numShards))
    {}

    /// Get the total number of shards of this node table.
    size_t getNumShards() const { return numShards; }

    /// Get the shard with the given shard index.
    /// @note This function is thread-safe.
    Shard getShardByShardIndex(size_t index) const
    {
        return Shard {index, shards[index], mutexes[index]};
    }

    /// Get the shard that contains the node with the given hash key.
    /// @note This function is thread-safe.
    Shard getShardByHash(HashKey hash) const
    {
        size_t index = hash & mask;
        return getShardByShardIndex(index);
    }

    /// Find the node with the given hash key.
    /// @return Pointer to the node if found, otherwise nullptr.
    /// @note This function uses reader lock to ensure thread-safety.
    Node *findNode(HashKey hash) const
    {
        Shard            shard = getShardByHash(hash);
        std::shared_lock lock(shard.mutex);
        return lookup(shard.data, hash);
    }

    /// Try emplace a new node into the table.
    /// @param hash Hash key of the new node.
    /// @param args Extra arguments to pass to the constructor of the new node.
    /// @return A pair of (Pointer to the inserted node, Whether the node is
    ///   successfully inserted). If there is already a node inserted by other
    ///   threads, the pointer to that node is returned instead.
    template <typename... Args>
    std::pair<Node *, bool> tryEmplaceNode(HashKey hash, Args... args)
    {
        Shard            shard = getShardByHash(hash);
        std::unique_lock lock(shard.mutex);
        ShardData       &d = shard.data;

        // Another thread may have inserted the node after our findNode missed.
        if (Node *existing = lookup(d, hash))
            return {existing, false};

        // Grow before computing the insert position, so the position is in the final array.
        if (4 * (d.numNodes + 1) > 3 * d.capacity)
            rehash(d, d.capacity ? 2 * d.capacity : InitialSlots);

        Node *node = new (allocateNodeStorage(d)) Node(hash, std::forward<Args>(args)...);
        insertSlot(d, hash, node);
        d.numNodes++;
        return {node, true};
    }

    /// Erase every node of the shard for which pred(node) returns true.
    /// pred sees the node intact (edges included) before it is destroyed. The surviving nodes
    /// keep their addresses; the shard's slot array is rebuilt from them, and slabs left
    /// without a live node are freed (any pointer to an erased node then dangles).
    /// @return The number of erased nodes.
    /// @note The caller must hold the shard's unique lock.
    template <typename Pred>
    size_t eraseNodesIf(const Shard &shard, Pred &&pred)
    {
        ShardData &d         = shard.data;
        size_t     numErased = 0;
        for (size_t i = 0; i < d.capacity; i++) {
            uint64_t slot = d.slots[i];
            if (!slot)
                continue;
            Node *node = slotNode(slot);
            if (pred(*node)) {
                d.slots[i] = 0;
                freeNodeStorage(d, node);
                numErased++;
            }
        }

        if (numErased) {
            d.numNodes -= numErased;
            rebuildAfterErase(d);
        }
        return numErased;
    }

    /// Destroy every node of the shard and release its slot array and slabs.
    /// @note The caller must hold the shard's unique lock.
    void clearShard(const Shard &shard) { releaseAll(shard.data); }

    /// Call f(node) on every node of the shard.
    /// @note The caller must hold the shard's lock.
    template <typename F>
    void forEachNode(const Shard &shard, F &&f) const
    {
        const ShardData &d = shard.data;
        for (size_t i = 0; i < d.capacity; i++)
            if (uint64_t slot = d.slots[i])
                f(*slotNode(slot));
    }

    /// Statistics of one shard.
    /// @note The caller must hold the shard's lock.
    ShardStats getShardStats(const Shard &shard) const
    {
        const ShardData &d = shard.data;
        ShardStats       s {};
        s.numNodes         = d.numNodes;
        s.slotCapacity     = d.capacity;
        s.numSlabs         = d.numSlabs;
        s.slabNodeCapacity = d.numSlabs * SlabNodes;
        s.numFreeNodes     = d.numFree;

        size_t totalProbe = 0;
        for (size_t i = 0; i < d.capacity; i++) {
            if (uint64_t slot = d.slots[i]) {
                size_t home  = homeIndex(slotNode(slot)->getHash(), d.capacity);
                size_t probe = ((i - home) & (d.capacity - 1)) + 1;
                totalProbe += probe;
                s.maxProbeLength = std::max(s.maxProbeLength, probe);
            }
        }
        if (d.numNodes)
            s.meanProbeLength = double(totalProbe) / double(d.numNodes);
        return s;
    }

    /// Bytes requested from the allocator for all slot arrays and slabs (allocator headers not
    /// included). Takes each shard's shared lock.
    size_t heapBytes() const
    {
        size_t bytes = 0;
        for (size_t i = 0; i < numShards; i++) {
            std::shared_lock lock(mutexes[i]);
            bytes += shards[i].capacity * sizeof(uint64_t) + shards[i].numSlabs * SlabBytes;
        }
        return bytes;
    }

private:
    size_t                               numShards;
    size_t                               mask;
    size_t                               shardBits;
    std::unique_ptr<ShardData[]>         shards;
    std::unique_ptr<std::shared_mutex[]> mutexes;

    static Node *slotNode(uint64_t slot) { return reinterpret_cast<Node *>(slot & PointerMask); }
    static uint64_t tagOf(HashKey hash) { return hash & ~PointerMask; }

    /// Home slot of a hash. Uses the bits above the shard bits, so that the slot index does not
    /// repeat the shard index.
    size_t homeIndex(HashKey hash, size_t capacity) const
    {
        return size_t(hash >> shardBits) & (capacity - 1);
    }

    Node *lookup(const ShardData &d, HashKey hash) const
    {
        if (d.capacity == 0)
            return nullptr;

        const size_t   slotMask = d.capacity - 1;
        const uint64_t tag      = tagOf(hash);
        for (size_t i = homeIndex(hash, d.capacity);; i = (i + 1) & slotMask) {
            uint64_t slot = d.slots[i];
            if (!slot)
                return nullptr;
            // The tag only filters: the node's own hash decides.
            if ((slot & ~PointerMask) == tag) {
                Node *node = slotNode(slot);
                if (node->getHash() == hash)
                    return node;
            }
        }
    }

    /// Put a node into the first empty slot of its probe sequence. The hash must not be in the
    /// table and the array must have a free slot.
    void insertSlot(ShardData &d, HashKey hash, Node *node) const
    {
        const size_t slotMask = d.capacity - 1;
        size_t       i        = homeIndex(hash, d.capacity);
        while (d.slots[i])
            i = (i + 1) & slotMask;
        d.slots[i] = tagOf(hash) | reinterpret_cast<uintptr_t>(node);
    }

    /// Move all slot words into a new zeroed array of newCapacity slots. Nodes do not move.
    void rehash(ShardData &d, size_t newCapacity) const
    {
        uint64_t *oldSlots    = d.slots;
        size_t    oldCapacity = d.capacity;

        d.slots    = new uint64_t[newCapacity]();
        d.capacity = newCapacity;
        for (size_t i = 0; i < oldCapacity; i++)
            if (uint64_t slot = oldSlots[i])
                insertSlot(d, slotNode(slot)->getHash(), slotNode(slot));
        delete[] oldSlots;
    }

    /// After an erase: free the slabs left without a live node, then size the slot array for the
    /// survivors (load at most 3/4) and reinsert them, which removes the holes the erase left in
    /// the probe sequences.
    void rebuildAfterErase(ShardData &d) const
    {
        releaseEmptySlabs(d);

        if (d.numNodes == 0) {
            delete[] d.slots;
            d.slots    = nullptr;
            d.capacity = 0;
            return;
        }

        size_t newCapacity = InitialSlots;
        while (4 * d.numNodes > 3 * newCapacity)
            newCapacity *= 2;
        rehash(d, newCapacity);
    }

    /// Storage for one node: the free list first, then the newest slab, then a new slab.
    static void *allocateNodeStorage(ShardData &d)
    {
        if (d.freeList) {
            void *mem = d.freeList;
            std::memcpy(&d.freeList, mem, sizeof(void *));
            d.numFree--;
            unpoisonNodeStorage(mem);
            return mem;
        }

        if (d.slabUsed == SlabNodes) {
            Slab *slab = new Slab;
            if ((reinterpret_cast<uintptr_t>(slab) + sizeof(Slab)) >> PointerBits) {
                std::fprintf(stderr,
                             "NodeTable: slab address %p does not fit in %d bits\n",
                             static_cast<void *>(slab),
                             PointerBits);
                std::abort();
            }
            slab->next = d.slabs;
            d.slabs    = slab;
            d.numSlabs++;
            d.slabUsed = 0;
        }
        return d.slabs->storage + sizeof(Node) * d.slabUsed++;
    }

    /// Destroy a node and push its storage on the free list.
    static void freeNodeStorage(ShardData &d, Node *node)
    {
        node->~Node();
        void *mem = node;
        std::memcpy(mem, &d.freeList, sizeof(void *));
        d.freeList = mem;
        d.numFree++;
        poisonNodeStorage(mem);
    }

    /// Poison a dead node's storage except its first word, which holds the free-list link.
    static void poisonNodeStorage([[maybe_unused]] void *mem)
    {
#ifdef RAPFI_NODETABLE_ASAN
        ASAN_POISON_MEMORY_REGION(static_cast<char *>(mem) + sizeof(void *),
                                  sizeof(Node) - sizeof(void *));
#endif
    }

    static void unpoisonNodeStorage([[maybe_unused]] void *mem)
    {
#ifdef RAPFI_NODETABLE_ASAN
        ASAN_UNPOISON_MEMORY_REGION(mem, sizeof(Node));
#endif
    }

    static void deleteSlab(Slab *slab)
    {
#ifdef RAPFI_NODETABLE_ASAN
        ASAN_UNPOISON_MEMORY_REGION(slab, sizeof(Slab));
#endif
        delete slab;
    }

    /// Free every slab and reset the pool to its empty state (no slab, no free node).
    static void releaseAllSlabs(ShardData &d)
    {
        for (Slab *slab = d.slabs; slab;) {
            Slab *next = slab->next;
            deleteSlab(slab);
            slab = next;
        }
        d.slabs    = nullptr;
        d.numSlabs = 0;
        d.slabUsed = SlabNodes;
        d.freeList = nullptr;
        d.numFree  = 0;
    }

    /// After an erase (PERF-04): free every slab that holds no live node, the bump slab
    /// included, and unlink the free-list entries that point into those slabs. Survivors do not
    /// move, kept slabs keep their list order, and the kept free-list entries keep their order.
    /// Runs under the shard's unique lock, between searches.
    static void releaseEmptySlabs(ShardData &d)
    {
        if (d.numNodes == 0) {
            releaseAllSlabs(d);
            return;
        }

        // An empty slab owns SlabNodes free or never-used slots. With fewer of them in the
        // shard, no slab can be empty.
        const size_t bumpUnused = d.slabs ? SlabNodes - d.slabUsed : 0;
        if (d.numFree + bumpUnused < SlabNodes)
            return;

        // Node -> slab: sort the slab addresses, then binary-search each node address.
        std::vector<Slab *> sorted;
        sorted.reserve(d.numSlabs);
        for (Slab *slab = d.slabs; slab; slab = slab->next)
            sorted.push_back(slab);
        std::sort(sorted.begin(), sorted.end(), std::less<Slab *>());
        auto slabIndexOf = [&sorted](const void *p) -> size_t {
            auto it = std::upper_bound(sorted.begin(),
                                       sorted.end(),
                                       p,
                                       [](const void *addr, const Slab *slab) {
                                           return std::less<const void *>()(addr, slab);
                                       });
            assert(it != sorted.begin());
            size_t index = size_t(it - sorted.begin()) - 1;
            assert(static_cast<const unsigned char *>(p) >= sorted[index]->storage
                   && static_cast<const unsigned char *>(p)
                          < sorted[index]->storage + sizeof(sorted[index]->storage));
            return index;
        };

        std::vector<uint32_t> live(sorted.size(), 0);
        for (size_t i = 0; i < d.capacity; i++)
            if (uint64_t slot = d.slots[i])
                live[slabIndexOf(slotNode(slot))]++;

        const size_t numEmpty = size_t(std::count(live.begin(), live.end(), 0u));
#ifdef NDEBUG
        if (numEmpty == 0)
            return;
#endif

        // Unlink the free-list entries of empty slabs before any slab is freed: the links run
        // through the storage of every slab.
#ifndef NDEBUG
        std::vector<uint32_t> numFreeOf(sorted.size(), 0);
#endif
        void *prev = nullptr;
        for (void *mem = d.freeList; mem;) {
            void *next;
            std::memcpy(&next, mem, sizeof(void *));
            size_t index = slabIndexOf(mem);
#ifndef NDEBUG
            numFreeOf[index]++;
#endif
            if (live[index] == 0) {
                if (prev)
                    std::memcpy(prev, &next, sizeof(void *));
                else
                    d.freeList = next;
                d.numFree--;
            }
            else
                prev = mem;
            mem = next;
        }

#ifndef NDEBUG
        // Every handed-out slot is either live or on the free list.
        for (size_t index = 0; index < sorted.size(); index++) {
            size_t handedOut = sorted[index] == d.slabs ? d.slabUsed : SlabNodes;
            assert(live[index] + numFreeOf[index] == handedOut);
        }
        if (numEmpty == 0)
            return;
#endif

        // Free the empty slabs. If the bump slab goes, the new head slab is fully handed out.
        const bool headReleased = live[slabIndexOf(d.slabs->storage)] == 0;
        for (Slab **link = &d.slabs; Slab *slab = *link;) {
            if (live[slabIndexOf(slab->storage)] == 0) {
                *link = slab->next;
                deleteSlab(slab);
                d.numSlabs--;
            }
            else
                link = &slab->next;
        }
        if (headReleased)
            d.slabUsed = SlabNodes;
    }

    /// Destroy every live node, then free the slot array and every slab.
    static void releaseAll(ShardData &d)
    {
        for (size_t i = 0; i < d.capacity; i++)
            if (uint64_t slot = d.slots[i])
                slotNode(slot)->~Node();
        delete[] d.slots;

        d.slots    = nullptr;
        d.capacity = 0;
        d.numNodes = 0;
        releaseAllSlabs(d);
    }
};

}  // namespace Search::MCTS
