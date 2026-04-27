#pragma once
#include "Allocator.h"
#include "LinkedList.h"

// ─────────────────────────────────────────────────────────────────────────────
// FreeListAllocator  –  general-purpose allocator backed by a sorted
// singly-linked free-list with optional First-Fit or Best-Fit search.
// Coalescence merges adjacent free blocks on every Free().
// ─────────────────────────────────────────────────────────────────────────────
class FreeListAllocator : public Allocator {
public:
    enum class PlacementPolicy { FIND_FIRST, FIND_BEST };

    FreeListAllocator(std::size_t totalSize,
                      PlacementPolicy policy = PlacementPolicy::FIND_FIRST)
        : Allocator(totalSize, "FreeList"), m_pPolicy(policy) {}
    ~FreeListAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;

    // Diagnostics
    [[nodiscard]] std::size_t GetFreeBlockCount() const;
    [[nodiscard]] double      GetExternalFragmentation() const;

private:
    struct AllocationHeader {
        std::size_t blockSize;
        std::size_t padding;
    };
    struct FreeHeader {
        std::size_t blockSize;
    };

    PlacementPolicy  m_pPolicy;
    SinglyLinkedList m_freeList;

    void Find(std::size_t size, std::size_t alignment,
              std::size_t& padding, Node*& prevNode, Node*& foundNode);
    void FindFirst(std::size_t size, std::size_t alignment,
                   std::size_t& padding, Node*& prevNode, Node*& foundNode);
    void FindBest(std::size_t size, std::size_t alignment,
                  std::size_t& padding, Node*& prevNode, Node*& foundNode);
    void Coalescence(Node* prevNode, Node* freeNode);
};
