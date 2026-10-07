#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "bench_common.hpp"
#include "component.hpp"
#include "interface.hpp"
#include "logger.hpp"
#include "lig.hpp"

namespace fs = std::filesystem;
using namespace lig;

// ============================================================
// 工具：构造不同级别的 logger 实例，避免每次 benchmark 里重复声明
// ============================================================
namespace {

auto temp_file(const std::string &name) -> fs::path {
    auto dir = fs::temp_directory_path() / "lig_bench";
    fs::create_directories(dir);
    return dir / name;
}

// 让文件 sink 每 N 次 sync 一次，与生产配置接近
constexpr size_t kFileSyncInterval = 4096;

} // namespace

// ============================================================
// 1. 前端延迟（单线程，无 I/O）
//    - 用 InnerBackend 的虚调用开销 + std::format 打包
//    - 关注 ns/op 增长曲线
// ============================================================

// 1.1 被 level 过滤：应近似常数，与格式化参数无关
static void BM_FrontendSuppressed(benchmark::State &state) {
    logger::g_level.store(Level::Fatal, std::memory_order_relaxed);
    lig::Info logger;
    for (auto _ : state) {
        logger("payload {}", 42);
        benchmark::DoNotOptimize(logger);
    }
    state.SetItemsProcessed(state.iterations());
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
}
BENCHMARK(BM_FrontendSuppressed)->Unit(benchmark::kNanosecond);

// 1.2 启用，无参数
static void BM_FrontendEnabled_NoArgs(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 16 * 1024);
    be.start();

    Info log;
    for (auto _ : state) {
        log("hello");
    }
    state.SetItemsProcessed(state.iterations());

    be.flush();
    be.stop();
}
BENCHMARK(BM_FrontendEnabled_NoArgs)->Unit(benchmark::kNanosecond);

// 1.3 启用，单 int 参数
static void BM_FrontendEnabled_IntArg(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 16 * 1024);
    be.start();

    Info log;
    for (auto _ : state) {
        log("value={}", 42);
    }
    state.SetItemsProcessed(state.iterations());

    be.flush();
    be.stop();
}
BENCHMARK(BM_FrontendEnabled_IntArg)->Unit(benchmark::kNanosecond);

// 1.4 启用，混合参数（int + double + string）
static void BM_FrontendEnabled_MixedArgs(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 16 * 1024);
    be.start();

    const std::string name = "user_id";
    Info log;
    for (auto _ : state) {
        log("{}: id={} score={:.2f}", name, 42, 3.14);
    }
    state.SetItemsProcessed(state.iterations());

    be.flush();
    be.stop();
}
BENCHMARK(BM_FrontendEnabled_MixedArgs)->Unit(benchmark::kNanosecond);

// ============================================================
// 2. 单线程传输提交（Sync vs Async）
// ============================================================

// 2.1 SyncTransport::from
static void BM_SyncTransportFrom(benchmark::State &state) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_bench::NullSink>();
    backend::SyncTransport t;
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.flush();

    auto make_task = [](std::string body) -> logger::Task {
        return ([body = std::move(body)](const interface::Formatter *f) -> std::vector<std::byte> {
            LogEntry entry;
            entry.head.level = Level::Info;
            entry.body = body;
            std::vector<std::byte> buf;
            f->format(entry, buf);
            return buf;
        });
    };

    for (auto _ : state) {
        t.from(make_task("payload"));
    }
    state.SetItemsProcessed(state.iterations());
    t.flush();
}
BENCHMARK(BM_SyncTransportFrom)->Unit(benchmark::kNanosecond);

// 2.2 AsyncTransport::from
static void BM_AsyncTransportFrom(benchmark::State &state) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_bench::NullSink>();
    backend::AsyncTransport t;
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    auto make_task = [](std::string body) {
        return logger::Task(
            [body = std::move(body)](const interface::Formatter *f) -> std::vector<std::byte> {
                LogEntry entry;
                entry.head.level = Level::Info;
                entry.body = body;
                std::vector<std::byte> buf;
                f->format(entry, buf);
                return buf;
            });
    };

    for (auto _ : state) {
        t.from(make_task("payload"));
    }
    state.SetItemsProcessed(state.iterations());
    t.flush();
    t.stop();
}
BENCHMARK(BM_AsyncTransportFrom)->Unit(benchmark::kNanosecond);

// 2.3 AsyncTransport 提交 + flush（每条一次 flush，测最坏情况延迟）
static void BM_AsyncTransportFromFlush(benchmark::State &state) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_bench::NullSink>();
    backend::AsyncTransport t;
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    auto make_task = [](std::string body) {
        return logger::Task(
            [body = std::move(body)](const interface::Formatter *f) -> std::vector<std::byte> {
                LogEntry entry;
                entry.head.level = Level::Info;
                entry.body = body;
                std::vector<std::byte> buf;
                f->format(entry, buf);
                return buf;
            });
    };

    for (auto _ : state) {
        t.from(make_task("payload"));
        t.flush();
    }
    state.SetItemsProcessed(state.iterations());
    t.stop();
}
BENCHMARK(BM_AsyncTransportFromFlush)->Unit(benchmark::kNanosecond);

// ============================================================
// 3. 端到端（含格式化 + 落地）
// ============================================================

// 3.1 Sync 端到端：Task 构造 → transport → Sink
static void BM_EndToEnd_Sync_Minimal(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    auto *sink_raw = sink.get();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 16 * 1024);
    be.start();

    Info log;
    for (auto _ : state) {
        log("value={}", 42);
    }
    be.flush();

    state.SetItemsProcessed(state.iterations());
    state.counters["bytes"] = benchmark::Counter(static_cast<double>(sink_raw->bytes.load()),
                                                 benchmark::Counter::kIsRate);
    be.stop();
}
BENCHMARK(BM_EndToEnd_Sync_Minimal)->Unit(benchmark::kNanosecond);

