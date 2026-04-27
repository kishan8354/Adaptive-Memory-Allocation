#pragma once
#include "Allocator.h"
#include <vector>

// Stack (LIFO) allocator with checkpoint-based rollback.
// Novel addition: Push()/Pop() for region-based bulk frees (scratchpad pattern).
class StackAllocator : public Allocator {
public:
    explicit StackAllocator(std::size_t totalSize)
        : Allocator(totalSize, "Stack") {}
    ~StackAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;  // LIFO order – asserts otherwise

    // Checkpoint API: push/pop entire regions (like a stack frame)
    std::size_t Push();
    void        Pop(std::size_t marker);

private:
    std::size_t              m_offset = 0;
    std::vector<std::size_t> m_markers;       // per-allocation rollback points
    std::vector<std::size_t> m_checkpoints;   // Push()/Pop() stack frames
};
