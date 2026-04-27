#pragma once
#include "Allocator.h"
#include <array>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// BuddyAllocator  –  classic binary buddy system
//
// Properties
//   • Every allocation is rounded up to the next power-of-2 block size
//   • Blocks are split into two equal "buddies" until the right size is reached
//   • On free, buddies are merged (coalesced) bottom-up in O(log N)
//   • External fragmentation stays bounded; internal fragmentation ≤ 50%
//
// Data structures
//   • m_freeLists[order]: doubly-linked list of free blocks at that order
//   • m_bitmap: 1 bit per minimum-size (MIN_BLOCK_SIZE) slot,
//               XOR-based buddy tracking (bit = 0 → both same state,
//                                         bit = 1 → buddies differ)
// ─────────────────────────────────────────────────────────────────────────────
class BuddyAllocator : public Allocator {
public:
    static constexpr std::size_t MIN_BLOCK_SIZE = 32;   // 2^5 bytes
    static constexpr int         MIN_ORDER      = 5;    // log2(MIN_BLOCK_SIZE)
    static constexpr int         MAX_ORDER      = 30;   // up to 1 GiB
    static constexpr int         NUM_LEVELS     = MAX_ORDER - MIN_ORDER + 1;

    // Intrusive doubly-linked list node embedded in each free block
    struct FreeBlock {
        FreeBlock* prev = nullptr;
        FreeBlock* next = nullptr;
    };

    explicit BuddyAllocator(std::size_t totalSize);
    ~BuddyAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;

    // Diagnostics
    void        PrintFreeList() const;
    std::size_t FreeBlockCount(int order) const;
    double      GetExternalFragmentation() const;

private:
    std::array<FreeBlock*, NUM_LEVELS> m_freeLists{};
    uint8_t* m_bitmap = nullptr;
    int      m_maxOrder = 0;

    int     SizeToOrder(std::size_t size)                    const;
    void    PushFree(int order, FreeBlock* block);
    void    RemoveFree(int order, FreeBlock* block);
    FreeBlock* PopFree(int order);
    FreeBlock* FindBuddy(FreeBlock* block, int order)        const;
    void    ToggleBuddyBit(FreeBlock* block, int order);
    bool    IsBuddyFree(FreeBlock* block, int order)         const;
    std::size_t OrderSize(int order)                         const { return std::size_t(1) << order; }
    std::size_t BlockIndex(FreeBlock* block, int order)      const;
};
