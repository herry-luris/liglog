// Benchmark suite for liglog, without any external framework.
//
// Coverage:
//   - FrontendSuppressed            : level filter drops the call
//   - FrontendEnabled_NoArgs        : Info with no args, null transport
//   - FrontendEnabled_IntArg        : Info with one int
//   - FrontendEnabled_MixedArgs     : Info with mixed args
//   - SyncTransportFrom             : SyncTransport::from, null sink
//   - AsyncTransportFrom            : AsyncTransport::from, null sink
//   - AsyncTransportFromFlush       : AsyncTransport::from + flush
//   - EndToEnd_Sync_Minimal         : full Config, sync, console sink
//   - EndToEnd_Async                : full Config, async, console sink
//   - MultiThread_Sync_N            : N threads through sync transport
//   - MultiThread_Async_N           : N threads through async transport
//   - TextFormatter_Full            : TextFormatter with all header fields
//   - TextFormatter_Minimal         : TextFormatter with only level
//   - DataBufferPush                : DataBuffer::push, 64-byte chunks
//   - FileSink_Write                : FileSink::write, 64-byte chunks
//   - MixedWorkload                 : mix of levels and args
//
// Console sinks write to stderr so the benchmark table on stdout stays
// readable; run with `./bench_liglog 2>/dev/null` to silence log output.

#include "bench_help.hpp"
#include "lig.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Test sinks and formatters
// ---------------------------------------------------------------------------

struct NullSink final : lig::interface::Sink {
    auto write(const void *, size_t len) -> WriteRes final {
        WriteRes r{};
        r.len = len;
        return r;
    }
    auto is_valid() const noexcept -> bool final { return true; }
};

struct NullFormatter final : lig::interface::Formatter {
    void format(const lig::LogEntry &, std::vector<std::byte> &) const final {}
};

// Each benchmark scenario gets a distinct backend type, so the singleton
// `getBackend<>()` does not share configuration between scenarios.
template <typename Tag> struct BenchBackend : lig::interface::IBackend<BenchBackend<Tag>> {
    friend struct lig::interface::IBackend<BenchBackend<Tag>>;

    lig::InnerBackend inner;

    void submit_impl(lig::logger::Task &&task) { inner.submit(std::move(task)); }
};

using FrontendBE = BenchBackend<struct FrontendTag>;
using EndToEndSyncBE = BenchBackend<struct EndToEndSyncTag>;
using EndToEndAsyncBE = BenchBackend<struct EndToEndAsyncTag>;
using MultiSyncBE = BenchBackend<struct MultiSyncTag>;
using MultiAsyncBE = BenchBackend<struct MultiAsyncTag>;

void setup_null_sync(lig::InnerBackend &inner) {
    std::unique_ptr<const lig::interface::Formatter> formatter = std::make_unique<NullFormatter>();
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks;
    sinks.push_back(std::make_unique<NullSink>());
    inner.add_entity(lig::interface::ITransport::Kind::Sync, formatter, sinks, 0);
    inner.start();
}

void setup_real_sync(lig::InnerBackend &inner) {
    std::unique_ptr<const lig::interface::Formatter> formatter =
        std::make_unique<lig::component::TextFormatter>();
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks;
    sinks.push_back(std::make_unique<lig::component::ConsoleSink>(
        lig::component::ConsoleSink::Config{.to_err = true}));
    inner.add_entity(lig::interface::ITransport::Kind::Sync, formatter, sinks, 0);
    inner.start();
}

void setup_real_async(lig::InnerBackend &inner) {
    std::unique_ptr<const lig::interface::Formatter> formatter =
        std::make_unique<lig::component::TextFormatter>();
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks;
    sinks.push_back(std::make_unique<lig::component::ConsoleSink>(
        lig::component::ConsoleSink::Config{.to_err = true}));
    inner.add_entity(lig::interface::ITransport::Kind::Async, formatter, sinks, 0);
    inner.start();
}

