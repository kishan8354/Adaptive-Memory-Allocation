#include "PoolAllocator.h"
#include <cstdlib>
#include <cassert>
#include <algorithm>

PoolAllocator::~PoolAllocator() {
    std::free(m_start_ptr);
}

void PoolAllocator::Init() {
    assert(m_chunkSize >= sizeof(StackLinkedList::Node) && "Chunk too small for free-list node");
    assert(m_totalSize % m_chunkSize == 0 && "totalSize must be multiple of chunkSize");
    if (m_start_ptr) std::free(m_start_ptr);
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr && "PoolAllocator::Init – malloc failed");
    Reset();
}

void PoolAllocator::Reset() {
    m_offset    = 0;
    m_used      = 0;
    m_peak      = 0;
    m_metrics   = {};
    m_freeList.head = nullptr;
}

void* PoolAllocator::Allocate(std::size_t size, std::size_t /*alignment*/) {
    if (size > m_chunkSize) {
        fprintf(stderr, "[PoolAllocator] size=%zu > chunkSize=%zu — returning nullptr\n",
                size, m_chunkSize);
        ++m_metrics.allocationFailures;
        return nullptr;
    }

    // Pop from free list first
    StackLinkedList::Node* node = m_freeList.pop();
    if (!node) {
        // Bump-allocate from the raw pool
        if (m_offset >= m_totalSize / m_chunkSize) {
            ++m_metrics.allocationFailures;
            return nullptr;
        }
        node = reinterpret_cast<StackLinkedList::Node*>(
            reinterpret_cast<uint8_t*>(m_start_ptr) + m_offset * m_chunkSize);
        ++m_offset;
    }
    m_used += m_chunkSize;
    UpdatePeak();

    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += m_chunkSize;
    m_metrics.totalPaddingWaste   += (m_chunkSize - size);
    return static_cast<void*>(node);
}

void PoolAllocator::Free(void* ptr) {
    if (!ptr) return;
    m_freeList.push(static_cast<StackLinkedList::Node*>(ptr));
    m_used -= m_chunkSize;
    ++m_metrics.totalDeallocations;
}

std::size_t PoolAllocator::GetFreeChunks() const {
    // Count free-list length + unallocated bump space
    std::size_t count = m_totalSize / m_chunkSize - m_offset;
    StackLinkedList::Node* n = m_freeList.head;
    while (n) { ++count; n = n->next; }
    return count;
}
