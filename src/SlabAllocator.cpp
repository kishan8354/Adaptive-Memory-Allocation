#include "SlabAllocator.h"
#include "Utils.h"
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

// Size classes (rounded up to nearest): 16,32,64,128,256,512,1024,2048,4096,8192
static const std::size_t SIZE_CLASSES[] = {16,32,64,128,256,512,1024,2048,4096,8192};
static const int NUM_CLASSES = 10;

SlabAllocator::SlabAllocator(std::size_t totalSize)
    : Allocator(totalSize, "Slab") {}

SlabAllocator::~SlabAllocator() {
    for (auto* c : m_allCaches) delete c;
    std::free(m_pool);
}

void SlabAllocator::Init() {
    if (m_pool) { std::free(m_pool); }
    m_pool       = static_cast<uint8_t*>(std::malloc(m_totalSize));
    assert(m_pool && "SlabAllocator::Init – malloc failed");
    Reset();
}

void SlabAllocator::Reset() {
    m_poolOffset = 0;
    m_used = m_peak = 0;
    m_metrics = {};
    for (auto* c : m_allCaches) delete c;
    m_allCaches.clear();
    m_sizeMap.clear();
    m_regions.clear();
}

// ── Slab list manipulation ────────────────────────────────────────────────────
void SlabAllocator::InsertSlab(SlabHeader*& list, SlabHeader* slab) {
    slab->prev = nullptr; slab->next = list;
    if (list) list->prev = slab;
    list = slab;
}

void SlabAllocator::RemoveSlab(SlabHeader*& list, SlabHeader* slab) {
    if (slab->prev) slab->prev->next = slab->next;
    else            list             = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    slab->prev = slab->next = nullptr;
}

// ── Slab init ─────────────────────────────────────────────────────────────────
void SlabAllocator::InitSlabFreeList(SlabHeader* slab, std::size_t objSize,
                                      std::size_t colorOffset) {
    uint8_t* base = reinterpret_cast<uint8_t*>(slab) + sizeof(SlabHeader) + colorOffset;
    const std::size_t available = SLAB_SIZE - sizeof(SlabHeader) - colorOffset;
    uint32_t cap = static_cast<uint32_t>(available / objSize);
    assert(cap >= 1 && "SlabAllocator: object too large for slab");

    slab->freeCount = cap;
    slab->capacity  = cap;

    // Build an embedded singly-linked free list
    for (uint32_t i = 0; i < cap; ++i) {
        void** cell  = reinterpret_cast<void**>(base + i * objSize);
        *cell = (i + 1 < cap) ? static_cast<void*>(base + (i+1) * objSize) : nullptr;
    }
    slab->freeHead = static_cast<void*>(base);
}

SlabAllocator::SlabHeader* SlabAllocator::AllocateSlab(ObjectCache* cache) {
    if (m_poolOffset + SLAB_SIZE > m_totalSize) return nullptr;

    uint8_t* mem = m_pool + m_poolOffset;
    m_poolOffset += SLAB_SIZE;

    // Cache coloring: advance offset by CACHE_LINE_SIZE each slab
    std::size_t color = cache->colorCursor;
    cache->colorCursor = (cache->colorCursor + CACHE_LINE_SIZE) %
                         std::max(CACHE_LINE_SIZE, cache->objSize);

    auto* slab    = reinterpret_cast<SlabHeader*>(mem);
    slab->prev    = slab->next = nullptr;
    slab->objSize = cache->objSize;
    InitSlabFreeList(slab, cache->objSize, color);
    ++cache->slabsAllocated;

    // Register region for reverse lookup
    m_regions.push_back({mem, mem + SLAB_SIZE, slab, cache});

    m_used += SLAB_SIZE;
    UpdatePeak();
    return slab;
}

// ── Cache API ─────────────────────────────────────────────────────────────────
SlabAllocator::ObjectCache* SlabAllocator::CreateCache(const std::string& name,
                                                         std::size_t objSize,
                                                         std::size_t /*alignment*/) {
    objSize = std::max(objSize, sizeof(void*));  // need at least pointer-sized free node
    auto* cache = new ObjectCache{};
    cache->name       = name;
    cache->objSize    = objSize;
    cache->alignment  = 8;
    m_allCaches.push_back(cache);
    return cache;
}

