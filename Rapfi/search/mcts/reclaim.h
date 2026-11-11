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

// MCTS node reclaim inside a search (SEARCH-16): helpers that do not depend on the searcher.
// See docs/todo/SEARCH-16-mcts-node-reclaim.md.

#include "membudget.h"
#include "node.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace Search::MCTS {

/// Number of visit buckets: one per visit count below 8, then four per power of two, up to the
/// 48 bit visit counter.
constexpr int NumVisitBuckets = 4 * 46 + 4;

/// The histogram bucket of a visit count. Monotonic, without gaps.
inline int visitBucket(uint64_t visits)
{
    if (visits < 8)
        return static_cast<int>(visits);
    visits = std::min(visits, Edge::VisitMask);
    int exponent = 3;
    while (visits >> (exponent + 1))
        exponent++;
    return 4 * (exponent - 1) + static_cast<int>((visits >> (exponent - 2)) & 3);
}

/// The largest visit count that falls in the bucket.
inline uint64_t bucketMaxVisits(int bucket)
{
    if (bucket < 8)
        return static_cast<uint64_t>(bucket);
    int      exponent = bucket / 4 + 1;
    uint64_t mantissa = 4 + static_cast<uint64_t>(bucket % 4);
    return ((mantissa + 1) << (exponent - 2)) - 1;
}

/// Estimated bytes a reclaim pass frees, by the visit count of the trimmed node.
struct ReclaimHistogram
{
    size_t bytes[NumVisitBuckets] = {};

    void add(uint64_t visits, size_t freedBytes) { bytes[visitBucket(visits)] += freedBytes; }

    void merge(const ReclaimHistogram &other)
    {
        for (int i = 0; i < NumVisitBuckets; i++)
            bytes[i] += other.bytes[i];
    }

    /// The smallest bucket bound V such that trimming every node with at most V visits frees
    /// at least needBytes, by this estimate. 0 (trim nothing) for an empty histogram or
    /// needBytes == 0.
    /// The need is capped at 3/4 of the bytes in the histogram: the estimate misses what is
    /// freed below a trimmed node (proven nodes, children with more visits than their parent),
    /// so an unreachable need must not raise V to the most visited nodes, which are the
    /// principal lines. The caller runs another pass when one was not enough.
    uint64_t thresholdFor(size_t needBytes) const
    {
        if (needBytes == 0)
            return 0;
        size_t total = 0;
        for (int i = 0; i < NumVisitBuckets; i++)
            total += bytes[i];
        needBytes = std::min(needBytes, total - total / 4);
        size_t   sum       = 0;
        uint64_t threshold = 0;
        for (int i = 0; i < NumVisitBuckets; i++) {
            if (bytes[i] == 0)
                continue;
            sum += bytes[i];
            threshold = bucketMaxVisits(i);
            if (sum >= needBytes)
                break;
        }
        return threshold;
    }
};

/// The graph size in bytes a reclaim pass aims for: `percent` % of the soft limit.
/// 0 for a non-positive percent; the percent is capped at 100.
inline size_t reclaimTargetBytes(size_t limitBytes, int percent)
{
    if (percent <= 0)
        return 0;
    size_t soft = softGraphLimit(limitBytes);
    size_t pct  = static_cast<size_t>(std::min(percent, 100));
    return soft / 100 * pct + (soft % 100) * pct / 100;
}

/// Whether a reclaim pass may turn this node back into a leaf: it is expanded, not terminal,
/// has at most maxVisits visits and no proven bound (a re-expansion would loosen the bound).
/// The caller excludes the root. Only meaningful while no search thread runs.
inline bool nodeTrimmable(const Node &node, uint64_t maxVisits)
{
    if (node.isLeaf() || node.isTerminal() || node.getVisits() > maxVisits)
        return false;
    ValueBound bound = node.getBound();
    return bound.lower <= -VALUE_INFINITE && bound.upper >= VALUE_INFINITE;
}

/// Stop-the-world gate for the search threads (SEARCH-16).
///
/// Every search thread calls enter() before its playout loop and leave() after it, and polls
/// requested() at its safe point (the top of the loop, where it holds no node or edge
/// pointer); when a stop is requested it calls park(). A thread that wants the world stopped
/// calls tryBegin() from its own safe point: on true every other entered thread is parked (or
/// has left), the caller may run jobs on all of them with runJob(), and must call end().
class SafepointGate
{
public:
    /// A job run by the coordinator (index 0) and every parked thread (distinct indices from 1).
    using Job = std::function<void(size_t workerIndex)>;

    /// Registers the calling thread as running. Blocks while the world is stopped.
    void enter()
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return !stopRequested.load(std::memory_order_relaxed); });
        numActive++;
    }

    /// Unregisters the calling thread. Never blocks.
    void leave()
    {
        std::lock_guard lock(mutex);
        numActive--;
        cv.notify_all();
    }

    /// Whether some thread asked for a stop. Cheap; to be polled at the safe point.
    bool requested() const { return stopRequested.load(std::memory_order_relaxed); }

    /// Parks the calling thread until the stop ends, running the coordinator's jobs meanwhile.
    /// Returns at once when no stop is requested.
    void park()
    {
        std::unique_lock lock(mutex);
        if (!stopRequested.load(std::memory_order_relaxed))
            return;

        numParked++;
        cv.notify_all();
        uint64_t seenEpoch = jobEpoch;
        for (;;) {
            cv.wait(lock, [&] {
                return !stopRequested.load(std::memory_order_relaxed) || jobEpoch != seenEpoch;
            });
            if (jobEpoch == seenEpoch)
                break;  // the stop ended

            seenEpoch         = jobEpoch;
            const Job *job    = currentJob;
            size_t     worker = nextWorkerIndex++;
            lock.unlock();
            (*job)(worker);
            lock.lock();
            numJobsDone++;
            cv.notify_all();
        }
        numParked--;
    }

    /// Asks for a stop and waits until every other entered thread is parked or has left.
    /// @return False, without waiting, when another thread already holds the stop: the caller
    ///   is at its safe point and should park().
    bool tryBegin()
    {
        std::unique_lock lock(mutex);
        if (stopRequested.load(std::memory_order_relaxed))
            return false;
        stopRequested.store(true, std::memory_order_relaxed);
        cv.wait(lock, [this] { return numParked + 1 >= numActive; });
        return true;
    }

    /// Runs the job on the calling thread and on every parked thread, and waits for all of them.
    /// Only between a successful tryBegin() and end().
    void runJob(const Job &job)
    {
        std::unique_lock lock(mutex);
        size_t           numWorkers = numParked;
        currentJob                  = &job;
        nextWorkerIndex             = 1;
        numJobsDone                 = 0;
        jobEpoch++;
        cv.notify_all();
        lock.unlock();

        job(0);

        lock.lock();
        cv.wait(lock, [&] { return numJobsDone == numWorkers; });
        currentJob = nullptr;
    }

    /// Ends the stop: parked threads resume, enter() unblocks.
    void end()
    {
        std::lock_guard lock(mutex);
        stopRequested.store(false, std::memory_order_relaxed);
        cv.notify_all();
    }

private:
    std::mutex              mutex;
    std::condition_variable cv;
    /// Written under the mutex; read without it by requested().
    std::atomic<bool> stopRequested {false};
    size_t            numActive       = 0;
    size_t            numParked       = 0;
    uint64_t          jobEpoch        = 0;
    const Job        *currentJob      = nullptr;
    size_t            nextWorkerIndex = 1;
    size_t            numJobsDone     = 0;
};

}  // namespace Search::MCTS