std::filesystem::path temp_dir(const std::string &name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

lig::logger::Task null_task() {
    return [](const lig::interface::Formatter *, std::vector<std::byte> &) {};
}

} // namespace

// ---------------------------------------------------------------------------
// Front-end. Null transport, so the numbers reflect the front-end cost plus
// Task construction and destruction.
// ---------------------------------------------------------------------------

void bench_frontend_suppressed() {
    lig::logger::g_level.store(lig::Level::Fatal, std::memory_order_relaxed);
    lig::Info logger{};
    bench::print_result(bench::run_bench("FrontendSuppressed", 1'000'000, [&] {
        logger("hello, world");
        bench::do_not_optimize(logger);
    }));
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
}

void bench_frontend_enabled_no_args() {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    lig::Info logger{};
    bench::print_result(bench::run_bench("FrontendEnabled_NoArgs", 200'000, [&] {
        logger("hello, world");
        bench::do_not_optimize(logger);
    }));
}

void bench_frontend_enabled_int_arg() {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    lig::Info logger{};
    bench::print_result(bench::run_bench("FrontendEnabled_IntArg", 100'000, [&] {
        logger("value = {}", 42);
        bench::do_not_optimize(logger);
    }));
}

void bench_frontend_enabled_mixed_args() {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    lig::Info logger{};
    bench::print_result(bench::run_bench("FrontendEnabled_MixedArgs", 50'000, [&] {
        logger("v={} f={} s={}", 42, 3.14, "abc");
        bench::do_not_optimize(logger);
    }));
}

// ---------------------------------------------------------------------------
// Transport layer. Bypass the front-end and call `from` directly on a
// transport with a null formatter and a null sink.
// ---------------------------------------------------------------------------