void* SlabAllocator::CacheAlloc(ObjectCache* cache) {
    // Prefer partial slabs, then empty, then allocate a new slab
    SlabHeader* slab = cache->partial ? cache->partial
                     : cache->empty   ? cache->empty
                     : nullptr;
    bool fromPartial = (slab != nullptr) && (slab == cache->partial);

    if (!slab) {
        slab = AllocateSlab(cache);
        if (!slab) return nullptr;
        InsertSlab(cache->empty, slab);
    }

    // Move slab from the appropriate list before checking state
    if (fromPartial)        RemoveSlab(cache->partial, slab);
    else if (slab->IsEmpty()) RemoveSlab(cache->empty, slab);

    // Pop object from slab free list
    void* obj       = slab->freeHead;
    slab->freeHead  = *reinterpret_cast<void**>(obj);
    --slab->freeCount;

    // Re-insert into correct list
    if      (slab->IsFull())  InsertSlab(cache->full,    slab);
    else if (slab->IsEmpty()) InsertSlab(cache->empty,   slab);
    else                      InsertSlab(cache->partial, slab);

    ++cache->totalAllocs;
    if (fromPartial) ++cache->cacheHits;

    m_used += cache->objSize;
    UpdatePeak();
    ++m_metrics.totalAllocations;
    m_metrics.totalBytesRequested += cache->objSize;
    m_metrics.totalBytesActual    += cache->objSize;
    return obj;
}

void SlabAllocator::CacheFree(ObjectCache* cache, void* ptr) {
    auto [slab, _cache] = FindSlab(ptr);
    (void)_cache;
    assert(slab && "SlabAllocator: free of ptr not belonging to this cache");

    bool wasFull = slab->IsFull();
    // Return to slab free list
    *reinterpret_cast<void**>(ptr) = slab->freeHead;
    slab->freeHead = ptr;
    ++slab->freeCount;

    // Re-classify slab
    if (wasFull) {
        RemoveSlab(cache->full, slab);
        if (slab->IsEmpty()) InsertSlab(cache->empty, slab);
        else                 InsertSlab(cache->partial, slab);
    } else if (slab->IsEmpty()) {
        RemoveSlab(cache->partial, slab);
        InsertSlab(cache->empty, slab);
    }

    m_used -= cache->objSize;
    ++m_metrics.totalDeallocations;
}

void SlabAllocator::DestroyCache(ObjectCache* cache) {
    // Just remove from our list; underlying pool memory is reclaimed at Reset()
    m_allCaches.erase(std::remove(m_allCaches.begin(), m_allCaches.end(), cache),
                      m_allCaches.end());
    delete cache;
}

// ── Size-class routing ────────────────────────────────────────────────────────
std::size_t SlabAllocator::NormSize(std::size_t s) const {
    for (int i = 0; i < NUM_CLASSES; ++i)
        if (s <= SIZE_CLASSES[i]) return SIZE_CLASSES[i];
    return s;  // oversized – will likely fail gracefully
}

SlabAllocator::ObjectCache* SlabAllocator::FindOrCreateCache(std::size_t size,
                                                               std::size_t alignment) {
    std::size_t key = NormSize(size);
    auto it = m_sizeMap.find(key);
    if (it != m_sizeMap.end()) return it->second;
    std::string name = "sc-" + std::to_string(key);
    auto* c = CreateCache(name, key, alignment);
    m_sizeMap[key] = c;
    return c;
}

// ── Generic Allocate/Free ─────────────────────────────────────────────────────
void* SlabAllocator::Allocate(std::size_t size, std::size_t alignment) {
    ObjectCache* cache = FindOrCreateCache(size, alignment);
    return CacheAlloc(cache);
}

void SlabAllocator::Free(void* ptr) {
    if (!ptr) return;
    auto [slab, cache] = FindSlab(ptr);
    assert(slab && cache && "SlabAllocator::Free – unknown ptr");
    CacheFree(cache, ptr);
}

std::pair<SlabAllocator::SlabHeader*, SlabAllocator::ObjectCache*>
SlabAllocator::FindSlab(void* ptr) const {
    auto* p = reinterpret_cast<uint8_t*>(ptr);
    for (const auto& r : m_regions) {
        if (p >= r.start && p < r.end) return {r.hdr, r.cache};
    }
    return {nullptr, nullptr};
}

// ── Diagnostics ──────────────────────────────────────────────────────────────
void SlabAllocator::PrintCacheStats() const {
    std::cout << "\n=== Slab Cache Statistics ===\n";
    std::cout << std::left << std::setw(12) << "Cache"
              << std::setw(10) << "ObjSize"
              << std::setw(12) << "Allocs"
              << std::setw(12) << "HitRate%"
              << std::setw(10) << "Slabs\n";
    std::cout << std::string(56, '-') << "\n";
    for (auto* c : m_allCaches) {
        std::cout << std::left << std::setw(12) << c->name
                  << std::setw(10) << c->objSize
                  << std::setw(12) << c->totalAllocs
                  << std::setw(12) << std::fixed << std::setprecision(1) << c->HitRate()*100.0
                  << std::setw(10) << c->slabsAllocated << "\n";
    }
}
