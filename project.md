# How to Test on Your Linux System

---

## Step 1 — Download the project

Download `advanced-allocators.zip` from this conversation, then open a terminal:

```bash
# Move to your home folder (or wherever you want)
cd ~

# Unzip
unzip advanced-allocators.zip

# Enter the project
cd advanced-allocators
```

---

## Step 2 — Install build tools (if not already installed)

Open a terminal and run:

```bash
# Ubuntu / Debian / Linux Mint
sudo apt update
sudo apt install -y g++ cmake make

# Fedora / RHEL / CentOS
sudo dnf install -y gcc-c++ cmake make

# Arch Linux
sudo pacman -S gcc cmake make
```

Check they installed correctly:

```bash
g++ --version     # should show g++ 9.x or higher
cmake --version   # should show cmake 3.14 or higher
```

---

## Step 3 — Build the project

You have two options. Use whichever feels easier.

### Option A — CMake (recommended)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run it:

```bash
./build/allocator_demo
```

### Option B — Single g++ command (no CMake needed)

```bash
g++ -std=c++17 -O2 -Iinclude \
    src/CAllocator.cpp \
    src/LinearAllocator.cpp \
    src/StackAllocator.cpp \
    src/PoolAllocator.cpp \
    src/FreeListAllocator.cpp \
    src/BuddyAllocator.cpp \
    src/SlabAllocator.cpp \
    src/TLSFAllocator.cpp \
    src/AdaptiveAllocator.cpp \
    src/Benchmark.cpp \
    main.cpp \
    -o allocator_demo
```

Run it:

```bash
./allocator_demo
```

---

## Step 4 — What you should see

The program runs 7 demos then a full benchmark. Output looks like this:

```
Advanced Memory Allocators  –  Thesis-Level Implementation

================================================================
  MemoryGuard – Canary-based Corruption Detection
================================================================
  Guard check PASS: YES
  After user write, guard check PASS: YES
  Freed successfully. Canaries verified.

================================================================
  AllocationTracker – Leak Detection
================================================================
  Live allocations: 3  total bytes: 448
[AllocationTracker] *** 1 LEAK(S) DETECTED ***   ← intentional demo
  After freeing c: No leaks detected.

================================================================
  SlabAllocator – Object Caches with Cache Coloring
================================================================
  Allocated 1000 TreeNode + 1000 Event objects
Cache       ObjSize   Allocs  HitRate%  Slabs
TreeNode    64        1000    99.9      1
Event       128       1000    99.8      2

================================================================
  BuddyAllocator – Power-of-2 Coalescing
================================================================
  After 3 allocs: used=3584 B
  After freeing p2: used=0 B  extFrag=0.000
  order=20 size=1048576 blocks=1       ← fully coalesced back to 1 block

================================================================
  AdaptiveAllocator – Online Workload-Aware Strategy Selection
================================================================
  Phase 1: Uniform 64-byte allocations
  Current strategy  : Pool             ← correctly detected uniform sizes
  uniformityScore   : 1.000

  Phase 2: Mixed sizes 32-8192 B
  Current strategy  : FreeList         ← switched after detecting variance
  Strategy switches (2):
    @alloc#512   FreeList → Pool
    @alloc#1024  Pool → FreeList

================================================================
  Statistical Benchmark Suite – All Allocators
================================================================

  [results for every allocator...]

╔══════════ LEADERBOARD ═══════════╗
  Linear       SingleAlloc    7.5 ns/op
  Stack        SingleAlloc    8.0 ns/op
  Slab         SingleAlloc   24.6 ns/op
  FreeList     SingleAlloc   40.2 ns/op
  ...

CSV exported to: benchmark_results.csv
Markdown exported to: benchmark_results.md
```

---

## Step 5 — Run individual allocator tests

Create a file called `my_test.cpp` inside the `advanced-allocators/` folder:

