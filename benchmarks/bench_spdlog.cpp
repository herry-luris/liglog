// Benchmark suite for spdlog, mirroring bench_liglog.cpp scenario by scenario.
//
// Coverage:
//   - FrontendSuppressed            : level filter drops the call
//   - FrontendEnabled_NoArgs        : spdlog::info with no args
//   - FrontendEnabled_IntArg        : spdlog::info with one int
//   - FrontendEnabled_MixedArgs     : spdlog::info with mixed args
//   - SyncTransportFrom             : sync logger info, null sink
//   - AsyncTransportFrom            : async logger info, null sink
//   - AsyncTransportFromFlush       : async logger info + flush
//   - EndToEnd_Sync_Minimal         : sync logger, console sink on stderr
//   - EndToEnd_Async                : async logger, console sink on stderr
//   - MultiThread_Sync_N            : N threads through sync logger
//   - MultiThread_Async_N           : N threads through async logger
//   - TextFormatter_Full            : pattern_formatter with all flags
//   - TextFormatter_Minimal         : pattern_formatter with level only
//   - MixedWorkload                 : mix of info / warn / error
//
// Console sinks write to stderr so the benchmark table on stdout stays
// readable; run with `./bench_spdlog 2>/dev/null` to silence log output.
//
// Requires spdlog to be available as a CMake target (spdlog::spdlog).

#include "bench_help.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

namespace {

constexpr const char *kMsgNoArgs = "hello, world";
constexpr const char *kMsgIntArg = "value = {}";
constexpr const char *kMsgMixedArgs = "v={} f={} s={}";

// Ensure the global thread pool exists. spdlog ignores repeated calls.
void ensure_thread_pool() {
    if (spdlog::thread_pool() == nullptr) {
        spdlog::init_thread_pool(8192, 1);
    }
}

// Build a logger that writes into a null sink. The sink is stored as
// shared_ptr<sinks::sink> so the async_logger constructor overload can
// resolve unambiguously.
std::shared_ptr<spdlog::logger> make_null_logger(const std::string &name, bool async) {
    std::shared_ptr<spdlog::sinks::sink> sink =
        std::make_shared<spdlog::sinks::null_sink_mt>();

    std::shared_ptr<spdlog::logger> logger;
    if (async) {
        ensure_thread_pool();
        logger = std::make_shared<spdlog::async_logger>(
            name, sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
    } else {
        logger = std::make_shared<spdlog::logger>(name, sink);
    }
    logger->set_level(spdlog::level::trace);
    return logger;
}

// Same shape, but the sink writes to stderr.
std::shared_ptr<spdlog::logger> make_stderr_logger(const std::string &name, bool async) {
    std::shared_ptr<spdlog::sinks::sink> sink =
        std::make_shared<spdlog::sinks::stderr_sink_mt>();

    std::shared_ptr<spdlog::logger> logger;
    if (async) {
        ensure_thread_pool();
        logger = std::make_shared<spdlog::async_logger>(
            name, sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
    } else {
        logger = std::make_shared<spdlog::logger>(name, sink);
    }
    logger->set_level(spdlog::level::trace);
    return logger;
}

} // namespace

// ---------------------------------------------------------------------------
// Front-end
// ---------------------------------------------------------------------------

void bench_frontend_suppressed() {
    auto logger = make_null_logger("frontend_suppressed", false);
    logger->set_level(spdlog::level::off);
    bench::print_result(bench::run_bench("FrontendSuppressed", 1'000'000, [&] {
        logger->info(kMsgNoArgs);
        bench::do_not_optimize(logger);
    }));
}

void bench_frontend_enabled_no_args() {
    auto logger = make_null_logger("frontend_no_args", false);
    bench::print_result(bench::run_bench("FrontendEnabled_NoArgs", 200'000, [&] {
        logger->info(kMsgNoArgs);
        bench::do_not_optimize(logger);
    }));
}

void bench_frontend_enabled_int_arg() {
    auto logger = make_null_logger("frontend_int_arg", false);
    bench::print_result(bench::run_bench("FrontendEnabled_IntArg", 100'000, [&] {
        logger->info(kMsgIntArg, 42);
        bench::do_not_optimize(logger);
    }));
}

void bench_frontend_enabled_mixed_args() {
    auto logger = make_null_logger("frontend_mixed_args", false);
    bench::print_result(bench::run_bench("FrontendEnabled_MixedArgs", 50'000, [&] {
        logger->info(kMsgMixedArgs, 42, 3.14, "abc");
        bench::do_not_optimize(logger);
    }));
}

// ---------------------------------------------------------------------------
// Transport layer
// ---------------------------------------------------------------------------

