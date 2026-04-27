#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// AllocationTracker  –  singleton that records every live allocation across all
// allocators.  Enables:
//   • memory-leak detection
//   • double-free detection (LRU cache of recently freed pointers)
//   • per-allocator live-allocation breakdown
// ─────────────────────────────────────────────────────────────────────────────
class AllocationTracker {
public:
    struct Record {
        void*       ptr;
        std::size_t requestedSize;
        std::size_t actualSize;
        std::string allocatorName;
        uint64_t    seqNum;
    };

    static AllocationTracker& Instance() {
        static AllocationTracker inst;
        return inst;
    }

    void Track(void* ptr, std::size_t reqSize, std::size_t actualSize,
               const std::string& allocName) {
        if (!ptr) return;
        m_live[ptr] = {ptr, reqSize, actualSize, allocName, m_seq++};
    }

    void Untrack(void* ptr) {
        if (!ptr) return;
        auto it = m_live.find(ptr);
        if (it == m_live.end()) {
            // Check recent-free cache for double-free
            if (m_freed.count(ptr)) {
                std::cerr << "[AllocationTracker] DOUBLE-FREE detected @ " << ptr << "\n";
            } else {
                std::cerr << "[AllocationTracker] FREE of untracked ptr @ " << ptr << "\n";
            }
            return;
        }
        // Move to freed cache
        m_freed[ptr] = it->second;
        m_live.erase(it);
        if (m_freed.size() > FREED_CACHE_SIZE) {
            // Evict oldest
            auto oldest = std::min_element(m_freed.begin(), m_freed.end(),
                [](auto& a, auto& b){ return a.second.seqNum < b.second.seqNum; });
            m_freed.erase(oldest);
        }
    }

    [[nodiscard]] bool IsLive(void* ptr)   const { return m_live.count(ptr) > 0; }
    [[nodiscard]] bool WasFreed(void* ptr) const { return m_freed.count(ptr) > 0; }

    [[nodiscard]] std::size_t LiveCount() const { return m_live.size(); }
    [[nodiscard]] std::size_t LiveBytes() const {
        std::size_t total = 0;
        for (auto& [p, r] : m_live) total += r.requestedSize;
        return total;
    }

    void ReportLeaks(std::ostream& out = std::cout) const {
        if (m_live.empty()) {
            out << "[AllocationTracker] No leaks detected.\n";
            return;
        }
        out << "[AllocationTracker] *** " << m_live.size() << " LEAK(S) DETECTED ***\n";
        // Sort by sequence number for readability
        std::vector<const Record*> leaks;
        leaks.reserve(m_live.size());
        for (auto& [p, r] : m_live) leaks.push_back(&r);
        std::sort(leaks.begin(), leaks.end(), [](auto* a, auto* b){ return a->seqNum < b->seqNum; });
        for (auto* r : leaks) {
            out << "  Leak #" << r->seqNum
                << "  allocator=" << r->allocatorName
                << "  ptr=" << r->ptr
                << "  size=" << r->requestedSize << " B\n";
        }
    }

    void Reset() { m_live.clear(); m_freed.clear(); m_seq = 0; }

private:
    AllocationTracker() = default;
    static constexpr std::size_t FREED_CACHE_SIZE = 512;

    std::unordered_map<void*, Record> m_live;
    std::unordered_map<void*, Record> m_freed;
    uint64_t m_seq = 0;
};