// 3.2 Async 端到端
static void BM_EndToEnd_Async(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Async, fmt, sinks, 16 * 1024);
    be.start();

    Info log;
    for (auto _ : state) {
        log("value={}", 42);
    }
    be.flush();
    be.stop();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEnd_Async)->Unit(benchmark::kNanosecond);

// 3.3 完整 TextFormatter + 文件落地
static void BM_EndToEnd_Sync_FileFull(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();

    auto cfg = component::FileSink::Config{};
    cfg.dir = fs::temp_directory_path() / "lig_bench";
    cfg.file = "full.log";
    cfg.sync_interval = 0; // benchmark 里禁用 sync，专注吞吐
    cfg.max_file_size = component::FileSink::MaxFileSize;
    fs::create_directories(cfg.dir);

    auto sink = std::make_unique<component::FileSink>(cfg);
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_full_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 64 * 1024);
    be.start();

    Info log;
    for (auto _ : state) {
        log("user {} did action {} at {}", 42, "login", 3.14);
    }
    be.flush();
    be.stop();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEnd_Sync_FileFull)->Unit(benchmark::kNanosecond);

// ============================================================
// 4. 多线程扩展性
// ============================================================

// 4.1 Async 多线程提交
static void BM_MultiThread_Async(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Async, fmt, sinks, 16 * 1024);
    be.start();

    for (auto _ : state) {
        Info log;
        log("value={}", 42);
    }
    state.SetItemsProcessed(state.iterations() * state.threads());
    be.flush();
    be.stop();
}
BENCHMARK(BM_MultiThread_Async)
    ->Unit(benchmark::kNanosecond)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

// 4.2 Sync 多线程提交
static void BM_MultiThread_Sync(benchmark::State &state) {
    logger::g_level.store(Level::Trace, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Sync, fmt, sinks, 16 * 1024);
    be.start();

    for (auto _ : state) {
        Info log;
        log("value={}", 42);
    }
    state.SetItemsProcessed(state.iterations() * state.threads());
    be.flush();
    be.stop();
}
BENCHMARK(BM_MultiThread_Sync)
    ->Unit(benchmark::kNanosecond)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

// ============================================================
// 5. 组件级
// ============================================================

// 5.1 纯 TextFormatter（full status）
static void BM_TextFormatter_Full(benchmark::State &state) {
    auto fmt = lig_bench::make_full_formatter();
    LogEntry entry;
    entry.head.level = Level::Info;
    entry.head.line = 42;
    entry.head.thread_id = 12345;
    entry.head.filename = "file.cpp";
    entry.head.funcname = "func";
    entry.head.now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    entry.body = "hello world";

    std::vector<std::byte> buf;
    buf.reserve(512);
    for (auto _ : state) {
        buf.clear();
        fmt->format(entry, buf);
        benchmark::DoNotOptimize(buf.data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TextFormatter_Full)->Unit(benchmark::kNanosecond);

// 5.2 纯 TextFormatter（minimal）
static void BM_TextFormatter_Minimal(benchmark::State &state) {
    auto fmt = lig_bench::make_minimal_formatter();
    LogEntry entry;
    entry.head.level = Level::Info;
    entry.body = "hello world";

    std::vector<std::byte> buf;
    buf.reserve(128);
    for (auto _ : state) {
        buf.clear();
        fmt->format(entry, buf);
        benchmark::DoNotOptimize(buf.data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TextFormatter_Minimal)->Unit(benchmark::kNanosecond);

// 5.3 DataBuffer::push
static void BM_DataBufferPush(benchmark::State &state) {
    backend::internal::DataBuffer buf;
    std::vector<std::byte> chunk(128, std::byte{0x42});
    for (auto _ : state) {
        auto res = buf.push(chunk.data(), chunk.size());
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DataBufferPush)->Unit(benchmark::kNanosecond);

// 5.4 FileSink 写入（512B 块）
static void BM_FileSink_Write(benchmark::State &state) {
    auto dir = fs::temp_directory_path() / "lig_bench";
    fs::create_directories(dir);
    component::FileSink::Config cfg;
    cfg.dir = dir;
    cfg.file = "sink_write.log";
    cfg.sync_interval = 0;
    cfg.max_file_size = component::FileSink::MaxFileSize;

    component::FileSink sink{cfg};
    sink.open();
    std::string chunk(512, 'x');
    for (auto _ : state) {
        auto res = sink.write(chunk.data(), chunk.size());
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(chunk.size()));
    sink.close();
}
BENCHMARK(BM_FileSink_Write)->Unit(benchmark::kNanosecond);

// ============================================================
// 6. 混合负载：90% 被过滤 + 10% 启用
// ============================================================
static void BM_MixedWorkload(benchmark::State &state) {
    logger::g_level.store(Level::Info, std::memory_order_relaxed);
    auto &be = logger::getBackend<backend::InnerBackend>();
    auto sink = std::make_unique<lig_bench::NullSink>();
    std::unique_ptr<const interface::Formatter> fmt = lig_bench::make_minimal_formatter();
    std::vector<std::unique_ptr<interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    be.add_entity(backend::TransportKind::Async, fmt, sinks, 16 * 1024);
    be.start();

    Trace trace; // 被过滤
    Info info;   // 启用
    for (auto _ : state) {
        for (int i = 0; i < 9; ++i) {
            trace("debug info {}", i);
        }
        info("real log {}", 42);
    }
    be.flush();
    be.stop();
    state.SetItemsProcessed(state.iterations() * 10);
}
BENCHMARK(BM_MixedWorkload)->Unit(benchmark::kNanosecond);
