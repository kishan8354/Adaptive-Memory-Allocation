#include "FreeListAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cassert>
#include <limits>
#include <algorithm>

FreeListAllocator::~FreeListAllocator() {
    std::free(m_start_ptr);
    m_start_ptr = nullptr;
}

void FreeListAllocator::Init() {
    if (m_start_ptr) { std::free(m_start_ptr); }
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr && "FreeListAllocator::Init – malloc failed");
    Reset();
}

void FreeListAllocator::Reset() {
    m_used = m_peak = 0;
    m_metrics = {};
    auto* first = reinterpret_cast<Node*>(m_start_ptr);
    first->data.blockSize = m_totalSize;
    first->next           = nullptr;
    m_freeList.head       = nullptr;
    m_freeList.insert(nullptr, first);
}

// ── Find helpers ─────────────────────────────────────────────────────────────
void FreeListAllocator::FindFirst(std::size_t size, std::size_t alignment,
                                   std::size_t& padding, Node*& prevNode, Node*& foundNode) {
    Node* it     = m_freeList.head;
    Node* itPrev = nullptr;
    while (it) {
        padding = Utils::CalculatePaddingWithHeader(
            reinterpret_cast<std::size_t>(it), alignment, sizeof(AllocationHeader));
        if (it->data.blockSize >= size + padding) break;
        itPrev = it; it = it->next;
    }
    prevNode  = itPrev;
    foundNode = it;
}

void FreeListAllocator::FindBest(std::size_t size, std::size_t alignment,
                                  std::size_t& padding, Node*& prevNode, Node*& foundNode) {
    std::size_t smallest = std::numeric_limits<std::size_t>::max();
    Node* best = nullptr, *bestPrev = nullptr;
    Node* it = m_freeList.head, *itPrev = nullptr;
    while (it) {
        std::size_t pad = Utils::CalculatePaddingWithHeader(
            reinterpret_cast<std::size_t>(it), alignment, sizeof(AllocationHeader));
        std::size_t req = size + pad;
        if (it->data.blockSize >= req && (it->data.blockSize - req) < smallest) {
            smallest = it->data.blockSize - req;
            best     = it;
            bestPrev = itPrev;
        }
        itPrev = it; it = it->next;
    }
    padding   = (best) ? Utils::CalculatePaddingWithHeader(
        reinterpret_cast<std::size_t>(best), alignment, sizeof(AllocationHeader)) : 0;
    prevNode  = bestPrev;
    foundNode = best;
}

void FreeListAllocator::Find(std::size_t size, std::size_t alignment,
                              std::size_t& padding, Node*& prevNode, Node*& foundNode) {
    if (m_pPolicy == PlacementPolicy::FIND_BEST)
        FindBest(size, alignment, padding, prevNode, foundNode);
    else
        FindFirst(size, alignment, padding, prevNode, foundNode);
}

// ── Allocate ─────────────────────────────────────────────────────────────────
void* FreeListAllocator::Allocate(std::size_t size, std::size_t alignment) {
    assert(size >= sizeof(Node) && "Allocation too small");
    assert(alignment >= 8      && "Alignment must be >= 8");

    std::size_t padding;
    Node *prevNode, *affectedNode;
    Find(size, alignment, padding, prevNode, affectedNode);
    if (!affectedNode) {
        ++m_metrics.allocationFailures;
        return nullptr;
    }

    const std::size_t alignPadding  = padding - sizeof(AllocationHeader);
    const std::size_t requiredSize  = size + padding;
    const std::size_t rest          = affectedNode->data.blockSize - requiredSize;

    if (rest > sizeof(Node)) {
        auto* newFree = reinterpret_cast<Node*>(
            reinterpret_cast<std::size_t>(affectedNode) + requiredSize);
        newFree->data.blockSize = rest;
        m_freeList.insert(affectedNode, newFree);
    }
    m_freeList.remove(prevNode, affectedNode);

    const std::size_t headerAddr = reinterpret_cast<std::size_t>(affectedNode) + alignPadding;
    const std::size_t dataAddr   = headerAddr + sizeof(AllocationHeader);

    auto* hdr = reinterpret_cast<AllocationHeader*>(headerAddr);
    hdr->blockSize = requiredSize;
    hdr->padding   = alignPadding;

    m_used += requiredSize;
    UpdatePeak();

    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += requiredSize;
    m_metrics.totalPaddingWaste   += alignPadding;
    m_metrics.totalHeaderWaste    += sizeof(AllocationHeader);

    return reinterpret_cast<void*>(dataAddr);
}

// ── Coalescence ──────────────────────────────────────────────────────────────
void FreeListAllocator::Coalescence(Node* prevNode, Node* freeNode) {
    // Merge with next block if contiguous
    if (freeNode->next &&
        (reinterpret_cast<std::size_t>(freeNode) + freeNode->data.blockSize ==
         reinterpret_cast<std::size_t>(freeNode->next))) {
        freeNode->data.blockSize += freeNode->next->data.blockSize;
        m_freeList.remove(freeNode, freeNode->next);
    }
    // Merge with previous block if contiguous
    if (prevNode &&
        (reinterpret_cast<std::size_t>(prevNode) + prevNode->data.blockSize ==
         reinterpret_cast<std::size_t>(freeNode))) {
        prevNode->data.blockSize += freeNode->data.blockSize;
        m_freeList.remove(prevNode, freeNode);
    }
}

// ── Free ─────────────────────────────────────────────────────────────────────
void FreeListAllocator::Free(void* ptr) {
    if (!ptr) return;
    const std::size_t hdrAddr = reinterpret_cast<std::size_t>(ptr) - sizeof(AllocationHeader);
    auto* hdr = reinterpret_cast<AllocationHeader*>(hdrAddr);

    auto* freeNode = reinterpret_cast<Node*>(hdrAddr);
    freeNode->data.blockSize = hdr->blockSize + hdr->padding;
    freeNode->next           = nullptr;

    Node* it = m_freeList.head, *itPrev = nullptr;
    while (it && ptr > static_cast<void*>(it)) {
        itPrev = it; it = it->next;
    }
    m_freeList.insert(itPrev, freeNode);
    m_used -= freeNode->data.blockSize;
    ++m_metrics.totalDeallocations;
    Coalescence(itPrev, freeNode);
}

// ── Diagnostics ──────────────────────────────────────────────────────────────
std::size_t FreeListAllocator::GetFreeBlockCount() const {
    std::size_t count = 0;
    Node* n = m_freeList.head;
    while (n) { ++count; n = n->next; }
    return count;
}

double FreeListAllocator::GetExternalFragmentation() const {
    if (m_totalSize == 0 || m_used == m_totalSize) return 0.0;
    // Find the largest free block
    std::size_t largest = 0;
    Node* n = m_freeList.head;
    while (n) { if (n->data.blockSize > largest) largest = n->data.blockSize; n = n->next; }
    const std::size_t totalFree = m_totalSize - m_used;
    if (totalFree == 0) return 0.0;
    return 1.0 - static_cast<double>(largest) / static_cast<double>(totalFree);
}
