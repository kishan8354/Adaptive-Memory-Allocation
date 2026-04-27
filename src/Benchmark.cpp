#include "Benchmark.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdlib>
#include <cassert>

// ═══════════════════════════════════════════════════════════════════════════════
// BenchmarkSuite
// ═══════════════════════════════════════════════════════════════════════════════

BenchmarkSuite::BenchmarkSuite(std::size_t nOperations, int runs)
    : m_nOps(std::min(nOperations, static_cast<std::size_t>(OPERATIONS_CAP)))
    , m_runs(runs) {}

// ── Statistics helpers ────────────────────────────────────────────────────────
double BenchmarkSuite::Mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

double BenchmarkSuite::StdDev(const std::vector<double>& v, double mean) {
    double sum = 0.0;
    for (auto x : v) sum += (x - mean) * (x - mean);
    return std::sqrt(sum / v.size());
}

double BenchmarkSuite::Percentile(std::vector<double>& sorted, double pct) {
    if (sorted.empty()) return 0.0;
    double idx = pct * (sorted.size() - 1);
    int lo = static_cast<int>(idx);
    int hi = std::min(lo + 1, static_cast<int>(sorted.size()) - 1);
    return sorted[lo] + (idx - lo) * (sorted[hi] - sorted[lo]);
}

BenchmarkResult BenchmarkSuite::ComputeStats(
        const std::string& allocName, const std::string& testName,
        std::vector<double>& times, std::size_t nOps,
        std::size_t peakBytes, double utilization,
        const AllocationMetrics& metrics) const {
    std::sort(times.begin(), times.end());
    double mean   = Mean(times);
    double stddev = StdDev(times, mean);
    double p95    = Percentile(times, 0.95);
    double p99    = Percentile(times, 0.99);

    BenchmarkResult r;
    r.allocatorName   = allocName;
    r.testName        = testName;
    r.nOperations     = nOps;
    r.meanUs          = mean;
    r.medianUs        = Percentile(times, 0.50);
    r.stddevUs        = stddev;
    r.p95Us           = p95;
    r.p99Us           = p99;
    r.minUs           = times.front();
    r.maxUs           = times.back();
    r.nsPerOp         = (mean * 1000.0) / static_cast<double>(nOps);
    r.mOpsPerSec      = nOps / (mean / 1000.0) / 1e6;   // µs → ms → MOps/s
    r.peakBytes       = peakBytes;
    r.utilization     = utilization;
    r.intFragRate     = metrics.InternalFragmentation();
    r.totalPaddingWaste = metrics.totalPaddingWaste;
    return r;
}

// ── Pick a random (size, align) pair from the provided lists ─────────────────
void BenchmarkSuite::RandomPick(const std::vector<std::size_t>& sizes,
                                 const std::vector<std::size_t>& aligns,
                                 std::size_t& size, std::size_t& align) const {
    int r = rand() % static_cast<int>(sizes.size());
    size  = sizes[r];
    align = aligns[r];
}

// ── Warm-up run ───────────────────────────────────────────────────────────────
static void WarmUp(Allocator* alloc, std::size_t size, std::size_t align, int nOps) {
    alloc->Init();
    std::vector<void*> ptrs;
    ptrs.reserve(nOps);
    for (int i = 0; i < nOps; ++i) {
        void* p = alloc->Allocate(size, align);
        if (p) ptrs.push_back(p);
    }
    for (void* p : ptrs) alloc->Free(p);
}

// ── SingleAlloc ───────────────────────────────────────────────────────────────
BenchmarkResult BenchmarkSuite::SingleAlloc(Allocator* alloc, std::size_t size, std::size_t align) {
    for (int w = 0; w < WARMUP_RUNS; ++w) WarmUp(alloc, size, align, 128);

    std::vector<double> times(m_runs);
    std::size_t peak = 0;

    for (int r = 0; r < m_runs; ++r) {
        alloc->Init();
        StartRound();
        for (std::size_t i = 0; i < m_nOps; ++i) alloc->Allocate(size, align);
        times[r] = FinishRound();
        peak = std::max(peak, alloc->GetPeak());
    }
    return ComputeStats(alloc->GetName(), "SingleAlloc", times,
                        m_nOps, peak, alloc->GetUtilization(), alloc->GetMetrics());
}

