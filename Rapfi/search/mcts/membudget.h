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

// MCTS graph memory budget helpers (SEARCH-03).

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <limits>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
    #include <unistd.h>
#endif

namespace Search::MCTS {

/// Percentage of the budget at which the graph stops accepting new nodes and edge arrays.
constexpr size_t GraphSoftLimitPercent = 85;

/// Available physical memory in KiB, or 0 when the platform cannot tell.
inline size_t availablePhysicalMemoryKB()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<size_t>(status.ullAvailPhys / 1024);
    return 0;
#elif defined(__linux__)
    if (FILE *f = std::fopen("/proc/meminfo", "r")) {
        char          line[256];
        unsigned long long kb = 0;
        bool          found = false;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                found = true;
                break;
            }
        }
        std::fclose(f);
        if (found)
            return static_cast<size_t>(kb);
    }
    return 0;
#elif (defined(__unix__) || defined(__APPLE__)) && defined(_SC_AVPHYS_PAGES)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        return static_cast<size_t>(pages) * static_cast<size_t>(psize) / 1024;
    return 0;
#else
    return 0;
#endif
}

/// Default graph budget in bytes for a search with no explicit memory limit:
/// `percent` % of (available RAM + what the graph already holds, since that memory is ours).
/// Returns 0 for "unbounded": percent <= 0, or the available memory is unknown.
/// A bounded result is never 0.
inline size_t deriveDefaultGraphBudget(size_t availableKB, size_t graphUsedBytes, int percent)
{
    if (percent <= 0)
        return 0;
    constexpr size_t Max = std::numeric_limits<size_t>::max();

    size_t availBytes = availableKB > Max / 1024 ? Max : availableKB * 1024;
    size_t total      = availBytes > Max - graphUsedBytes ? Max : availBytes + graphUsedBytes;
    if (total == 0)
        return 0;

    size_t pct = static_cast<size_t>(std::min(percent, 100));
    return std::max<size_t>(1, total / 100 * pct + (total % 100) * pct / 100);
}

/// The graph size in bytes at which insertion closes.
inline size_t softGraphLimit(size_t limitBytes)
{
    return limitBytes / 100 * GraphSoftLimitPercent
           + (limitBytes % 100) * GraphSoftLimitPercent / 100;
}

/// Whether the graph must stop adding nodes and edge arrays. An unbounded graph never closes.
inline bool graphInsertionClosed(bool bounded, size_t usedBytes, size_t limitBytes)
{
    return bounded && usedBytes >= softGraphLimit(limitBytes);
}

}  // namespace Search::MCTS
