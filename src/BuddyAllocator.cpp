#include "BuddyAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <iostream>
#include <algorithm>

BuddyAllocator::BuddyAllocator(std::size_t totalSize)
    : Allocator(totalSize, "Buddy") {
    assert(Utils::IsPowerOf2(totalSize) && "BuddyAllocator: totalSize must be a power of 2");
    m_maxOrder = Utils::FloorLog2(totalSize);
    assert(m_maxOrder >= MIN_ORDER && m_maxOrder <= MAX_ORDER);
}

BuddyAllocator::~BuddyAllocator() {
    std::free(m_start_ptr);
    std::free(m_bitmap);
}

void BuddyAllocator::Init() {
    if (m_start_ptr) { std::free(m_start_ptr); std::free(m_bitmap); m_bitmap = nullptr; }
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr);
    Reset();
}

void BuddyAllocator::Reset() {
    m_used = 0; m_peak = 0; m_metrics = {};
    m_freeLists.fill(nullptr);
    if (!m_start_ptr) return;
    auto* root = reinterpret_cast<FreeBlock*>(m_start_ptr);
    root->prev = root->next = nullptr;
    m_freeLists[m_maxOrder - MIN_ORDER] = root;
}

int BuddyAllocator::SizeToOrder(std::size_t size) const {
    size += 1;  // 1-byte header overhead
    if (size <= MIN_BLOCK_SIZE) return MIN_ORDER;
    int ord = Utils::FloorLog2(size - 1) + 1;
    return std::min(ord, m_maxOrder);
}

void BuddyAllocator::PushFree(int order, FreeBlock* block) {
    int idx = order - MIN_ORDER;
    block->prev = nullptr;
    block->next = m_freeLists[idx];
    if (m_freeLists[idx]) m_freeLists[idx]->prev = block;
    m_freeLists[idx] = block;
}

void BuddyAllocator::RemoveFree(int order, FreeBlock* block) {
    int idx = order - MIN_ORDER;
    if (block->prev) block->prev->next = block->next;
    else             m_freeLists[idx]  = block->next;
    if (block->next) block->next->prev = block->prev;
    block->prev = block->next = nullptr;
}

BuddyAllocator::FreeBlock* BuddyAllocator::PopFree(int order) {
    FreeBlock* blk = m_freeLists[order - MIN_ORDER];
    if (!blk) return nullptr;
    RemoveFree(order, blk);
    return blk;
}

BuddyAllocator::FreeBlock* BuddyAllocator::FindBuddy(FreeBlock* block, int order) const {
    std::size_t offset = reinterpret_cast<uint8_t*>(block) -
                         reinterpret_cast<uint8_t*>(m_start_ptr);
    std::size_t buddyOff = offset ^ OrderSize(order);
    return reinterpret_cast<FreeBlock*>(
        reinterpret_cast<uint8_t*>(m_start_ptr) + buddyOff);
}

std::size_t BuddyAllocator::BlockIndex(FreeBlock* block, int order) const {
    std::size_t offset = reinterpret_cast<uint8_t*>(block) -
                         reinterpret_cast<uint8_t*>(m_start_ptr);
    return offset / OrderSize(order);
}

// Linear free-list search: avoids the bitmap collision bug entirely
bool BuddyAllocator::IsBuddyFree(FreeBlock* block, int order) const {
    FreeBlock* buddy = FindBuddy(block, order);
    FreeBlock* cur   = m_freeLists[order - MIN_ORDER];
    while (cur) {
        if (cur == buddy) return true;
        cur = cur->next;
    }
    return false;
}

void BuddyAllocator::ToggleBuddyBit(FreeBlock*, int) {}  // stub, bitmap removed

void* BuddyAllocator::Allocate(std::size_t size, std::size_t) {
    if (size == 0) size = 1;
    int reqOrder = SizeToOrder(size);
    if (reqOrder > m_maxOrder) { ++m_metrics.allocationFailures; return nullptr; }

    int order = reqOrder;
    while (order <= m_maxOrder && !m_freeLists[order - MIN_ORDER]) ++order;
    if (order > m_maxOrder)     { ++m_metrics.allocationFailures; return nullptr; }

    FreeBlock* block = PopFree(order);
    while (order > reqOrder) {
        --order;
        auto* buddy = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<uint8_t*>(block) + OrderSize(order));
        PushFree(order, buddy);
    }

    m_used += OrderSize(order);
    UpdatePeak();
    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += OrderSize(order);
    m_metrics.totalPaddingWaste   += (OrderSize(order) - size - 1);

    uint8_t* raw = reinterpret_cast<uint8_t*>(block);
    *raw = static_cast<uint8_t>(order);
    return raw + 1;
}

void BuddyAllocator::Free(void* ptr) {
    if (!ptr) return;

    uint8_t* rawPtr = reinterpret_cast<uint8_t*>(ptr) - 1;
    int order = static_cast<int>(*rawPtr);
    if (order < MIN_ORDER || order > m_maxOrder) {
        assert(false && "BuddyAllocator::Free – corrupt order byte");
        return;
    }

    auto* block = reinterpret_cast<FreeBlock*>(rawPtr);
    m_used -= OrderSize(order);
    ++m_metrics.totalDeallocations;

    while (order < m_maxOrder) {
        if (!IsBuddyFree(block, order)) break;
        FreeBlock* buddy = FindBuddy(block, order);
        RemoveFree(order, buddy);
        if (buddy < block) block = buddy;
        ++order;
    }
    PushFree(order, block);
}

std::size_t BuddyAllocator::FreeBlockCount(int order) const {
    std::size_t cnt = 0;
    for (FreeBlock* n = m_freeLists[order - MIN_ORDER]; n; n = n->next) ++cnt;
    return cnt;
}

double BuddyAllocator::GetExternalFragmentation() const {
    std::size_t totalFree = m_totalSize - m_used;
    if (totalFree == 0) return 0.0;
    std::size_t largest = 0;
    for (int o = m_maxOrder; o >= MIN_ORDER; --o) {
        if (m_freeLists[o - MIN_ORDER]) { largest = OrderSize(o); break; }
    }
    return 1.0 - static_cast<double>(largest) / static_cast<double>(totalFree);
}

void BuddyAllocator::PrintFreeList() const {
    std::cout << "BuddyAllocator free lists:\n";
    for (int o = MIN_ORDER; o <= m_maxOrder; ++o) {
        std::size_t cnt = FreeBlockCount(o);
        if (cnt) std::cout << "  order=" << o << " size=" << OrderSize(o)
                           << " blocks=" << cnt << "\n";
    }
}
