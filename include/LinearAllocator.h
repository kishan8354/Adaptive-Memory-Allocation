#pragma once
#include "Allocator.h"

class LinearAllocator : public Allocator {
public:
    explicit LinearAllocator(std::size_t totalSize)
        : Allocator(totalSize, "Linear") {}
    ~LinearAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;   // No-op – use Reset()

private:
    std::size_t m_offset = 0;
};
