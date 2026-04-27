#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>

namespace Utils {

// ── Alignment helpers ────────────────────────────────────────────────────────

inline std::size_t CalculatePadding(std::size_t baseAddress, std::size_t alignment) {
    if (alignment == 0) return 0;
    const std::size_t mod = baseAddress % alignment;
    return (mod == 0) ? 0 : alignment - mod;
}

inline std::size_t CalculatePaddingWithHeader(std::size_t baseAddress,
                                               std::size_t alignment,
                                               std::size_t headerSize) {
    std::size_t padding     = CalculatePadding(baseAddress, alignment);
    std::size_t neededSpace = headerSize;
    if (padding < neededSpace) {
        neededSpace -= padding;
        padding += alignment * ((neededSpace + alignment - 1) / alignment);
    }
    return padding;
}

inline std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    if (alignment == 0) return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

inline std::size_t AlignDown(std::size_t value, std::size_t alignment) {
    if (alignment == 0) return value;
    return value & ~(alignment - 1);
}

// ── Power-of-2 helpers ───────────────────────────────────────────────────────

inline bool IsPowerOf2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

inline std::size_t NextPowerOf2(std::size_t n) {
    if (n == 0) return 1;
    --n;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16; n |= n >> 32;
    return ++n;
}

// floor(log2(n)),  n must be > 0
inline int FloorLog2(std::size_t n) {
    assert(n > 0 && "FloorLog2: argument must be > 0");
    return 63 - __builtin_clzll(static_cast<unsigned long long>(n));
}

// ── Bit-scan helpers (used by TLSF) ─────────────────────────────────────────

// Index of lowest set bit  (undefined if bitmap == 0)
inline int BitScanForward(uint32_t bitmap) {
    assert(bitmap != 0);
    return __builtin_ctz(bitmap);
}
// Index of highest set bit (undefined if bitmap == 0)
inline int BitScanReverse(uint32_t bitmap) {
    assert(bitmap != 0);
    return 31 - __builtin_clz(bitmap);
}

// ── Canary / fill patterns ───────────────────────────────────────────────────
inline constexpr uint8_t  FILL_ALLOC = 0xCD; // freshly allocated memory
inline constexpr uint8_t  FILL_FREE  = 0xFD; // freed memory
inline constexpr uint64_t CANARY_FRONT = 0xDEADBEEFCAFEBABEULL;
inline constexpr uint64_t CANARY_BACK  = 0xBAADF00DDEADC0DEULL;

} // namespace Utils
