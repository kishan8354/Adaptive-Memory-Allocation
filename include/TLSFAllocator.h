#pragma once
#include "Allocator.h"
#include <array>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// TLSFAllocator  –  Two-Level Segregated Fit
//   Based on: Masmano et al., "TLSF: A New Dynamic Memory Allocator for
//             Real-Time Systems", ECRTS 2004
//
// Key properties
//   • O(1) allocation and deallocation (worst-case, not amortised)
//   • Maximum fragmentation overhead < 25%
//   • Suitable for hard real-time systems
//
// Architecture
//   FL (first-level): fl = floor(log2(size)), one bit in m_flBitmap
//   SL (second-level): subdivides each FL class into 2^SL_SHIFT sub-lists
//   Two bitmaps (fl + sl[]) allow O(1) search via bit-scan instructions
//
// Block layout (in-place, no external metadata)
//   [ BlockHeader | ... data ... | (back-link: prev-phys-size at end if free) ]
// ─────────────────────────────────────────────────────────────────────────────
class TLSFAllocator : public Allocator {
public:
    // Tuning parameters
    static constexpr int SL_SHIFT       = 5;                      // 32 sub-classes per FL
    static constexpr int SL_COUNT       = 1 << SL_SHIFT;
    static constexpr int FL_INDEX_MAX   = 32;
    static constexpr int FL_INDEX_SHIFT = SL_SHIFT + 1;           // smallest FL class = 2^(SL_SHIFT+1)
    static constexpr int FL_COUNT       = FL_INDEX_MAX - FL_INDEX_SHIFT + 1;
    static constexpr std::size_t MIN_ALLOC_SIZE = 1 << FL_INDEX_SHIFT; // 64 bytes

    // ── Block header ─────────────────────────────────────────────────────
    struct BlockHeader {
        // Physical size of the PREVIOUS block (0 if this is the first block)
        // Stored so we can navigate to the previous block in O(1)
        std::size_t prevPhysSize;
        // Current block's size | flags in lower 2 bits
        //   bit 0 → FREE (1) / USED (0)
        //   bit 1 → PREV_FREE (1) / PREV_USED (0)
        std::size_t sizeAndFlags;

        // Free-list links (only valid when block is FREE)
        BlockHeader* prevFree;
        BlockHeader* nextFree;

        static constexpr std::size_t FLAG_FREE      = 1ULL;
        static constexpr std::size_t FLAG_PREV_FREE = 2ULL;
        static constexpr std::size_t MASK_SIZE      = ~std::size_t(3);

        std::size_t  Size()         const { return sizeAndFlags & MASK_SIZE; }
        bool         IsFree()       const { return sizeAndFlags & FLAG_FREE; }
        bool         IsPrevFree()   const { return sizeAndFlags & FLAG_PREV_FREE; }
        void         SetSize(std::size_t s) { sizeAndFlags = (sizeAndFlags & ~MASK_SIZE) | (s & MASK_SIZE); }
        void         SetFree()             { sizeAndFlags |=  FLAG_FREE; }
        void         SetUsed()             { sizeAndFlags &= ~FLAG_FREE; }
        void         SetPrevFree()         { sizeAndFlags |=  FLAG_PREV_FREE; }
        void         SetPrevUsed()         { sizeAndFlags &= ~FLAG_PREV_FREE; }

        // Pointer arithmetic helpers
        uint8_t*     DataPtr()      { return reinterpret_cast<uint8_t*>(this) + sizeof(BlockHeader); }
        BlockHeader* NextPhys()     {
            return reinterpret_cast<BlockHeader*>(DataPtr() + Size());
        }
    };

    explicit TLSFAllocator(std::size_t totalSize);
    ~TLSFAllocator() override;

    void  Init()    override;
    void  Reset()   override;
    void* Allocate(std::size_t size, std::size_t alignment = 8) override;
    void  Free(void* ptr) override;

    // Diagnostics
    [[nodiscard]] double GetExternalFragmentation() const;
    void PrintState() const;

private:
    uint32_t m_flBitmap = 0;
    std::array<uint32_t, FL_COUNT>                      m_slBitmap{};
    std::array<std::array<BlockHeader*, SL_COUNT>, FL_COUNT> m_lists{};

    // ── Index computation ─────────────────────────────────────────────────
    void MappingInsert(std::size_t size, int& fl, int& sl) const;
    void MappingSearch(std::size_t size, int& fl, int& sl) const;

    // ── Free-list management ──────────────────────────────────────────────
    void         InsertBlock(BlockHeader* block);
    void         RemoveBlock(BlockHeader* block, int fl, int sl);
    BlockHeader* FindSuitable(std::size_t size, int& fl, int& sl);

    // ── Block surgery ─────────────────────────────────────────────────────
    BlockHeader* Absorb(BlockHeader* prev, BlockHeader* block);
    BlockHeader* SplitBlock(BlockHeader* block, std::size_t size);
    BlockHeader* MergeBlock(BlockHeader* block);

    // ── Helpers ───────────────────────────────────────────────────────────
    std::size_t AdjustSize(std::size_t size) const;
    void        MarkFree(BlockHeader* block);
    void        MarkUsed(BlockHeader* block);
    void        LinkBlock(BlockHeader* block, int fl, int sl);
    void        UnlinkBlock(BlockHeader* block, int fl, int sl);
    // Write prev-phys-size into the footer of `block` (needed for backward coalescing)
    void        WritePrevPhys(BlockHeader* block);
};
