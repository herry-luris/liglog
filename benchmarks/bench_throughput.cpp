// bench/bench_throughput.cpp
//
// 吞吐量对比：liglog vs spdlog
// 支持多生产者 × 多消费者扫描

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lig.hpp"
#include "logger.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kMessagesPerThread = 1'000'000;
constexpr int    kRepetitions       = 3;
constexpr size_t kQueueSize         = 64 * 1024;

// liglog 的 null sink
class LigNullSink final : public lig::interface::Sink {
  public:
    auto is_valid() const noexcept -> bool final { return true; }
    auto write(const void *, size_t len) -> WriteRes final { return WriteRes{len, {}}; }
    void close() noexcept final {}
    auto open() -> bool final { return true; }
};

struct StartBarrier {
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    int total;
    explicit StartBarrier(int t) : total(t) {}
    void arrive_and_wait() {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    void wait_all_ready() {
        while (ready.load(std::memory_order_acquire) < total) std::this_thread::yield();
    }
    void fire() { go.store(true, std::memory_order_release); }
};

template <typename ThreadFunc, typename FlushFunc>
double measure_throughput(int threads, ThreadFunc &&thread_fn, FlushFunc &&flush_fn) {
    StartBarrier barrier{threads};
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&, i] {
            barrier.arrive_and_wait();
            thread_fn(i);
        });
    }
    barrier.wait_all_ready();

    auto t0 = Clock::now();
    barrier.fire();
    for (auto &t : pool) t.join();
    flush_fn();
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(threads) * kMessagesPerThread / sec;
}

// ---------------- liglog ----------------

void setup_liglog() {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    auto &be = lig::logger::getBackend<lig::InnerBackend>();

    std::vector<std::unique_ptr<lig::interface::Sink>> sinks;
    sinks.push_back(std::make_unique<LigNullSink>());

    auto fmt_owner = std::make_unique<lig::component::TextFormatter>();
    fmt_owner->status.setTimeStamp(false).setThreadId(false).setFuncname(false).setLocation(false);
    std::unique_ptr<const lig::interface::Formatter> fmt = std::move(fmt_owner);

    be.add_entity(lig::interface::ITransport::Kind::Async, fmt, sinks, kQueueSize);
    be.start();
}

void flush_liglog() {
    lig::logger::getBackend<lig::InnerBackend>().flush();
}

// ---------------- spdlog ----------------

std::shared_ptr<spdlog::logger> g_spd_logger;

void setup_spdlog(int consumers) {
    spdlog::init_thread_pool(kQueueSize, consumers);
    g_spd_logger = std::make_shared<spdlog::async_logger>(
        "spd", std::make_shared<spdlog::sinks::null_sink_mt>(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    g_spd_logger->set_pattern("%v");
    g_spd_logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(g_spd_logger);
}

void flush_spdlog() { g_spd_logger->flush(); }

} // namespace

int main() {
    std::cout << "=== Throughput: liglog vs spdlog ===\n"
              << "Messages/thread : " << kMessagesPerThread << "\n"
              << "Repetitions     : " << kRepetitions << "\n"
              << "Queue size      : " << kQueueSize << "\n"
              << "Payload         : \"value={}\" (1 int)\n\n";

    setup_liglog();
    {
        lig::Info log;
        for (int i = 0; i < 10'000; ++i) log("warmup {}", i);
        flush_liglog();
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::setw(6)  << "prod"  << " | "
              << std::setw(6)  << "cons"  << " | "
              << std::setw(14) << "liglog M/s" << " | "
              << std::setw(14) << "spdlog M/s" << " | "
              << std::setw(8)  << "ratio"  << "\n";
    std::cout << std::string(62, '-') << "\n";

    const std::vector<int> prod_counts = {1, 2, 4, 8};

    // 单消费者：多生产者
    for (int P : prod_counts) {
        // liglog（消费者固定 1）
        double best_lig = 0;
        for (int r = 0; r < kRepetitions; ++r) {
            double tp = measure_throughput(
                P,
                [](int) {
                    lig::Info log;
                    for (size_t i = 0; i < kMessagesPerThread; ++i)
                        log("value={}", static_cast<int>(i));
                },
                [] { flush_liglog(); });
            best_lig = std::max(best_lig, tp);
        }

        // spdlog（消费者固定 1）
        setup_spdlog(1);
        {
            auto logger = g_spd_logger;
            for (int i = 0; i < 10'000; ++i) SPDLOG_LOGGER_INFO(logger, "warmup {}", i);
            flush_spdlog();
        }
        double best_spd = 0;
        for (int r = 0; r < kRepetitions; ++r) {
            double tp = measure_throughput(
                P,
                [](int) {
                    auto logger = g_spd_logger;
                    for (size_t i = 0; i < kMessagesPerThread; ++i)
                        SPDLOG_LOGGER_INFO(logger, "value={}", static_cast<int>(i));
                },
                [] { flush_spdlog(); });
            best_spd = std::max(best_spd, tp);
        }

        double ratio = best_spd > 0 ? best_lig / best_spd : 0;
        std::cout << std::setw(6)  << P              << " | "
                  << std::setw(6)  << 1              << " | "
                  << std::setw(14) << best_lig / 1e6 << " | "
                  << std::setw(14) << best_spd / 1e6 << " | "
                  << std::setw(7)  << ratio          << "x\n";
    }

    std::cout << "\n-- Consumer scaling (spdlog only) --\n";
    std::cout << std::setw(6)  << "prod"  << " | "
              << std::setw(6)  << "cons"  << " | "
              << std::setw(14) << "spdlog M/s" << "\n";
    std::cout << std::string(36, '-') << "\n";

    for (int C : {1, 2, 4, 8}) {
        setup_spdlog(C);
        {
            auto logger = g_spd_logger;
            for (int i = 0; i < 10'000; ++i) SPDLOG_LOGGER_INFO(logger, "warmup {}", i);
            flush_spdlog();
        }
        double best_spd = 0;
        for (int r = 0; r < kRepetitions; ++r) {
            double tp = measure_throughput(
                4,
                [](int) {
                    auto logger = g_spd_logger;
                    for (size_t i = 0; i < kMessagesPerThread; ++i)
                        SPDLOG_LOGGER_INFO(logger, "value={}", static_cast<int>(i));
                },
                [] { flush_spdlog(); });
            best_spd = std::max(best_spd, tp);
        }
        std::cout << std::setw(6) << 4 << " | "
                  << std::setw(6) << C << " | "
                  << std::setw(14) << best_spd / 1e6 << "\n";
    }

    return 0;
}
