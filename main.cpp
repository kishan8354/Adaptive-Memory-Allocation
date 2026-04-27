#include <iostream>
#include <iomanip>
#include <vector>
#include <memory>

#include "Allocator.h"
#include "CAllocator.h"
#include "LinearAllocator.h"
#include "StackAllocator.h"
#include "PoolAllocator.h"
#include "FreeListAllocator.h"
#include "BuddyAllocator.h"
#include "SlabAllocator.h"
#include "TLSFAllocator.h"
#include "AdaptiveAllocator.h"
#include "MemoryGuard.h"
#include "AllocationTracker.h"
#include "Benchmark.h"

// ── Demo helpers ──────────────────────────────────────────────────────────────
static void PrintSeparator(const char* title) {
    int len = 64;
    std::cout << "\n" << std::string(len, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(len, '=') << "\n";
}

// ── Demonstrate MemoryGuard ───────────────────────────────────────────────────
static void DemoMemoryGuard() {
    PrintSeparator("MemoryGuard – Canary-based Corruption Detection");

    // Use FreeList as backing allocator
    FreeListAllocator backing(65536);
    backing.Init();

    std::cout << "  Allocating 64 bytes with guard overhead = "
              << MemoryGuard::Overhead() << " bytes\n";

    const std::size_t guardedSize = 64 + MemoryGuard::Overhead();
    void* raw = backing.Allocate(guardedSize, 8);
    void* userPtr = MemoryGuard::Wrap(raw, 64);

    std::cout << "  Raw ptr:  " << raw     << "\n";
    std::cout << "  User ptr: " << userPtr << "\n";
    std::cout << "  Guard check PASS: " << (MemoryGuard::Check(userPtr) ? "YES" : "NO") << "\n";

    // Simulate writing data
    std::memset(userPtr, 0xAB, 64);
    std::cout << "  After user write, guard check PASS: "
              << (MemoryGuard::Check(userPtr) ? "YES" : "NO") << "\n";

    // Unwrap (validates canaries, poisons freed region)
    void* freed = MemoryGuard::Unwrap(userPtr);
    backing.Free(freed);
    std::cout << "  Freed successfully. Canaries verified.\n";
}

// ── Demonstrate AllocationTracker ────────────────────────────────────────────
static void DemoAllocationTracker() {
    PrintSeparator("AllocationTracker – Leak Detection");

    auto& tracker = AllocationTracker::Instance();
    tracker.Reset();

    FreeListAllocator alloc(65536);
    alloc.Init();

    void* a = alloc.Allocate(128, 8); tracker.Track(a, 128, 128, "FreeList");
    void* b = alloc.Allocate(256, 8); tracker.Track(b, 256, 256, "FreeList");
    void* c = alloc.Allocate( 64, 8); tracker.Track(c,  64,  64, "FreeList");

    std::cout << "  Live allocations: " << tracker.LiveCount()
              << "  total bytes: " << tracker.LiveBytes() << "\n";

    // Free only a and b – c becomes a leak
    tracker.Untrack(a); alloc.Free(a);
    tracker.Untrack(b); alloc.Free(b);

    std::cout << "  After freeing a, b:\n";
    tracker.ReportLeaks();

    tracker.Untrack(c); alloc.Free(c);
    std::cout << "  After freeing c: ";
    tracker.ReportLeaks();
}

// ── Demonstrate StackAllocator checkpoint API ─────────────────────────────────
static void DemoStackCheckpoints() {
    PrintSeparator("StackAllocator – Checkpoint/Rollback (Region-based Free)");

    StackAllocator stack(1 << 20);
    stack.Init();

    std::size_t frame0 = stack.Push();
    std::cout << "  Frame0 start offset: " << frame0 << "\n";

    void* a = stack.Allocate(512, 8);
    void* b = stack.Allocate(1024, 8);
    std::cout << "  Allocated 512 + 1024 B,  used=" << stack.GetUsed() << "\n";

    std::size_t frame1 = stack.Push();
    void* c = stack.Allocate(4096, 8);
    std::cout << "  Frame1 + 4096 B,  used=" << stack.GetUsed() << "\n";

    // Rollback frame1 – frees c
    stack.Pop(frame1);
    std::cout << "  After Pop(frame1), used=" << stack.GetUsed()
              << "  (c freed)\n";

    // Rollback frame0 – frees a, b
    stack.Pop(frame0);
    std::cout << "  After Pop(frame0), used=" << stack.GetUsed()
              << "  (a, b freed)\n";
    (void)a; (void)b; (void)c;
}

// ── Demonstrate SlabAllocator with cache coloring ─────────────────────────────
static void DemoSlabAllocator() {
    PrintSeparator("SlabAllocator – Object Caches with Cache Coloring");

    SlabAllocator slab(4 * 1024 * 1024);  // 4 MiB pool
    slab.Init();

    auto* nodeCache  = slab.CreateCache("TreeNode",  64, 8);
    auto* eventCache = slab.CreateCache("Event",    128, 8);

    const int N = 1000;
    std::vector<void*> nodes(N), events(N);

    for (int i = 0; i < N; ++i) {
        nodes[i]  = slab.CacheAlloc(nodeCache);
        events[i] = slab.CacheAlloc(eventCache);
    }
    std::cout << "  Allocated " << N << " TreeNode + " << N << " Event objects\n";
    std::cout << "  Memory used: " << slab.GetUsed() << " B\n";

    for (int i = 0; i < N; ++i) {
        slab.CacheFree(nodeCache, nodes[i]);
        slab.CacheFree(eventCache, events[i]);
    }

    slab.PrintCacheStats();
}

// ── Demonstrate BuddyAllocator ────────────────────────────────────────────────
static void DemoBuddyAllocator() {
    PrintSeparator("BuddyAllocator – Power-of-2 Coalescing");

    BuddyAllocator buddy(1 << 20);  // 1 MiB
    buddy.Init();

    auto* p1 = buddy.Allocate(1000,  8);  // rounded to 1024
    auto* p2 = buddy.Allocate(2000,  8);  // rounded to 2048
    auto* p3 = buddy.Allocate(500,   8);  // rounded to 512
    std::cout << "  After 3 allocs: used=" << buddy.GetUsed() << " B\n";

    buddy.Free(p1);
    buddy.Free(p3);
    std::cout << "  After freeing p1, p3: used=" << buddy.GetUsed() << " B\n";
    buddy.Free(p2);
    std::cout << "  After freeing p2: used=" << buddy.GetUsed()
              << " B  extFrag=" << std::fixed << std::setprecision(3)
              << buddy.GetExternalFragmentation() << "\n";
    buddy.PrintFreeList();
}

// ── Demonstrate AdaptiveAllocator ─────────────────────────────────────────────
static void DemoAdaptiveAllocator() {
    PrintSeparator("AdaptiveAllocator – Online Workload-Aware Strategy Selection");

    AdaptiveAllocator adapt(1 << 24);  // 16 MiB
    adapt.Init();

    std::cout << "\n  Phase 1: Uniform 64-byte allocations (expect POOL recommendation)\n";
    std::vector<void*> ptrs;
    for (int i = 0; i < 600; ++i) {
        ptrs.push_back(adapt.Allocate(64, 8));
    }
    adapt.PrintAdaptiveReport(std::cout);
    for (auto* p : ptrs) adapt.Free(p);
    ptrs.clear();

    std::cout << "\n  Phase 2: Mixed sizes 32-8192 B (expect FREELIST or TLSF)\n";
    const std::size_t sizes[] = {32, 64, 128, 256, 512, 1024, 4096, 8192};
    for (int i = 0; i < 600; ++i) {
        ptrs.push_back(adapt.Allocate(sizes[rand() % 8], 8));
    }
    adapt.PrintAdaptiveReport(std::cout);
    for (auto* p : ptrs) adapt.Free(p);
}

// ── Full statistical benchmark ────────────────────────────────────────────────
static void RunFullBenchmark() {
    PrintSeparator("Statistical Benchmark Suite – All Allocators");

    const std::size_t MEM = static_cast<std::size_t>(1) << 26; // 64 MiB
    const std::vector<std::size_t> SIZES   = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    const std::vector<std::size_t> ALIGNS  = { 8,  8,   8,   8,   8,    8,    8,    8};

    CAllocator        cAlloc;
    LinearAllocator   linear(MEM);
    StackAllocator    stack(MEM);
    PoolAllocator     pool(MEM, 512);
    FreeListAllocator freeList(MEM);
    TLSFAllocator     tlsf(MEM);
    SlabAllocator     slab(MEM);

    // Buddy needs power-of-2 size
    std::size_t buddySz = 1;
    while (buddySz < MEM) buddySz <<= 1;
    BuddyAllocator buddy(buddySz >> 1);

    AdaptiveAllocator adapt(MEM);

    std::vector<Allocator*> all = {
        &cAlloc, &linear, &stack,
        &freeList, &tlsf, &slab, &buddy, &adapt
    };

    BenchmarkSuite suite(1 << 15, 5);  // 32K ops, 5 runs

    auto results = suite.RunComparison(all, SIZES, ALIGNS);

    for (auto& r : results) suite.PrintResult(r);
    suite.PrintLeaderboard(results);

    suite.ExportCSV     (results, "/mnt/user-data/outputs/benchmark_results.csv");
    suite.ExportMarkdown(results, "/mnt/user-data/outputs/benchmark_results.md");
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "Advanced Memory Allocators  –  Thesis-Level Implementation\n";
    std::cout << "Novel contributions:\n"
              << "  1. TLSF (Two-Level Segregated Fit)  –  O(1) real-time\n"
              << "  2. Buddy System                     –  XOR coalescing\n"
              << "  3. Slab Allocator                   –  cache-colored object pools\n"
              << "  4. Adaptive Allocator               –  online workload-driven strategy\n"
              << "  5. MemoryGuard                      –  canary corruption detection\n"
              << "  6. AllocationTracker                –  live-allocation leak detection\n"
              << "  7. Statistical BenchmarkSuite       –  mean, stddev, P95, P99\n"
              << "  8. StackAllocator Checkpoints       –  region-based bulk free API\n";

    DemoMemoryGuard();
    DemoAllocationTracker();
    DemoStackCheckpoints();
    DemoSlabAllocator();
    DemoBuddyAllocator();
    DemoAdaptiveAllocator();
    RunFullBenchmark();

    std::cout << "\nDone.\n";
    return 0;
}