// ── SingleAllocFree ───────────────────────────────────────────────────────────
BenchmarkResult BenchmarkSuite::SingleAllocFree(Allocator* alloc, std::size_t size, std::size_t align) {
    std::vector<double> times(m_runs);
    std::size_t peak = 0;
    const std::size_t cap = std::min(m_nOps, static_cast<std::size_t>(OPERATIONS_CAP));
    std::vector<void*> ptrs(cap);

    for (int r = 0; r < m_runs; ++r) {
        alloc->Init();
        StartRound();
        for (std::size_t i = 0; i < cap; ++i) ptrs[i] = alloc->Allocate(size, align);
        for (std::size_t i = cap; i-- > 0;)  alloc->Free(ptrs[i]);
        times[r] = FinishRound();
        peak = std::max(peak, alloc->GetPeak());
    }
    return ComputeStats(alloc->GetName(), "AllocFree", times,
                        cap * 2, peak, alloc->GetUtilization(), alloc->GetMetrics());
}

// ── RandomMixed ──────────────────────────────────────────────────────────────
BenchmarkResult BenchmarkSuite::RandomMixed(Allocator* alloc,
                                              const std::vector<std::size_t>& sizes,
                                              const std::vector<std::size_t>& aligns) {
    srand(42);
    std::vector<double> times(m_runs);
    std::size_t peak = 0;
    const std::size_t cap = std::min(m_nOps, static_cast<std::size_t>(OPERATIONS_CAP));
    std::vector<void*> ptrs(cap);

    for (int r = 0; r < m_runs; ++r) {
        alloc->Init();
        StartRound();
        for (std::size_t i = 0; i < cap; ++i) {
            std::size_t sz, al;
            RandomPick(sizes, aligns, sz, al);
            ptrs[i] = alloc->Allocate(sz, al);
        }
        for (std::size_t i = cap; i-- > 0;) alloc->Free(ptrs[i]);
        times[r] = FinishRound();
        peak = std::max(peak, alloc->GetPeak());
    }
    return ComputeStats(alloc->GetName(), "RandomMixed", times,
                        cap * 2, peak, alloc->GetUtilization(), alloc->GetMetrics());
}

// ── FragmentationStress ───────────────────────────────────────────────────────
// Allocates alternating sizes then frees every other block to stress external fragmentation
BenchmarkResult BenchmarkSuite::FragmentationStress(Allocator* alloc,
                                                       const std::vector<std::size_t>& sizes,
                                                       const std::vector<std::size_t>& aligns) {
    srand(7);
    std::vector<double> times(m_runs);
    std::size_t peak = 0;
    const std::size_t cap = std::min(m_nOps, static_cast<std::size_t>(1 << 14));
    std::vector<void*> ptrs(cap, nullptr);

    for (int r = 0; r < m_runs; ++r) {
        alloc->Init();
        StartRound();
        // Phase 1: allocate cap blocks
        for (std::size_t i = 0; i < cap; ++i) {
            std::size_t sz, al;
            RandomPick(sizes, aligns, sz, al);
            ptrs[i] = alloc->Allocate(sz, al);
        }
        // Phase 2: free even-indexed blocks (creates holes)
        for (std::size_t i = 0; i < cap; i += 2)
            if (ptrs[i]) { alloc->Free(ptrs[i]); ptrs[i] = nullptr; }
        // Phase 3: allocate into the freed (even) holes only — avoids overwriting
        // still-live odd-indexed Phase 1 allocations, which would leak pool memory.
        for (std::size_t i = 0; i < cap; i += 2) {
            std::size_t sz, al;
            RandomPick(sizes, aligns, sz, al);
            void* p = alloc->Allocate(sz, al);
            if (p) ptrs[i] = p;
        }
        times[r] = FinishRound();
        peak = std::max(peak, alloc->GetPeak());
        // Cleanup
        for (std::size_t i = 0; i < cap; ++i)
            if (ptrs[i]) { alloc->Free(ptrs[i]); ptrs[i] = nullptr; }
    }
    return ComputeStats(alloc->GetName(), "FragmentStress", times,
                        cap, peak, alloc->GetUtilization(), alloc->GetMetrics());
}

// ── RunComparison ─────────────────────────────────────────────────────────────
std::vector<BenchmarkResult> BenchmarkSuite::RunComparison(
        const std::vector<Allocator*>& allocators,
        const std::vector<std::size_t>& sizes,
        const std::vector<std::size_t>& aligns) {

    std::vector<BenchmarkResult> results;
    results.reserve(allocators.size() * 3);
    for (auto* a : allocators) {
        if (!a) continue;
        results.push_back(SingleAlloc    (a, sizes[0], aligns[0]));
        results.push_back(RandomMixed    (a, sizes, aligns));
        results.push_back(FragmentationStress(a, sizes, aligns));
    }
    return results;
}

