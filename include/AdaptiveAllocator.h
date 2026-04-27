#pragma once
#include "Allocator.h"
#include <memory>
#include <unordered_map>
#include <deque>
#include <array>
#include <vector>
#include <cstdint>
#include <string>

// Forward declarations
class LinearAllocator;
class PoolAllocator;
class FreeListAllocator;
class TLSFAllocator;
class BuddyAllocator;

// ─────────────────────────────────────────────────────────────────────────────
// AdaptiveAllocator  –  NOVEL CONTRIBUTION
//
// An online-learning allocator that analyses the running workload and
// transparently routes allocations to the most suitable underlying allocator.
//
// Design overview
//   1. Workload sampling: every WINDOW allocations, update the WorkloadProfile
//   2. Pattern detection:  size uniformity, LIFO score, burst detection
//   3. Strategy scoring:   each candidate strategy is scored against the profile
//   4. Migration:          when a better strategy is found, switch on next window
//   5. Routing:            every Free() is routed to the allocator that served
//                          the matching Allocate() via m_routingTable
//
// Metrics tracked per window
//   • avgSize, sizeVariance, coefficient-of-variation (CV)
//   • LIFO score  (how often the last-allocated ptr is the first freed)
//   • uniformity  (fraction of allocations within ±10% of modal size class)
//   • allocation rate  (ops/sec)
//   • current fragmentation
// ─────────────────────────────────────────────────────────────────────────────
class AdaptiveAllocator : public Allocator {
public:
    enum class Strategy { LINEAR, STACK, POOL, FREELIST, TLSF, BUDDY };
    static const char* StrategyName(Strategy s);

    static constexpr int    WINDOW_SIZE = 512;   // re-analyse every N allocations
    static constexpr int    HISTORY_LEN = 4;     // windows of history kept

    struct WorkloadProfile {
        double avgSize          = 0.0;
        double sizeCV           = 0.0;   // coefficient of variation; 0 = uniform
        double lifoScore        = 0.0;   // [0,1]: 1 = perfect LIFO order
        double uniformityScore  = 0.0;   // [0,1]: 1 = all same size class
        double allocationRate   = 0.0;   // allocations per ms
        double fragmentation    = 0.0;

        uint64_t windowAllocs   = 0;
        uint64_t totalAllocs    = 0;
        std::size_t dominantSize = 64;   // most-frequent size in last window

        Strategy RecommendStrategy() const;
    };

    explicit AdaptiveAllocator(std::size_t totalSize);
    ~AdaptiveAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;

    void            ForceStrategy(Strategy s);
    Strategy        GetCurrentStrategy() const { return m_current; }
    const WorkloadProfile& GetProfile()  const { return m_profile; }
    void            PrintAdaptiveReport(std::ostream& out) const;

private:
    Strategy        m_current = Strategy::FREELIST;  // safe default
    bool            m_forced  = false;
    WorkloadProfile m_profile;

    // Sub-allocators (lazy-initialised)
    std::unique_ptr<LinearAllocator>   m_linear;
    std::unique_ptr<PoolAllocator>     m_pool;
    std::unique_ptr<FreeListAllocator> m_freeList;
    std::unique_ptr<TLSFAllocator>     m_tlsf;
    std::unique_ptr<BuddyAllocator>    m_buddy;

    Allocator* m_active = nullptr;  // points to one of the above

    // Routing table: maps user ptr → allocator that served it
    struct RouteEntry { Allocator* alloc; std::size_t size; };
    std::unordered_map<void*, RouteEntry> m_routingTable;

    // Sampling window
    std::deque<std::size_t> m_windowSizes;   // recent allocation sizes
    std::deque<void*>       m_windowPtrs;    // recent allocation pointers (LIFO score)
    uint64_t m_windowStart = 0;              // monotonic clock (ns) at window start

    // Per-strategy switch history (for reporting)
    struct SwitchEvent { Strategy from, to; uint64_t atAlloc; std::string reason; };
    std::vector<SwitchEvent> m_switchLog;

    // ── Internal helpers ─────────────────────────────────────────────────
    void     AnalyseWindow();
    void     SwitchTo(Strategy s, const char* reason);
    Allocator* AllocatorForStrategy(Strategy s);
    void     EnsureInitialised(Strategy s);

    double   ComputeLIFOScore()       const;
    double   ComputeUniformityScore() const;
    double   ComputeSizeCV()          const;
    void     UpdateProfile(std::size_t size);
};
