#include "TLSFAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <algorithm>

// ── Index computation ─────────────────────────────────────────────────────────
// fl = floor(log2(size))
// sl = (size >> (fl - SL_SHIFT)) - SL_COUNT   (the SL_SHIFT most-significant
//       sub-bits below the leading bit)
void TLSFAllocator::MappingInsert(std::size_t size, int& fl, int& sl) const {
    fl = Utils::FloorLog2(size);
    sl = static_cast<int>((size >> (fl - SL_SHIFT)) - SL_COUNT);
    fl = fl - FL_INDEX_SHIFT;
    fl = std::clamp(fl, 0, FL_COUNT - 1);
    sl = std::clamp(sl, 0, SL_COUNT - 1);
}

// For search we round UP to the next sub-class so we don't pick a block too small
void TLSFAllocator::MappingSearch(std::size_t size, int& fl, int& sl) const {
    // Round up to next sub-class boundary
    std::size_t rounded = size + (std::size_t(1) << (Utils::FloorLog2(size) - SL_SHIFT)) - 1;
    MappingInsert(rounded, fl, sl);
}

// ── Constructor / destructor ───────────────────────────────────────────────────
TLSFAllocator::TLSFAllocator(std::size_t totalSize)
    : Allocator(totalSize, "TLSF") {}

TLSFAllocator::~TLSFAllocator() {
    std::free(m_start_ptr);
}

// ── Init / Reset ──────────────────────────────────────────────────────────────
void TLSFAllocator::Init() {
    if (m_start_ptr) std::free(m_start_ptr);
    m_start_ptr = std::malloc(m_totalSize);
    assert(m_start_ptr && "TLSFAllocator::Init – malloc failed");
    Reset();
}

void TLSFAllocator::Reset() {
    m_used = m_peak = 0;
    m_metrics = {};
    m_flBitmap = 0;
    m_slBitmap.fill(0);
    for (auto& row : m_lists) row.fill(nullptr);

    if (!m_start_ptr) return;

    // Pool layout: [BlockHeader | userData | BlockHeader(sentinel)]
    // NextPhys() = this + sizeof(BlockHeader) + Size(), so sentinel needs
    // its OWN header beyond the userData area → reserve 2 × sizeof(BlockHeader).
    const std::size_t sentinelSize = 2 * sizeof(BlockHeader);
    const std::size_t poolSize     = m_totalSize - sentinelSize;

    auto* block = reinterpret_cast<BlockHeader*>(m_start_ptr);
    block->prevPhysSize = 0;
    block->prevFree     = nullptr;
    block->nextFree     = nullptr;
    block->SetSize(poolSize);
    block->SetFree();
    block->SetPrevUsed();  // before the pool there's nothing

    // Write prev-phys-size into sentinel's location
    auto* sentinel = block->NextPhys();
    sentinel->prevPhysSize = poolSize;
    sentinel->sizeAndFlags = 0;  // size=0, used

    InsertBlock(block);
}

// ── Free-list management ──────────────────────────────────────────────────────
void TLSFAllocator::LinkBlock(BlockHeader* block, int fl, int sl) {
    block->nextFree = m_lists[fl][sl];
    block->prevFree = nullptr;
    if (m_lists[fl][sl]) m_lists[fl][sl]->prevFree = block;
    m_lists[fl][sl] = block;
    m_flBitmap    |= (1u << fl);
    m_slBitmap[fl]|= (1u << sl);
}

void TLSFAllocator::UnlinkBlock(BlockHeader* block, int fl, int sl) {
    if (block->prevFree) block->prevFree->nextFree = block->nextFree;
    else                 m_lists[fl][sl]            = block->nextFree;
    if (block->nextFree) block->nextFree->prevFree  = block->prevFree;
    if (!m_lists[fl][sl]) {
        m_slBitmap[fl] &= ~(1u << sl);
        if (!m_slBitmap[fl]) m_flBitmap &= ~(1u << fl);
    }
    block->prevFree = block->nextFree = nullptr;
}

void TLSFAllocator::InsertBlock(BlockHeader* block) {
    int fl, sl;
    MappingInsert(block->Size(), fl, sl);
    LinkBlock(block, fl, sl);
}

void TLSFAllocator::RemoveBlock(BlockHeader* block, int fl, int sl) {
    UnlinkBlock(block, fl, sl);
}

// ── Find suitable block (O(1) via bitmaps) ────────────────────────────────────
TLSFAllocator::BlockHeader* TLSFAllocator::FindSuitable(std::size_t size, int& fl, int& sl) {
    MappingSearch(size, fl, sl);
    if (fl >= FL_COUNT) return nullptr;

    // Mask off smaller SL classes in this FL
    uint32_t slMasked = m_slBitmap[fl] & (~uint32_t(0) << sl);
    if (!slMasked) {
        // Look in a higher FL class
        uint32_t flMasked = m_flBitmap & (~uint32_t(0) << (fl + 1));
        if (!flMasked) return nullptr;
        fl = Utils::BitScanForward(flMasked);
        slMasked = m_slBitmap[fl];
    }
    sl = Utils::BitScanForward(slMasked);
    return m_lists[fl][sl];
}

