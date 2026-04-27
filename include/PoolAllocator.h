#pragma once
#include "Allocator.h"
#include "LinkedList.h"

class PoolAllocator : public Allocator {
public:
    PoolAllocator(std::size_t totalSize, std::size_t chunkSize)
        : Allocator(totalSize, "Pool"), m_chunkSize(chunkSize) {}
    ~PoolAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;

    [[nodiscard]] std::size_t GetChunkSize()   const { return m_chunkSize; }
    [[nodiscard]] std::size_t GetFreeChunks()  const;

private:
    std::size_t       m_chunkSize;
    std::size_t       m_offset = 0;
    StackLinkedList   m_freeList;
};
