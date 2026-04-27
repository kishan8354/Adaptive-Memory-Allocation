#include "AdaptiveAllocator.h"
#include "LinearAllocator.h"
#include "PoolAllocator.h"
#include "FreeListAllocator.h"
#include "TLSFAllocator.h"
#include "BuddyAllocator.h"
#include "Utils.h"
#include "BuddyAllocator.h"
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <unordered_map>

// ── Strategy recommendation ───────────────────────────────────────────────────
const char* AdaptiveAllocator::StrategyName(Strategy s) {
    switch (s) {
        case Strategy::LINEAR:   return "Linear";
        case Strategy::STACK:    return "Stack";
        case Strategy::POOL:     return "Pool";
        case Strategy::FREELIST: return "FreeList";
        case Strategy::TLSF:     return "TLSF";
        case Strategy::BUDDY:    return "Buddy";
        default:                 return "Unknown";
    }
}

AdaptiveAllocator::Strategy
AdaptiveAllocator::WorkloadProfile::RecommendStrategy() const {
    // Decision tree based on workload features
    //
    //  uniformityScore > 0.9  →  Pool (all same size)
    //  lifoScore       > 0.85 →  Stack (strict LIFO)
    //  sizeCV          < 0.15 →  Pool (very low variance)
    //  avgSize         < 128  →  TLSF (small, fragmentation-sensitive)
    //  fragmentation   > 0.3  →  TLSF (fragmentation too high)
    //  default                →  FreeList

    if (uniformityScore > 0.90)  return Strategy::POOL;
    if (lifoScore       > 0.85)  return Strategy::STACK;
    if (sizeCV          < 0.15)  return Strategy::POOL;
    if (avgSize         < 128.0) return Strategy::TLSF;
    if (fragmentation   > 0.30)  return Strategy::TLSF;
    if (avgSize         > 4096.0 && sizeCV < 0.5) return Strategy::BUDDY;
    return Strategy::FREELIST;
}

// ── Constructor / destructor ──────────────────────────────────────────────────
AdaptiveAllocator::AdaptiveAllocator(std::size_t totalSize)
    : Allocator(totalSize, "Adaptive") {}

AdaptiveAllocator::~AdaptiveAllocator() = default;

// ── Init / Reset ──────────────────────────────────────────────────────────────
void AdaptiveAllocator::Init() {
    m_routingTable.clear();
    m_windowSizes.clear();
    m_windowPtrs.clear();
    m_switchLog.clear();
    m_profile     = {};
    m_windowStart = 0;
    m_current     = m_forced ? m_current : Strategy::FREELIST;

    // Initialise default allocator
    EnsureInitialised(m_current);
    m_active = AllocatorForStrategy(m_current);
    m_active->Init();
    m_used = m_peak = 0;
    m_metrics = {};
}

void AdaptiveAllocator::Reset() {
    m_routingTable.clear();
    m_windowSizes.clear();
    m_windowPtrs.clear();
    if (m_active) m_active->Reset();
    m_used = m_peak = 0;
    m_metrics = {};
}

// ── Sub-allocator management ──────────────────────────────────────────────────
void AdaptiveAllocator::EnsureInitialised(Strategy s) {
    const std::size_t half = m_totalSize / 2;
    switch (s) {
        case Strategy::LINEAR:
            if (!m_linear) {
                m_linear = std::make_unique<LinearAllocator>(m_totalSize);
                m_linear->Init();
            }
            break;
        case Strategy::POOL:
            if (!m_pool) {
                // Use the observed median size (or 64 bytes as a floor).
                // Round up to next 8-byte boundary so all same-size objects fit.
                std::size_t chunk = (m_profile.dominantSize > 0)
                    ? m_profile.dominantSize
                    : std::size_t(64);
                if (chunk < 8)  chunk = 8;
                chunk = (chunk + 7) & ~std::size_t(7);  // align to 8
                const std::size_t poolSz = (m_totalSize / chunk) * chunk;
                m_pool = std::make_unique<PoolAllocator>(poolSz, chunk);
                m_pool->Init();
            }
            break;
        case Strategy::FREELIST:
            if (!m_freeList) {
                m_freeList = std::make_unique<FreeListAllocator>(
                    m_totalSize, FreeListAllocator::PlacementPolicy::FIND_FIRST);
                m_freeList->Init();
            }
            break;
        case Strategy::TLSF:
            if (!m_tlsf) {
                m_tlsf = std::make_unique<TLSFAllocator>(m_totalSize);
                m_tlsf->Init();
            }
            break;
        case Strategy::BUDDY: {
            if (!m_buddy) {
                // Round down to nearest power of 2
                std::size_t sz = Utils::NextPowerOf2(m_totalSize) >> 1;
                if (sz < BuddyAllocator::MIN_BLOCK_SIZE) sz = BuddyAllocator::MIN_BLOCK_SIZE;
                m_buddy = std::make_unique<BuddyAllocator>(sz);
                m_buddy->Init();
            }
            break;
        }
        default: break;
    }
}