void bench_sync_transport_from() {
    lig::component::SyncTransport t{};
    std::unique_ptr<const lig::interface::Formatter> formatter = std::make_unique<NullFormatter>();
    t.set_formatter(formatter);
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<NullSink>();
    t.add_sink(sink);
    auto task = null_task();
    bench::print_result(bench::run_bench("SyncTransportFrom", 500'000, [&] { t.from(task); }));
}

void bench_async_transport_from() {
    lig::component::AsyncTransport t{};
    std::unique_ptr<const lig::interface::Formatter> formatter = std::make_unique<NullFormatter>();
    t.set_formatter(formatter);
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<NullSink>();
    t.add_sink(sink);
    t.start();
    auto task = null_task();
    bench::print_result(bench::run_bench("AsyncTransportFrom", 200'000, [&] { t.from(task); }));
    t.stop();
}

void bench_async_transport_from_flush() {
    lig::component::AsyncTransport t{};
    std::unique_ptr<const lig::interface::Formatter> formatter = std::make_unique<NullFormatter>();
    t.set_formatter(formatter);
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<NullSink>();
    t.add_sink(sink);
    t.start();
    auto task = null_task();
    bench::print_result(bench::run_bench("AsyncTransportFromFlush", 100'000, [&] {
        t.from(task);
        t.flush();
    }));
    t.stop();
}

// ---------------------------------------------------------------------------
// End-to-end. Full Config with real formatter and a console sink on stderr.
// ---------------------------------------------------------------------------

void bench_end_to_end_sync_minimal() {
    setup_real_sync(lig::logger::getBackend<EndToEndSyncBE>().inner);
    lig::Info logger{};
    bench::print_result(
        bench::run_bench("EndToEnd_Sync_Minimal", 100'000, [&] { logger("hello, world"); }));
}

void bench_end_to_end_async() {
    setup_real_async(lig::logger::getBackend<EndToEndAsyncBE>().inner);
    lig::Info logger{};
    auto result = bench::run_bench("EndToEnd_Async", 100'000, [&] { logger("hello, world"); });
    lig::logger::getBackend<EndToEndAsyncBE>().inner.flush();
    bench::print_result(result);
}

// ---------------------------------------------------------------------------
// Multi-threaded. ns/op is total wall-clock / total ops across all threads.
// ---------------------------------------------------------------------------

void bench_multi_thread_sync(std::size_t num_threads) {
    static bool configured = false;
    if (!configured) {
        setup_real_sync(lig::logger::getBackend<MultiSyncBE>().inner);
        configured = true;
    }
    lig::Info logger{};
    const std::string name = "MultiThread_Sync/threads:" + std::to_string(num_threads);
    bench::print_result(
        bench::run_bench_mt(name, 200'000, num_threads, [&] { logger("hello, world"); }));
}

void bench_multi_thread_async(std::size_t num_threads) {
    static bool configured = false;
    if (!configured) {
        setup_real_async(lig::logger::getBackend<MultiAsyncBE>().inner);
        configured = true;
    }
    lig::Info logger{};
    const std::string name = "MultiThread_Async/threads:" + std::to_string(num_threads);
    auto result = bench::run_bench_mt(name, 200'000, num_threads, [&] { logger("hello, world"); });
    lig::logger::getBackend<MultiAsyncBE>().inner.flush();
    bench::print_result(result);
}

// ---------------------------------------------------------------------------
// Component micro-benchmarks.
// ---------------------------------------------------------------------------

void bench_text_formatter_full() {
    lig::component::TextFormatter fmt{};
    lig::LogEntry e{};
    e.head.level = lig::Level::Info;
    e.head.line = 42;
    e.head.thread_id = 1234;
    e.head.filename = "bench_liglog.cpp";
    e.head.funcname = "bench";
    e.head.now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    e.body = "hello, world";
    std::vector<std::byte> buf;
    buf.reserve(256);
    bench::print_result(bench::run_bench("TextFormatter_Full", 200'000, [&] {
        buf.clear();
        fmt.format(e, buf);
        bench::do_not_optimize(buf.size());
    }));
}

void bench_text_formatter_minimal() {
    lig::component::TextFormatter fmt{};
    fmt.status.setTimeStamp(false).setThreadId(false).setFuncname(false).setLocation(false);
    lig::LogEntry e{};
    e.head.level = lig::Level::Info;
    e.body = "hello, world";
    std::vector<std::byte> buf;
    buf.reserve(64);
    bench::print_result(bench::run_bench("TextFormatter_Minimal", 500'000, [&] {
        buf.clear();
        fmt.format(e, buf);
        bench::do_not_optimize(buf.size());
    }));
}

void bench_data_buffer_push() {
    lig::component::internal::DataBuffer db{};
    std::vector<std::byte> chunk(64, std::byte{0x41});
    bench::print_result(bench::run_bench("DataBufferPush", 200'000, [&] {
        auto blocks = db.push(chunk.data(), chunk.size());
        bench::do_not_optimize(blocks.size());
    }));
}

void bench_file_sink_write() {
    const auto dir = temp_dir("liglog_bench_file_sink");
    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;
    cfg.file = "bench.log";
    cfg.max_file_size = lig::component::FileSink::MaxFileSize;
    cfg.sync_interval = 0;
    lig::component::FileSink sink{cfg};
    sink.open();
    std::vector<std::byte> chunk(64, std::byte{0x41});
    bench::print_result(bench::run_bench("FileSink_Write", 100'000, [&] {
        auto res = sink.write(chunk.data(), chunk.size());
        bench::do_not_optimize(res.len);
    }));
    sink.close();
    std::filesystem::remove_all(dir);
}

void bench_mixed_workload() {
    // Reuses EndToEndSyncBE; its real sync transport is already configured.
    lig::Info info{};
    lig::Warn warn{};
    lig::Error err{};
    bench::print_result(bench::run_bench("MixedWorkload", 100'000, [&] {
        info("info {}", 1);
        warn("warn {}", 2.0);
        err("error");
    }));
}

int main() {
    // Front-end benchmarks share one null backend.
    setup_null_sync(lig::logger::getBackend<FrontendBE>().inner);

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
    bench_data_buffer_push();
    bench_file_sink_write();
    bench_mixed_workload();

    return 0;
}
