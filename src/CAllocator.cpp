#include "CAllocator.h"
#include <cstdlib>
#include <algorithm>

void* CAllocator::Allocate(std::size_t size, std::size_t /*alignment*/) {
    void* ptr = std::malloc(size);
    if (ptr) {
        m_used += size;
        UpdatePeak();
        ++m_metrics.totalAllocations;
        m_metrics.totalBytesRequested += size;
        m_metrics.totalBytesActual    += size;
    } else {
        ++m_metrics.allocationFailures;
    }
    return ptr;
}

void CAllocator::Free(void* ptr) {
    if (!ptr) return;
    std::free(ptr);
    ++m_metrics.totalDeallocations;
}