Allocator* AdaptiveAllocator::AllocatorForStrategy(Strategy s) {
    switch (s) {
        case Strategy::LINEAR:   return m_linear.get();
        case Strategy::POOL:     return m_pool.get();
        case Strategy::FREELIST: return m_freeList.get();
        case Strategy::TLSF:     return m_tlsf.get();
        case Strategy::BUDDY:    return m_buddy.get();
        default:                 return m_freeList.get();
    }
}

void AdaptiveAllocator::SwitchTo(Strategy s, const char* reason) {
    if (s == m_current) return;
    EnsureInitialised(s);
    Allocator* next = AllocatorForStrategy(s);
    if (!next) return;

    m_switchLog.push_back({m_current, s, m_profile.totalAllocs, reason});
    m_current = s;
    m_active  = next;
}

void AdaptiveAllocator::ForceStrategy(Strategy s) {
    m_forced  = true;
    m_current = s;
    EnsureInitialised(s);
    m_active = AllocatorForStrategy(s);
}

// ── Profile update ────────────────────────────────────────────────────────────
void AdaptiveAllocator::UpdateProfile(std::size_t size) {
    m_windowSizes.push_back(size);
    if (m_windowSizes.size() > WINDOW_SIZE) m_windowSizes.pop_front();
    ++m_profile.totalAllocs;
    ++m_profile.windowAllocs;

    // Track dominant (most-frequent) size in the window
    // Use a simple frequency map over the recent window
    if (!m_windowSizes.empty()) {
        std::unordered_map<std::size_t, int> freq;
        freq.reserve(m_windowSizes.size());
        for (auto s : m_windowSizes) freq[s]++;
        auto it = std::max_element(freq.begin(), freq.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        m_profile.dominantSize = it->first;
    }
}

double AdaptiveAllocator::ComputeSizeCV() const {
    if (m_windowSizes.empty()) return 0.0;
    double sum = 0.0;
    for (auto s : m_windowSizes) sum += static_cast<double>(s);
    double mean = sum / m_windowSizes.size();
    if (mean == 0.0) return 0.0;
    double var = 0.0;
    for (auto s : m_windowSizes) var += std::pow(static_cast<double>(s) - mean, 2.0);
    var /= m_windowSizes.size();
    return std::sqrt(var) / mean;  // coefficient of variation
}

double AdaptiveAllocator::ComputeLIFOScore() const {
    // Measure how often the most recently allocated ptr appears at the
    // front of m_windowPtrs when a free occurs. Approximated by checking
    // if the last N pointers were freed in LIFO order.
    if (m_windowPtrs.size() < 2) return 0.0;
    int score = 0;
    // Compare adjacent pairs: if ptr[i] > ptr[i-1] in address (rough proxy)
    for (int i = 1; i < (int)m_windowPtrs.size(); ++i) {
        if (m_windowPtrs[i] > m_windowPtrs[i-1]) ++score;
    }
    return static_cast<double>(score) / (m_windowPtrs.size() - 1);
}

double AdaptiveAllocator::ComputeUniformityScore() const {
    if (m_windowSizes.empty()) return 1.0;
    // Find the modal size class
    std::unordered_map<std::size_t, int> freq;
    for (auto s : m_windowSizes) freq[s / 64]++;  // 64-byte buckets
    int maxFreq = 0;
    for (auto& [k,v] : freq) maxFreq = std::max(maxFreq, v);
    return static_cast<double>(maxFreq) / m_windowSizes.size();
}

void AdaptiveAllocator::AnalyseWindow() {
    if (m_windowSizes.empty()) return;
    m_profile.windowAllocs = 0;

    // Compute features
    double sum = 0.0;
    for (auto s : m_windowSizes) sum += static_cast<double>(s);
    m_profile.avgSize         = sum / m_windowSizes.size();
    m_profile.sizeCV          = ComputeSizeCV();
    m_profile.lifoScore       = ComputeLIFOScore();
    m_profile.uniformityScore = ComputeUniformityScore();
    m_profile.fragmentation   = m_active ? m_active->GetUtilization() : 0.0;

    if (!m_forced) {
        Strategy rec = m_profile.RecommendStrategy();
        if (rec != m_current) {
            SwitchTo(rec, "window-analysis");
        }
    }
}

// ── Allocate ──────────────────────────────────────────────────────────────────
void* AdaptiveAllocator::Allocate(std::size_t size, std::size_t alignment) {
    if (!m_active) {
        EnsureInitialised(m_current);
        m_active = AllocatorForStrategy(m_current);
    }

    UpdateProfile(size);
    m_windowPtrs.push_back(nullptr);  // placeholder
    if (m_windowPtrs.size() > WINDOW_SIZE) m_windowPtrs.pop_front();

    // Re-analyse every WINDOW_SIZE allocations
    if (m_profile.windowAllocs >= WINDOW_SIZE) AnalyseWindow();

    void* ptr = m_active->Allocate(size, alignment);

    // If POOL strategy rejects an oversized request, fall back to FreeList
    if (!ptr && m_current == Strategy::POOL) {
        EnsureInitialised(Strategy::FREELIST);
        ptr = m_freeList->Allocate(size, alignment);
        if (ptr) {
            m_routingTable[ptr] = {m_freeList.get(), size};
            m_used += size;
            UpdatePeak();
            ++m_metrics.totalAllocations;
            m_metrics.totalBytesRequested += size;
        } else {
            ++m_metrics.allocationFailures;
        }
        return ptr;
    }
    if (ptr) {
        m_routingTable[ptr] = {m_active, size};
        m_windowPtrs.back() = ptr;
        m_used += size;
        UpdatePeak();
        ++m_metrics.totalAllocations;
        m_metrics.totalBytesRequested += size;
    } else {
        ++m_metrics.allocationFailures;
    }
    return ptr;
}

// ── Free ─────────────────────────────────────────────────────────────────────
void AdaptiveAllocator::Free(void* ptr) {
    if (!ptr) return;
    auto it = m_routingTable.find(ptr);
    if (it == m_routingTable.end()) {
        // Try active allocator as fallback
        if (m_active) m_active->Free(ptr);
        return;
    }
    it->second.alloc->Free(ptr);
    m_used -= it->second.size;
    m_routingTable.erase(it);
    ++m_metrics.totalDeallocations;
}

// ── Report ────────────────────────────────────────────────────────────────────
void AdaptiveAllocator::PrintAdaptiveReport(std::ostream& out) const {
    out << "\n╔══════════════════════════════════════════════╗\n";
    out << "║       Adaptive Allocator Analysis Report     ║\n";
    out << "╚══════════════════════════════════════════════╝\n";
    out << "  Current strategy  : " << StrategyName(m_current) << "\n";
    out << "  Total allocations : " << m_profile.totalAllocs << "\n";
    out << "  Avg size          : " << std::fixed << std::setprecision(1) << m_profile.avgSize << " B\n";
    out << "  Size CV           : " << std::setprecision(3) << m_profile.sizeCV
        << "  (0=uniform, 1=varied)\n";
    out << "  Uniformity score  : " << std::setprecision(3) << m_profile.uniformityScore << "\n";
    out << "  LIFO score        : " << std::setprecision(3) << m_profile.lifoScore << "\n";
    out << "  Fragmentation     : " << std::setprecision(3) << m_profile.fragmentation << "\n";

    if (!m_switchLog.empty()) {
        out << "\n  Strategy switches (" << m_switchLog.size() << "):\n";
        for (auto& e : m_switchLog) {
            out << "    @alloc#" << std::setw(8) << e.atAlloc
                << "  " << StrategyName(e.from) << " → " << StrategyName(e.to)
                << "  [" << e.reason << "]\n";
        }
    }
    out << "\n";
}