```cpp
#include <iostream>
#include <vector>
#include <cassert>
#include "BuddyAllocator.h"
#include "TLSFAllocator.h"
#include "SlabAllocator.h"
#include "MemoryGuard.h"
#include "FreeListAllocator.h"

void test_buddy() {
    BuddyAllocator b(1 << 20);   // 1 MiB — must be power of 2
    b.Init();

    void* p1 = b.Allocate(100, 8);
    void* p2 = b.Allocate(500, 8);
    void* p3 = b.Allocate(64,  8);

    assert(p1 && p2 && p3 && "allocation failed");

    b.Free(p1);
    b.Free(p3);
    b.Free(p2);

    assert(b.GetUsed() == 0 && "memory leak in buddy");
    std::cout << "BuddyAllocator      PASS\n";
}

void test_tlsf() {
    TLSFAllocator t(4 * 1024 * 1024);
    t.Init();

    std::vector<void*> ptrs;
    for (std::size_t s : {16, 32, 64, 128, 256, 512, 1024, 2048})
        ptrs.push_back(t.Allocate(s, 8));

    for (auto* p : ptrs)
        assert(p != nullptr && "tlsf returned null");

    // Free in random order — TLSF handles any order
    for (int i : {3, 7, 1, 5, 0, 6, 2, 4})
        t.Free(ptrs[i]);

    assert(t.GetUsed() == 0 && "memory leak in tlsf");
    std::cout << "TLSFAllocator       PASS\n";
}

void test_slab() {
    SlabAllocator s(4 * 1024 * 1024);
    s.Init();

    auto* cache = s.CreateCache("MyObject", 64, 8);

    std::vector<void*> objs;
    for (int i = 0; i < 500; i++)
        objs.push_back(s.CacheAlloc(cache));

    for (auto* p : objs)
        assert(p != nullptr && "slab returned null");

    for (auto* p : objs)
        s.CacheFree(cache, p);

    std::cout << "SlabAllocator       PASS\n";
}

void test_memory_guard() {
    FreeListAllocator backing(65536);
    backing.Init();

    const std::size_t sz = 32;
    void* raw  = backing.Allocate(sz + MemoryGuard::Overhead(), 8);
    void* user = MemoryGuard::Wrap(raw, sz);

    // Normal write — should pass
    memset(user, 0xAB, sz);
    assert(MemoryGuard::Check(user) == true && "guard should pass on valid write");

    // Simulate overflow — write 1 byte past end
    uint8_t* overflow = static_cast<uint8_t*>(user) + sz;
    *overflow = 0xFF;
    assert(MemoryGuard::Check(user) == false && "guard should catch the overflow");

    std::cout << "MemoryGuard         PASS (overflow detected correctly)\n";
    backing.Free(raw);
}

int main() {
    std::cout << "\n=== Running allocator tests ===\n\n";
    test_buddy();
    test_tlsf();
    test_slab();
    test_memory_guard();
    std::cout << "\nAll tests passed!\n\n";
    return 0;
}
```

Compile and run:

```bash
g++ -std=c++17 -O2 -Iinclude \
    src/CAllocator.cpp src/LinearAllocator.cpp src/StackAllocator.cpp \
    src/PoolAllocator.cpp src/FreeListAllocator.cpp src/BuddyAllocator.cpp \
    src/SlabAllocator.cpp src/TLSFAllocator.cpp src/AdaptiveAllocator.cpp \
    src/Benchmark.cpp my_test.cpp -o my_test

./my_test
```

Expected output:

```
=== Running allocator tests ===

BuddyAllocator      PASS
TLSFAllocator       PASS
SlabAllocator       PASS
MemoryGuard         PASS (overflow detected correctly)

All tests passed!
```

---

## Step 6 — Run with memory safety checker (AddressSanitizer)

This catches hidden bugs like use-after-free, buffer overflows, and memory leaks:

```bash
g++ -std=c++17 -g -fsanitize=address,undefined -Iinclude \
    src/CAllocator.cpp src/LinearAllocator.cpp src/StackAllocator.cpp \
    src/PoolAllocator.cpp src/FreeListAllocator.cpp src/BuddyAllocator.cpp \
    src/SlabAllocator.cpp src/TLSFAllocator.cpp src/AdaptiveAllocator.cpp \
    src/Benchmark.cpp my_test.cpp -o my_test_safe

./my_test_safe
```

If there are no real bugs, it just runs normally.  
If it prints `ERROR: AddressSanitizer: ...` there is a memory bug to investigate.

---

## Step 7 — View benchmark results

After running `./allocator_demo`, two result files are created in the same folder:

```bash
# View the CSV (import into Excel or Python)
cat benchmark_results.csv

# View formatted markdown table
cat benchmark_results.md
```

To import into Python for analysis:

```python
import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('benchmark_results.csv')
print(df.head())

# Plot ns/op per allocator for SingleAlloc
single = df[df['test'] == 'SingleAlloc']
plt.bar(single['allocator'], single['ns_per_op'])
plt.ylabel('ns / op')
plt.title('Allocator Throughput — SingleAlloc')
plt.xticks(rotation=45)
plt.tight_layout()
plt.savefig('benchmark.png')
```

---

## Quick Reference — All Commands

```bash
# 1. Unzip and enter
unzip advanced-allocators.zip && cd advanced-allocators

# 2. Build (release)
g++ -std=c++17 -O2 -Iinclude src/*.cpp main.cpp -o allocator_demo

# 3. Run all demos + benchmark
./allocator_demo

# 4. Build and run your own test file
g++ -std=c++17 -O2 -Iinclude src/*.cpp my_test.cpp -o my_test && ./my_test

# 5. Build with memory safety checks
g++ -std=c++17 -g -fsanitize=address,undefined -Iinclude src/*.cpp my_test.cpp -o my_test_safe && ./my_test_safe

# 6. See benchmark CSV
cat benchmark_results.csv

# 7. Clean up build files
rm -f allocator_demo my_test my_test_safe
```

---

## Common Problems

| Problem | Fix |
|---|---|
| `g++: command not found` | `sudo apt install g++` |
| `cmake: command not found` | `sudo apt install cmake` |
| `fatal error: BuddyAllocator.h: No such file` | Make sure you are inside the `advanced-allocators/` folder and using `-Iinclude` in your command |
| `BuddyAllocator` assertion: pool must be power of 2 | Use `1 << 20` (1 MiB), `1 << 22` (4 MiB) etc. Never `1000000` |
| `PoolAllocator` returns nullptr | Your requested size is larger than the chunk size set at construction |
| Benchmark runs very slow | Normal — `FragmentStress` on FreeList is intentionally slow to show O(n) degradation |
