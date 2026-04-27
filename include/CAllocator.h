#pragma once
#include "Allocator.h"

// Thin wrapper around stdlib malloc/free – baseline for benchmarking
class CAllocator : public Allocator {
public:
    CAllocator() : Allocator(0, "C-malloc") {}
    void  Init()    override {}
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;
};