// ── Output ────────────────────────────────────────────────────────────────────
void BenchmarkSuite::PrintResult(const BenchmarkResult& r, std::ostream& out) const {
    out << "┌─────────────────────────────────────────────────────────────┐\n";
    out << "│  " << std::left << std::setw(20) << r.allocatorName
        << "  │  " << std::setw(20) << r.testName << "         │\n";
    out << "├─────────────────────────────────────────────────────────────┤\n";
    out << "│  Operations  : " << std::right << std::setw(12) << r.nOperations  << "                          │\n";
    out << "│  Mean        : " << std::setw(10) << std::fixed << std::setprecision(2) << r.meanUs    << " µs                        │\n";
    out << "│  Median      : " << std::setw(10) << r.medianUs  << " µs                        │\n";
    out << "│  Std-dev     : " << std::setw(10) << r.stddevUs  << " µs                        │\n";
    out << "│  P95 / P99   : " << std::setw(8)  << r.p95Us << " / " << std::setw(8) << r.p99Us << " µs              │\n";
    out << "│  ns/op       : " << std::setw(10) << std::setprecision(1) << r.nsPerOp     << "                          │\n";
    out << "│  MOps/sec    : " << std::setw(10) << std::setprecision(3) << r.mOpsPerSec  << "                          │\n";
    out << "│  Peak memory : " << std::setw(12) << r.peakBytes << " B                        │\n";
    out << "│  Int.Frag    : " << std::setw(9)  << std::setprecision(2) << r.intFragRate*100 << " %%                       │\n";
    out << "│  Padding waste: " << std::setw(10) << r.totalPaddingWaste << " B                        │\n";
    out << "└─────────────────────────────────────────────────────────────┘\n\n";
}

void BenchmarkSuite::PrintLeaderboard(const std::vector<BenchmarkResult>& results,
                                       std::ostream& out) const {
    // Group by testName, sort by meanUs within each group
    std::vector<BenchmarkResult> sorted = results;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b){ return a.meanUs < b.meanUs; });

    out << "\n╔══════════════════════════════════════════════════════════════════════╗\n";
    out << "║                    BENCHMARK LEADERBOARD                            ║\n";
    out << "╠══════════════════════════════╦═════════════╦═══════════╦════════════╣\n";
    out << "║  Allocator                   ║  Test       ║  ns/op    ║  MOps/s    ║\n";
    out << "╠══════════════════════════════╬═════════════╬═══════════╬════════════╣\n";
    for (auto& r : sorted) {
        out << "║  " << std::left << std::setw(28) << r.allocatorName
            << "║  " << std::setw(11) << r.testName
            << "║  " << std::right << std::setw(8) << std::fixed << std::setprecision(1) << r.nsPerOp
            << "   ║  " << std::setw(8) << std::setprecision(2) << r.mOpsPerSec << "    ║\n";
    }
    out << "╚══════════════════════════════╩═════════════╩═══════════╩════════════╝\n\n";
}

void BenchmarkSuite::ExportCSV(const std::vector<BenchmarkResult>& results,
                                const std::string& filename) const {
    std::ofstream f(filename);
    if (!f) { std::cerr << "Cannot open " << filename << "\n"; return; }
    f << "allocator,test,ops,mean_us,median_us,stddev_us,p95_us,p99_us,"
         "ns_per_op,mops_per_sec,peak_bytes,int_frag_pct,padding_waste\n";
    for (auto& r : results) {
        f << r.allocatorName   << ","
          << r.testName        << ","
          << r.nOperations     << ","
          << r.meanUs          << ","
          << r.medianUs        << ","
          << r.stddevUs        << ","
          << r.p95Us           << ","
          << r.p99Us           << ","
          << r.nsPerOp         << ","
          << r.mOpsPerSec      << ","
          << r.peakBytes       << ","
          << r.intFragRate*100 << ","
          << r.totalPaddingWaste << "\n";
    }
    std::cout << "CSV exported to: " << filename << "\n";
}

