#include "LinearAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <iostream>

LinearAllocator::~LinearAllocator() {
    std::free(m_start_ptr);
    m_start_ptr = nullptr;
}

void LinearAllocator::Init() {
    if (m_start_ptr) { std::free(m_start_ptr); }
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr && "LinearAllocator::Init - malloc failed");
    Reset();
}

void LinearAllocator::Reset() {
    m_offset = 0;
    m_used   = 0;
    m_peak   = 0;
    m_metrics = {};
}

void* LinearAllocator::Allocate(std::size_t size, std::size_t alignment) {
    const std::size_t currentAddress = reinterpret_cast<std::size_t>(m_start_ptr) + m_offset;
    std::size_t padding = 0;
    if (alignment > 1 && (currentAddress % alignment) != 0) {
        padding = Utils::CalculatePadding(currentAddress, alignment);
    }
    if (m_offset + padding + size > m_totalSize) {
        ++m_metrics.allocationFailures;
        return nullptr;
    }
    m_offset += padding;
    void* result = reinterpret_cast<void*>(currentAddress + padding);
    m_offset += size;
    m_used    = m_offset;
    UpdatePeak();

    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += size + padding;
    m_metrics.totalPaddingWaste   += padding;
    return result;
}

void LinearAllocator::Free(void* /*ptr*/) {
    // Linear allocators do not support individual frees.
    // Bulk reclaim is done via Reset(). Silently ignore for benchmark compatibility.
}