// ── Block surgery ─────────────────────────────────────────────────────────────
void TLSFAllocator::WritePrevPhys(BlockHeader* block) {
    block->NextPhys()->prevPhysSize = block->Size();
}

// Merge `block` with the next physical block if it is free
TLSFAllocator::BlockHeader* TLSFAllocator::Absorb(BlockHeader* prev, BlockHeader* block) {
    // prev absorbs block
    int fl, sl;
    MappingInsert(block->Size(), fl, sl);
    UnlinkBlock(block, fl, sl);
    prev->SetSize(prev->Size() + sizeof(BlockHeader) + block->Size());
    WritePrevPhys(prev);
    return prev;
}

TLSFAllocator::BlockHeader* TLSFAllocator::MergeBlock(BlockHeader* block) {
    // Merge with previous if free
    if (block->IsPrevFree()) {
        auto* prev = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uint8_t*>(block) - block->prevPhysSize - sizeof(BlockHeader));
        int fl, sl;
        MappingInsert(prev->Size(), fl, sl);
        UnlinkBlock(prev, fl, sl);
        prev->SetSize(prev->Size() + sizeof(BlockHeader) + block->Size());
        WritePrevPhys(prev);
        block = prev;
    }
    // Merge with next if free
    auto* next = block->NextPhys();
    if (next->IsFree()) {
        block = Absorb(block, next);
    }
    return block;
}

TLSFAllocator::BlockHeader* TLSFAllocator::SplitBlock(BlockHeader* block, std::size_t size) {
    const std::size_t remaining = block->Size() - size - sizeof(BlockHeader);
    if (remaining < MIN_ALLOC_SIZE) return block; // not worth splitting

    // Create remainder block
    auto* remainder = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(block->DataPtr()) + size);
    remainder->prevPhysSize = size;
    remainder->SetSize(remaining);
    remainder->SetFree();
    remainder->SetPrevUsed();
    remainder->prevFree = remainder->nextFree = nullptr;
    WritePrevPhys(remainder);

    block->SetSize(size);
    WritePrevPhys(block);

    InsertBlock(remainder);
    return block;
}

std::size_t TLSFAllocator::AdjustSize(std::size_t size) const {
    return std::max(Utils::AlignUp(size, sizeof(void*)), MIN_ALLOC_SIZE);
}

// ── Allocate ──────────────────────────────────────────────────────────────────
void* TLSFAllocator::Allocate(std::size_t size, std::size_t alignment) {
    if (size == 0) return nullptr;
    std::size_t adjSize = AdjustSize(size);
    if (alignment > sizeof(void*)) {
        adjSize = Utils::AlignUp(adjSize + alignment, alignment);
    }

    int fl, sl;
    BlockHeader* block = FindSuitable(adjSize, fl, sl);
    if (!block) { ++m_metrics.allocationFailures; return nullptr; }

    RemoveBlock(block, fl, sl);
    block = SplitBlock(block, adjSize);

    MarkUsed(block);

    m_used += adjSize;
    UpdatePeak();
    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += size;
    m_metrics.totalBytesActual    += adjSize;
    m_metrics.totalPaddingWaste   += (adjSize - size);
    m_metrics.totalHeaderWaste    += sizeof(BlockHeader);

    return block->DataPtr();
}

// ── Free ─────────────────────────────────────────────────────────────────────
void TLSFAllocator::Free(void* ptr) {
    if (!ptr) return;
    auto* block = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(BlockHeader));

    const std::size_t sz = block->Size();
    MarkFree(block);
    block = MergeBlock(block);
    InsertBlock(block);

    m_used -= sz;
    ++m_metrics.totalDeallocations;
}

void TLSFAllocator::MarkFree(BlockHeader* block) {
    block->SetFree();
    block->NextPhys()->SetPrevFree();
}

void TLSFAllocator::MarkUsed(BlockHeader* block) {
    block->SetUsed();
    block->NextPhys()->SetPrevUsed();
}

double TLSFAllocator::GetExternalFragmentation() const {
    // Walk all free lists, find largest free block
    std::size_t totalFree = 0, largest = 0;
    for (int fl = 0; fl < FL_COUNT; ++fl) {
        for (int sl = 0; sl < SL_COUNT; ++sl) {
            BlockHeader* b = m_lists[fl][sl];
            while (b) {
                totalFree += b->Size();
                if (b->Size() > largest) largest = b->Size();
                b = b->nextFree;
            }
        }
    }
    if (totalFree == 0) return 0.0;
    return 1.0 - static_cast<double>(largest) / static_cast<double>(totalFree);
}

void TLSFAllocator::PrintState() const {
    std::cout << "TLSF FL bitmap: " << std::hex << m_flBitmap << std::dec << "\n";
    for (int fl = 0; fl < FL_COUNT; ++fl) {
        if (!m_slBitmap[fl]) continue;
        std::cout << "  FL[" << fl << "] SL bitmap: " << std::hex << m_slBitmap[fl] << std::dec << "\n";
    }
}
