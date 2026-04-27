#pragma once
#include "Allocator.h"
#include <vector>
#include <string>
#include <chrono>
#include <ostream>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// BenchmarkResult  –  full statistical summary of one benchmark run
// ─────────────────────────────────────────────────────────────────────────────
struct BenchmarkResult {
    std::string allocatorName;
    std::string testName;
    std::size_t nOperations = 0;

    // Timing (µs = microseconds)
    double meanUs   = 0.0;
    double medianUs = 0.0;
    double stddevUs = 0.0;
    double p95Us    = 0.0;
    double p99Us    = 0.0;
    double minUs    = 0.0;
    double maxUs    = 0.0;

    // Throughput
    double mOpsPerSec = 0.0;   // millions of ops / sec
    double nsPerOp    = 0.0;

    // Memory
    std::size_t peakBytes        = 0;
    double      utilization      = 0.0;
    double      intFragRate      = 0.0;
    std::size_t totalPaddingWaste= 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BenchmarkSuite  –  advanced benchmark harness with
//   • configurable repeat-runs for statistical robustness
//   • warm-up phase to avoid cold-start bias
//   • CSV and Markdown report export
//   • comparative leaderboard across multiple allocators
// ─────────────────────────────────────────────────────────────────────────────
class BenchmarkSuite {
public:
    static constexpr int DEFAULT_RUNS   = 7;
    static constexpr int WARMUP_RUNS    = 2;
    static constexpr int OPERATIONS_CAP = 1 << 20;  // 1 M

    explicit BenchmarkSuite(std::size_t nOperations, int runs = DEFAULT_RUNS);

    // ── Individual benchmarks ────────────────────────────────────────────
    BenchmarkResult SingleAlloc   (Allocator*, std::size_t size, std::size_t align);
    BenchmarkResult SingleAllocFree(Allocator*, std::size_t size, std::size_t align);
    BenchmarkResult RandomMixed   (Allocator*,
                                    const std::vector<std::size_t>& sizes,
                                    const std::vector<std::size_t>& aligns);
    BenchmarkResult FragmentationStress(Allocator*,
                                         const std::vector<std::size_t>& sizes,
                                         const std::vector<std::size_t>& aligns);

    // ── Comparative suite ────────────────────────────────────────────────
    std::vector<BenchmarkResult> RunComparison(
        const std::vector<Allocator*>& allocators,
        const std::vector<std::size_t>& sizes,
        const std::vector<std::size_t>& aligns);

    // ── Output ───────────────────────────────────────────────────────────
    void PrintResult     (const BenchmarkResult& r, std::ostream& out = std::cout) const;
    void PrintLeaderboard(const std::vector<BenchmarkResult>& results,
                          std::ostream& out = std::cout) const;
    void ExportCSV       (const std::vector<BenchmarkResult>& results,
                          const std::string& filename) const;
    void ExportMarkdown  (const std::vector<BenchmarkResult>& results,
                          const std::string& filename) const;

private:
    std::size_t m_nOps;
    int         m_runs;

    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_t0;

    void        StartRound() { m_t0 = Clock::now(); }
    double      FinishRound() {
        auto dt = Clock::now() - m_t0;
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()) / 1000.0; // µs
    }

    static double Percentile(std::vector<double>& sorted, double pct);
    static double Mean      (const std::vector<double>& v);
    static double StdDev    (const std::vector<double>& v, double mean);

    void   RandomPick(const std::vector<std::size_t>& sizes,
                      const std::vector<std::size_t>& aligns,
                      std::size_t& size, std::size_t& align) const;

    BenchmarkResult ComputeStats(const std::string& allocName,
                                  const std::string& testName,
                                  std::vector<double>& times,
                                  std::size_t nOps,
                                  std::size_t peakBytes,
                                  double utilization,
                                  const AllocationMetrics& metrics) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Legacy Benchmark  –  backward-compatible interface (original API)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr std::size_t OPERATIONS = 1 << 16;

class Benchmark {
public:
    explicit Benchmark(std::size_t nOps) : m_nOps(nOps) {}

    void SingleAllocation   (Allocator*, std::size_t size, std::size_t align);
    void SingleFree         (Allocator*, std::size_t size, std::size_t align);
    void MultipleAllocation (Allocator*, const std::vector<std::size_t>&, const std::vector<std::size_t>&);
    void MultipleFree       (Allocator*, const std::vector<std::size_t>&, const std::vector<std::size_t>&);
    void RandomAllocation   (Allocator*, const std::vector<std::size_t>&, const std::vector<std::size_t>&);
    void RandomFree         (Allocator*, const std::vector<std::size_t>&, const std::vector<std::size_t>&);

private:
    std::size_t m_nOps;
    std::chrono::high_resolution_clock::time_point m_t0;
    std::chrono::milliseconds m_elapsed{0};

    struct Results {
        std::size_t ops;
        std::chrono::milliseconds ms;
        double opsPerMs;
        double msPerOp;
        std::size_t peak;
    };

    void    StartRound();
    void    FinishRound();
    Results BuildResults(std::size_t nOps, std::size_t peak) const;
    void    PrintResults(const Results& r) const;
    void    RandAttr(const std::vector<std::size_t>&, const std::vector<std::size_t>&,
                     std::size_t&, std::size_t&);
};
