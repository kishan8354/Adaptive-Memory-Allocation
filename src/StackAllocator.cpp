#include "StackAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <iostream>

StackAllocator::~StackAllocator() {
    std::free(m_start_ptr);
    m_start_ptr = nullptr;
}

void StackAllocator::Init() {
    if (m_start_ptr) { std::free(m_start_ptr); }
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr && "StackAllocator::Init - malloc failed");
    Reset();
}

void StackAllocator::Reset() {
    m_offset = 0;
    m_used   = 0;
    m_peak   = 0;
    m_metrics = {};
    m_markers.clear();
    m_checkpoints.clear();
}

void* StackAllocator::Allocate(std::size_t size, std::size_t alignment) {
    const std::size_t currentAddress = reinterpret_cast<std::size_t>(m_start_ptr) + m_offset;
    std::size_t padding = 0;
    if (alignment > 1) {
        padding = Utils::CalculatePadding(currentAddress, alignment);
    }
    if (m_offset + padding + size > m_totalSize) {
        ++m_metrics.allocationFailures;
        return nullptr;
    }
    m_markers.push_back(m_offset);       // rollback point for this alloc
    m_offset += padding + size;
    m_used    = m_offset;
    UpdatePeak();

    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += size + padding;
    m_metrics.totalPaddingWaste   += padding;

    return reinterpret_cast<void*>(currentAddress + padding);
}

void StackAllocator::Free(void* /*ptr*/) {
    // Stack allocators reclaim memory via Pop(marker) / Reset().
    // Individual per-pointer Free() is not supported; silently ignore.
}

// Push saves the current offset as a checkpoint (stack-frame begin)
std::size_t StackAllocator::Push() {
    m_checkpoints.push_back(m_offset);
    return m_offset;
}

// Pop rewinds to the given marker, freeing everything allocated since Push()
void StackAllocator::Pop(std::size_t marker) {
    assert(marker <= m_offset && "StackAllocator::Pop – marker out of range");
    // Remove any per-allocation markers that are now above `marker`
    while (!m_markers.empty() && m_markers.back() >= marker) {
        m_markers.pop_back();
    }
    // Remove checkpoints that are at or above `marker`
    while (!m_checkpoints.empty() && m_checkpoints.back() >= marker) {
        m_checkpoints.pop_back();
    }
    m_offset = marker;
    m_used   = m_offset;
}