void bench_sync_transport_from() {
    auto logger = make_null_logger("sync_transport", false);
    bench::print_result(bench::run_bench("SyncTransportFrom", 500'000, [&] {
        logger->info(kMsgNoArgs);
        bench::do_not_optimize(logger);
    }));
}

void bench_async_transport_from() {
    auto logger = make_null_logger("async_transport", true);
    bench::print_result(bench::run_bench("AsyncTransportFrom", 200'000, [&] {
        logger->info(kMsgNoArgs);
        bench::do_not_optimize(logger);
    }));
}

void bench_async_transport_from_flush() {
    auto logger = make_null_logger("async_transport_flush", true);
    bench::print_result(bench::run_bench("AsyncTransportFromFlush", 100'000, [&] {
        logger->info(kMsgNoArgs);
        logger->flush();
        bench::do_not_optimize(logger);
    }));
}

// ---------------------------------------------------------------------------
// End-to-end
// ---------------------------------------------------------------------------

void bench_end_to_end_sync_minimal() {
    auto logger = make_stderr_logger("e2e_sync", false);
    bench::print_result(bench::run_bench("EndToEnd_Sync_Minimal", 100'000, [&] {
        logger->info(kMsgNoArgs);
    }));
}

void bench_end_to_end_async() {
    auto logger = make_stderr_logger("e2e_async", true);
    auto result = bench::run_bench("EndToEnd_Async", 100'000, [&] {
        logger->info(kMsgNoArgs);
    });
    logger->flush();
    bench::print_result(result);
}

// ---------------------------------------------------------------------------
// Multi-threaded
// ---------------------------------------------------------------------------

void bench_multi_thread_sync(std::size_t num_threads) {
    auto logger = make_stderr_logger("mt_sync", false);
    const std::string name = "MultiThread_Sync/threads:" + std::to_string(num_threads);
    bench::print_result(bench::run_bench_mt(name, 200'000, num_threads, [&] {
        logger->info(kMsgNoArgs);
    }));
}

void bench_multi_thread_async(std::size_t num_threads) {
    auto logger = make_stderr_logger("mt_async", true);
    const std::string name = "MultiThread_Async/threads:" + std::to_string(num_threads);
    auto result = bench::run_bench_mt(name, 200'000, num_threads, [&] {
        logger->info(kMsgNoArgs);
    });
    logger->flush();
    bench::print_result(result);
}

// ---------------------------------------------------------------------------
// Component micro-benchmarks
// ---------------------------------------------------------------------------

void bench_text_formatter_full() {
    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        "%Y-%m-%d %H:%M:%S %l %t (%s:%#) %! %v");
    spdlog::memory_buf_t dest;
    spdlog::details::log_msg msg(
        spdlog::source_loc{"bench_spdlog.cpp", 42, "bench_text_formatter_full"},
        "bench", spdlog::level::info, kMsgNoArgs);
    bench::print_result(bench::run_bench("TextFormatter_Full", 200'000, [&] {
        dest.clear();
        formatter->format(msg, dest);
        bench::do_not_optimize(dest.size());
    }));
}

void bench_text_formatter_minimal() {
    auto formatter = std::make_unique<spdlog::pattern_formatter>("%l %v");
    spdlog::memory_buf_t dest;
    spdlog::details::log_msg msg(
        spdlog::source_loc{}, "bench", spdlog::level::info, kMsgNoArgs);
    bench::print_result(bench::run_bench("TextFormatter_Minimal", 500'000, [&] {
        dest.clear();
        formatter->format(msg, dest);
        bench::do_not_optimize(dest.size());
    }));
}

void bench_mixed_workload() {
    auto logger = make_stderr_logger("mixed", false);
    bench::print_result(bench::run_bench("MixedWorkload", 100'000, [&] {
        logger->info("info {}", 1);
        logger->warn("warn {}", 2.0);
        logger->error("error");
    }));
}

int main() {
    bench::print_header();

    bench_frontend_suppressed();
    bench_frontend_enabled_no_args();
    bench_frontend_enabled_int_arg();
    bench_frontend_enabled_mixed_args();

    bench_sync_transport_from();
    bench_async_transport_from();
    bench_async_transport_from_flush();

    bench_end_to_end_sync_minimal();
    bench_end_to_end_async();

    bench_multi_thread_sync(1);
    bench_multi_thread_sync(2);
    bench_multi_thread_sync(4);
    bench_multi_thread_sync(8);

    bench_multi_thread_async(1);
    bench_multi_thread_async(2);
    bench_multi_thread_async(4);
    bench_multi_thread_async(8);

    bench_text_formatter_full();
    bench_text_formatter_minimal();
    bench_mixed_workload();

    return 0;
}
