// Lightweight benchmark helpers, no external framework.
//
// Provides:
//   - Timer: steady_clock wrapper
//   - do_not_optimize: prevents the compiler from eliding a value
//   - run_bench: single-threaded, reps samples, median ns/op
//   - run_bench_mt: multi-threaded, total wall-clock / total ops
//   - print_header / print_result: aligned console output

#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace bench {

class Timer {
  public:
    void start() noexcept { now_ = std::chrono::steady_clock::now(); }
    std::uint64_t stop_ns() const noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - now_)
                .count());
    }

  private:
    std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::now()};
};

/// Opaque sink for the compiler, so the benchmarked expression is not
/// optimized away.
template <typename T> inline void do_not_optimize(T &&value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    volatile auto *p = &value;
    (void)p;
#endif
}

struct Result {
    std::string name;
    double ns_per_op;
    std::size_t iterations;
};

inline void print_header() {
    std::printf("%-42s %12s %12s %14s\n", "Benchmark", "Time(ns)", "Ops", "Items/s");
    std::printf("%s\n", std::string(84, '-').c_str());
}

inline void print_result(const Result &r) {
    const double ips = 1e9 / r.ns_per_op;
    std::printf("%-42s %12.2f %12zu %13.2fM\n", r.name.c_str(), r.ns_per_op, r.iterations,
                ips / 1e6);
}

/// Run `op` `iterations` times, `reps` rounds, report the median ns/op.
/// Warm-up runs min(iterations, 10000) times first.
template <typename Op>
Result run_bench(const std::string &name, std::size_t iterations, Op &&op, int reps = 5) {
    const auto warm = std::min<std::size_t>(iterations, 10000);
    for (std::size_t i = 0; i < warm; ++i) {
        op();
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(reps));
    for (int r = 0; r < reps; ++r) {
        Timer t;
        t.start();
        for (std::size_t i = 0; i < iterations; ++i) {
            op();
        }
        const auto ns = t.stop_ns();
        samples.push_back(static_cast<double>(ns) / static_cast<double>(iterations));
    }
    std::sort(samples.begin(), samples.end());
    return {name, samples[samples.size() / 2], iterations};
}

/// Multi-threaded variant: `num_threads` threads each call `op` `iterations`
/// times. Reports total wall-clock / (iterations * num_threads).
template <typename Op>
Result run_bench_mt(const std::string &name, std::size_t iterations, std::size_t num_threads,
                    Op &&op, int reps = 5) {
    // Warm-up with a short run.
    {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([&op] {
                for (std::size_t j = 0; j < 2000; ++j) {
                    op();
                }
            });
        }
        for (auto &t : threads) {
            t.join();
        }
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(reps));
    const double total_ops =
        static_cast<double>(iterations) * static_cast<double>(num_threads);
    for (int r = 0; r < reps; ++r) {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        Timer t;
        t.start();
        for (std::size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([&op, iterations] {
                for (std::size_t j = 0; j < iterations; ++j) {
                    op();
                }
            });
        }
        for (auto &th : threads) {
            th.join();
        }
        const auto ns = t.stop_ns();
        samples.push_back(static_cast<double>(ns) / total_ops);
    }
    std::sort(samples.begin(), samples.end());
    return {name, samples[samples.size() / 2],
            iterations * num_threads};
}

} // namespace bench