void BenchmarkSuite::ExportMarkdown(const std::vector<BenchmarkResult>& results,
                                     const std::string& filename) const {
    std::ofstream f(filename);
    if (!f) { std::cerr << "Cannot open " << filename << "\n"; return; }
    f << "# Allocator Benchmark Results\n\n"
      << "| Allocator | Test | Ops | Mean (µs) | Median (µs) | P99 (µs) | ns/op | MOps/s | Peak (B) | IntFrag% |\n"
      << "|-----------|------|-----|-----------|-------------|----------|-------|--------|----------|----------|\n";
    for (auto& r : results) {
        f << "| " << r.allocatorName
          << " | "  << r.testName
          << " | "  << r.nOperations
          << " | "  << std::fixed << std::setprecision(2) << r.meanUs
          << " | "  << r.medianUs
          << " | "  << r.p99Us
          << " | "  << std::setprecision(1) << r.nsPerOp
          << " | "  << std::setprecision(2) << r.mOpsPerSec
          << " | "  << r.peakBytes
          << " | "  << std::setprecision(1) << r.intFragRate*100 << " |\n";
    }
    std::cout << "Markdown exported to: " << filename << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Legacy Benchmark
// ═══════════════════════════════════════════════════════════════════════════════
void Benchmark::StartRound() { m_t0 = std::chrono::high_resolution_clock::now(); }
void Benchmark::FinishRound() {
    auto dt = std::chrono::high_resolution_clock::now() - m_t0;
    m_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(dt);
}
Benchmark::Results Benchmark::BuildResults(std::size_t nOps, std::size_t peak) const {
    double ms = static_cast<double>(m_elapsed.count());
    return {nOps, m_elapsed,
            ms > 0 ? nOps / ms : 0.0,
            ms > 0 ? ms / nOps : 0.0,
            peak};
}
void Benchmark::PrintResults(const Results& r) const {
    std::cout << "\tRESULTS:\n"
              << "\t\tOperations:   " << r.ops    << "\n"
              << "\t\tTime elapsed: " << r.ms.count() << " ms\n"
              << "\t\tOps/ms:       " << r.opsPerMs << "\n"
              << "\t\tMs/op:        " << r.msPerOp  << "\n"
              << "\t\tPeak:         " << r.peak << " B\n\n";
}
void Benchmark::RandAttr(const std::vector<std::size_t>& sizes,
                          const std::vector<std::size_t>& aligns,
                          std::size_t& size, std::size_t& align) {
    int r = rand() % static_cast<int>(sizes.size());
    size = sizes[r]; align = aligns[r];
}

void Benchmark::SingleAllocation(Allocator* alloc, std::size_t size, std::size_t align) {
    std::cout << "BENCHMARK: ALLOCATION  size=" << size << " align=" << align << "\n";
    alloc->Init(); StartRound();
    for (std::size_t i = 0; i < m_nOps; ++i) alloc->Allocate(size, align);
    FinishRound(); PrintResults(BuildResults(m_nOps, alloc->GetPeak()));
}
void Benchmark::SingleFree(Allocator* alloc, std::size_t size, std::size_t align) {
    std::cout << "BENCHMARK: ALLOC/FREE  size=" << size << " align=" << align << "\n";
    std::vector<void*> ptrs(m_nOps);
    alloc->Init(); StartRound();
    for (std::size_t i = 0; i < m_nOps; ++i) ptrs[i] = alloc->Allocate(size, align);
    for (std::size_t i = m_nOps; i-- > 0;)   alloc->Free(ptrs[i]);
    FinishRound(); PrintResults(BuildResults(m_nOps*2, alloc->GetPeak()));
}
void Benchmark::MultipleAllocation(Allocator* alloc, const std::vector<std::size_t>& sz,
                                    const std::vector<std::size_t>& al) {
    for (std::size_t i = 0; i < sz.size(); ++i) SingleAllocation(alloc, sz[i], al[i]);
}
void Benchmark::MultipleFree(Allocator* alloc, const std::vector<std::size_t>& sz,
                              const std::vector<std::size_t>& al) {
    for (std::size_t i = 0; i < sz.size(); ++i) SingleFree(alloc, sz[i], al[i]);
}
void Benchmark::RandomAllocation(Allocator* alloc, const std::vector<std::size_t>& sz,
                                  const std::vector<std::size_t>& al) {
    srand(1);
    std::cout << "BENCHMARK: RANDOM ALLOCATION\n";
    alloc->Init(); StartRound();
    for (std::size_t i = 0; i < m_nOps; ++i) {
        std::size_t s, a; RandAttr(sz, al, s, a);
        alloc->Allocate(s, a);
    }
    FinishRound(); PrintResults(BuildResults(m_nOps, alloc->GetPeak()));
}
void Benchmark::RandomFree(Allocator* alloc, const std::vector<std::size_t>& sz,
                            const std::vector<std::size_t>& al) {
    srand(1);
    std::cout << "BENCHMARK: RANDOM ALLOC/FREE\n";
    std::vector<void*> ptrs(m_nOps);
    alloc->Init(); StartRound();
    for (std::size_t i = 0; i < m_nOps; ++i) {
        std::size_t s, a; RandAttr(sz, al, s, a);
        ptrs[i] = alloc->Allocate(s, a);
    }
    for (std::size_t i = m_nOps; i-- > 0;) alloc->Free(ptrs[i]);
    FinishRound(); PrintResults(BuildResults(m_nOps*2, alloc->GetPeak()));
}
