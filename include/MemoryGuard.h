#pragma once
#include "Utils.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// MemoryGuard  –  wraps every allocation with canary values to detect:
//   • buffer overflows / underflows
//   • use-after-free (freed memory is poisoned)
//   • header corruption
//
// Layout of a guarded allocation:
//   [ GuardHeader (24 B) | user data (N B) | back-canary (8 B) ]
// ─────────────────────────────────────────────────────────────────────────────
namespace MemoryGuard {

struct GuardHeader {
    uint64_t    frontCanary;     // must equal CANARY_FRONT
    std::size_t requestedSize;
    uint32_t    checksum;        // XOR-based integrity check
    uint32_t    _pad;

    uint32_t ComputeChecksum() const {
        return static_cast<uint32_t>(
            (frontCanary ^ requestedSize ^ reinterpret_cast<uintptr_t>(this)) & 0xFFFFFFFF);
    }
    bool IsValid() const {
        return frontCanary == Utils::CANARY_FRONT && checksum == ComputeChecksum();
    }
};

static_assert(sizeof(GuardHeader) <= 24, "GuardHeader too large");

// Overhead added to every allocation
inline std::size_t Overhead() {
    return sizeof(GuardHeader) + sizeof(uint64_t);  // header + back canary
}

// Wrap a raw block (rawPtr points to sizeof(GuardHeader) extra bytes before user data)
// Returns pointer user should receive
inline void* Wrap(void* rawPtr, std::size_t requestedSize) {
    if (!rawPtr) return nullptr;
    auto* hdr = static_cast<GuardHeader*>(rawPtr);
    hdr->frontCanary   = Utils::CANARY_FRONT;
    hdr->requestedSize = requestedSize;
    hdr->checksum      = hdr->ComputeChecksum();
    hdr->_pad          = 0;

    uint8_t* userData = reinterpret_cast<uint8_t*>(hdr) + sizeof(GuardHeader);
    std::memset(userData, Utils::FILL_ALLOC, requestedSize);

    auto* backCanary = reinterpret_cast<uint64_t*>(userData + requestedSize);
    *backCanary = Utils::CANARY_BACK;

    return userData;
}

// Validate and unwrap  (returns raw pointer suitable for passing back to allocator)
// Asserts on corruption; always poisons the user region.
inline void* Unwrap(void* userPtr) {
    if (!userPtr) return nullptr;
    auto* raw = reinterpret_cast<uint8_t*>(userPtr) - sizeof(GuardHeader);
    auto* hdr = reinterpret_cast<GuardHeader*>(raw);

    assert(hdr->IsValid()   && "MemoryGuard: header corruption / double-free detected!");

    auto* backCanary = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint8_t*>(userPtr) + hdr->requestedSize);
    assert(*backCanary == Utils::CANARY_BACK && "MemoryGuard: buffer overflow detected!");

    // Poison freed region
    std::memset(userPtr, Utils::FILL_FREE, hdr->requestedSize);
    *backCanary = 0;
    hdr->frontCanary = 0; // invalidate so double-free is caught

    return raw;
}

// Non-asserting check (returns false on corruption)
inline bool Check(void* userPtr) {
    if (!userPtr) return true;
    auto* raw = reinterpret_cast<uint8_t*>(userPtr) - sizeof(GuardHeader);
    auto* hdr = reinterpret_cast<GuardHeader*>(raw);
    if (!hdr->IsValid()) return false;
    auto* backCanary = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint8_t*>(userPtr) + hdr->requestedSize);
    return (*backCanary == Utils::CANARY_BACK);
}

} // namespace MemoryGuard
