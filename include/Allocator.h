#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// AllocationMetrics  –  per-allocator statistics collected at runtime
// ─────────────────────────────────────────────────────────────────────────────
struct AllocationMetrics {
    uint64_t    totalAllocations   = 0;
    uint64_t    totalDeallocations = 0;
    uint64_t    allocationFailures = 0;
    std::size_t totalBytesRequested= 0;   // sum of user-requested sizes
    std::size_t totalBytesActual   = 0;   // sum of actually consumed bytes (with headers/padding)
    std::size_t totalPaddingWaste  = 0;   // alignment padding bytes wasted
    std::size_t totalHeaderWaste   = 0;   // metadata overhead bytes
    std::size_t peakUsage          = 0;

    // Internal fragmentation: padding / (padding + useful data)
    double InternalFragmentation() const {
        if (totalBytesActual == 0) return 0.0;
        return static_cast<double>(totalPaddingWaste + totalHeaderWaste)
               / static_cast<double>(totalBytesActual);
    }
    // Average wasted bytes per allocation
    double AvgWastePerAlloc() const {
        if (totalAllocations == 0) return 0.0;
        return static_cast<double>(totalPaddingWaste + totalHeaderWaste)
               / static_cast<double>(totalAllocations);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Allocator  –  abstract base class
// ─────────────────────────────────────────────────────────────────────────────
class Allocator {
public:
    std::size_t      m_totalSize;
    std::size_t      m_used  = 0;
    std::size_t      m_peak  = 0;
    void*            m_start_ptr = nullptr;
    std::string      m_name;
    AllocationMetrics m_metrics;

    explicit Allocator(std::size_t totalSize, std::string name = "Allocator")
        : m_totalSize(totalSize), m_name(std::move(name)) {}

    virtual ~Allocator() = default;

    virtual void  Init()                                              = 0;
    virtual void* Allocate(std::size_t size, std::size_t alignment=8) = 0;
    virtual void  Free(void* ptr)                                     = 0;
    virtual void  Reset() {}

    // Helpers
    [[nodiscard]] std::size_t GetTotalSize()  const { return m_totalSize; }
    [[nodiscard]] std::size_t GetUsed()       const { return m_used; }
    [[nodiscard]] std::size_t GetPeak()       const { return m_peak; }
    [[nodiscard]] std::size_t GetFree()       const { return m_totalSize > m_used ? m_totalSize - m_used : 0; }
    [[nodiscard]] const std::string& GetName() const { return m_name; }
    [[nodiscard]] double GetUtilization()     const {
        return m_totalSize ? static_cast<double>(m_used) / static_cast<double>(m_totalSize) : 0.0;
    }
    [[nodiscard]] const AllocationMetrics& GetMetrics() const { return m_metrics; }

protected:
    void UpdatePeak() { m_peak = std::max(m_peak, m_used); m_metrics.peakUsage = m_peak; }
};
