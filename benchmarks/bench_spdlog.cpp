// bench/bench_spdlog.cpp
//
// 与 bench_liglog.cpp 场景一一对应的 spdlog 对比基准。
// 场景编号完全对齐：
//   1. FrontendSuppressed         — level 不满足
//   2. FrontendEnabled_NoArgs     — 空格式调用
//   3. FrontendEnabled_IntArg     — 一个整型参数
//   4. FrontendEnabled_MixedArgs  — 混合参数
//   5. TransportFrom_Sync         — 同步传输提交
//   6. TransportFrom_Async        — 异步传输提交
//   7. TransportFromFlush         — 提交 + flush 一次
//   8. EndToEnd_Sync_Minimal      — 最小 formatter 同步端到端
//   9. EndToEnd_Sync_FileFull     — full formatter + 文件同步端到端
//   10. MultiThread_Async         — 1/2/4/8 线程异步扩展
//   11. MultiThread_Sync          — 1/2/4/8 线程同步扩展
//   12. TextFormatter_Full        — 纯格式化（full status）
//   13. TextFormatter_Minimal     — 纯格式化（无时间/位置）
//   14. MixedWorkload             — 90% 被过滤 + 10% 启用
//
// 编译前需要安装 spdlog：
//   Ubuntu:  sudo apt install libspdlog-dev
//   或从源码 fetch： https://github.com/gabime/spdlog

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace {

// ============================================================
// 工具
// ============================================================

// 与 liglog 的 FileSink 配置对齐：sync_interval=0，不触发 fsync
constexpr size_t kAsyncQueueSize = 16 * 1024;
constexpr size_t kAsyncThreads   = 1;

auto temp_file(const std::string &name) -> fs::path {
    auto dir = fs::temp_directory_path() / "spdlog_bench";
    fs::create_directories(dir);
    return dir / name;
}

// null sink logger（单线程 / 多线程）
auto make_null_st_logger(const std::string &name)
    -> std::shared_ptr<spdlog::logger> {
    auto sink = std::make_shared<spdlog::sinks::null_sink_st>();
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_pattern("%v"); // 只输出 body，与 liglog minimal formatter 对齐
    logger->set_level(spdlog::level::trace);
    return logger;
}

auto make_null_mt_logger(const std::string &name)
    -> std::shared_ptr<spdlog::logger> {
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::trace);
    return logger;
}

// full formatter pattern，与 liglog TextFormatter(full) 对齐
// liglog: [Info 2026-10-05 15:15:42.288] 0 (test.cpp:0) test_func:hello world
constexpr const char *kFullPattern =
    "[%l %Y-%m-%d %H:%M:%S.%e] %t (%s:%#) %!:%v";

// minimal pattern，只输出 body
constexpr const char *kMinimalPattern = "%v";

} // namespace

// ============================================================
// 1. 前端延迟（单线程，无 I/O）
// ============================================================

// 1.1 被 level 过滤
static void BM_Spdlog_FrontendSuppressed(benchmark::State &state) {
    auto logger = make_null_st_logger("suppressed");
    logger->set_level(spdlog::level::off);

    for (auto _ : state) {
        SPDLOG_LOGGER_TRACE(logger, "payload {}", 42);
        benchmark::DoNotOptimize(logger);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_FrontendSuppressed)->Unit(benchmark::kNanosecond);

// 1.2 启用，无参数
static void BM_Spdlog_FrontendEnabled_NoArgs(benchmark::State &state) {
    auto logger = make_null_st_logger("enabled_noargs");

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "hello");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_FrontendEnabled_NoArgs)->Unit(benchmark::kNanosecond);

// 1.3 启用，单 int 参数
static void BM_Spdlog_FrontendEnabled_IntArg(benchmark::State &state) {
    auto logger = make_null_st_logger("enabled_int");

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "value={}", 42);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_FrontendEnabled_IntArg)->Unit(benchmark::kNanosecond);

// 1.4 启用，混合参数
static void BM_Spdlog_FrontendEnabled_MixedArgs(benchmark::State &state) {
    auto logger = make_null_st_logger("enabled_mixed");
    const std::string name = "user_id";

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "{}: id={} score={:.2f}", name, 42, 3.14);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_FrontendEnabled_MixedArgs)->Unit(benchmark::kNanosecond);

// ============================================================
// 2. 单线程传输提交（Sync vs Async）
// ============================================================

// 2.1 同步传输提交
static void BM_Spdlog_TransportFrom_Sync(benchmark::State &state) {
    auto logger = make_null_st_logger("sync_transport");

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "payload");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_TransportFrom_Sync)->Unit(benchmark::kNanosecond);

// 2.2 异步传输提交
// spdlog 使用全局线程池，队列默认 8192，这里调到 16K 与 liglog 对齐
static void BM_Spdlog_TransportFrom_Async(benchmark::State &state) {
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreads);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "async_transport", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_pattern(kMinimalPattern);
    logger->set_level(spdlog::level::trace);

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "payload");
    }
    state.SetItemsProcessed(state.iterations());
    logger->flush();
}
BENCHMARK(BM_Spdlog_TransportFrom_Async)->Unit(benchmark::kNanosecond);

// 2.3 异步提交 + flush（每条一次）
static void BM_Spdlog_TransportFromFlush(benchmark::State &state) {
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreads);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "async_flush", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_pattern(kMinimalPattern);
    logger->set_level(spdlog::level::trace);

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "payload");
        logger->flush();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_TransportFromFlush)->Unit(benchmark::kNanosecond);

// ============================================================
// 3. 端到端（含格式化 + 落地）
// ============================================================

// 3.1 同步端到端（minimal formatter，null sink）
static void BM_Spdlog_EndToEnd_Sync_Minimal(benchmark::State &state) {
    auto logger = make_null_st_logger("e2e_sync_minimal");

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "value={}", 42);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_EndToEnd_Sync_Minimal)->Unit(benchmark::kNanosecond);

// 3.2 同步端到端（full formatter + 文件 sink）
static void BM_Spdlog_EndToEnd_Sync_FileFull(benchmark::State &state) {
    auto path = temp_file("full.log");
    // basic_file_sink_mt + 关闭 flush 频率（spdlog 默认自动 flush 策略）
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        path.string(), false /*truncate*/);
    auto logger = std::make_shared<spdlog::logger>("e2e_file_full", sink);
    logger->set_pattern(kFullPattern);
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::off); // 不自动 flush，对齐 liglog sync_interval=0

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "user {} did action {} at {}", 42, "login", 3.14);
    }
    state.SetItemsProcessed(state.iterations());
    logger->flush();
}
BENCHMARK(BM_Spdlog_EndToEnd_Sync_FileFull)->Unit(benchmark::kNanosecond);

// ============================================================
// 4. 多线程扩展性
// ============================================================

// 4.1 异步多线程提交
static void BM_Spdlog_MultiThread_Async(benchmark::State &state) {
    // 在 benchmark 第一次进入时初始化线程池（多线程安全）
    static std::once_flag pool_flag;
    std::call_once(pool_flag, [] {
        spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreads);
    });

    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "mt_async", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_pattern(kMinimalPattern);
    logger->set_level(spdlog::level::trace);

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "value={}", 42);
    }
    state.SetItemsProcessed(state.iterations() * state.threads());
    logger->flush();
}
BENCHMARK(BM_Spdlog_MultiThread_Async)
    ->Unit(benchmark::kNanosecond)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

// 4.2 同步多线程提交（共享同一个 _mt logger）
static void BM_Spdlog_MultiThread_Sync(benchmark::State &state) {
    auto logger = make_null_mt_logger("mt_sync");

    for (auto _ : state) {
        SPDLOG_LOGGER_INFO(logger, "value={}", 42);
    }
    state.SetItemsProcessed(state.iterations() * state.threads());
}
BENCHMARK(BM_Spdlog_MultiThread_Sync)
    ->Unit(benchmark::kNanosecond)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

// ============================================================
// 5. 组件级
// ============================================================

// 5.1 纯 pattern_formatter（full pattern）
static void BM_Spdlog_TextFormatter_Full(benchmark::State &state) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>(kFullPattern);
    spdlog::memory_buf_t dest;

    // 用默认构造 + 成员赋值，避开 log_msg 构造函数的版本差异
    spdlog::details::log_msg msg;
    msg.logger_name = spdlog::string_view_t{"bench-logger"};
    msg.level       = spdlog::level::info;
    msg.payload     = spdlog::string_view_t{"hello world"};
    msg.source      = spdlog::source_loc{"file.cpp", 42, "func"};
    msg.time        = spdlog::log_clock::now();

    for (auto _ : state) {
        dest.clear();
        formatter->format(msg, dest);
        benchmark::DoNotOptimize(dest);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_TextFormatter_Full)->Unit(benchmark::kNanosecond);

// 5.2 纯 pattern_formatter（minimal pattern）
static void BM_Spdlog_TextFormatter_Minimal(benchmark::State &state) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>(kMinimalPattern);
    spdlog::memory_buf_t dest;

    spdlog::details::log_msg msg;
    msg.logger_name = spdlog::string_view_t{"bench-logger"};
    msg.level       = spdlog::level::info;
    msg.payload     = spdlog::string_view_t{"hello world"};
    msg.source      = spdlog::source_loc{"file.cpp", 42, "func"};
    msg.time        = spdlog::log_clock::now();

    for (auto _ : state) {
        dest.clear();
        formatter->format(msg, dest);
        benchmark::DoNotOptimize(dest);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Spdlog_TextFormatter_Minimal)->Unit(benchmark::kNanosecond);

// ============================================================
// 6. 混合负载：90% 被过滤 + 10% 启用
// ============================================================
static void BM_Spdlog_MixedWorkload(benchmark::State &state) {
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreads);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "mixed", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_pattern(kMinimalPattern);
    logger->set_level(spdlog::level::info); // 只输出 info 及以上

    for (auto _ : state) {
        for (int i = 0; i < 9; ++i) {
            SPDLOG_LOGGER_TRACE(logger, "debug info {}", i); // 被过滤
        }
        SPDLOG_LOGGER_INFO(logger, "real log {}", 42);        // 启用
    }
    state.SetItemsProcessed(state.iterations() * 10);
    logger->flush();
}
BENCHMARK(BM_Spdlog_MixedWorkload)->Unit(benchmark::kNanosecond);

// ============================================================
// 入口点
//
// 不用 BENCHMARK_MAIN()，因为它在 benchmark 跑完后直接 return 0，
// 全局静态对象（含 spdlog::thread_pool 单例）会在 atexit 阶段被析构。
// 此时 Windows 上 condition_variable 的唤醒可能被丢弃，导致工作线程永远
// 醒不过来，join 卡死。
//
// 改成显式 main：先 benchmark::Shutdown()，再 spdlog::shutdown()，
// 主动 join 线程池，避免走全局析构路径。
// ============================================================
int main(int argc, char **argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        spdlog::shutdown();
        return 1;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    // 显式关闭 spdlog 全局线程池，避免在 atexit 阶段死锁
    spdlog::shutdown();

    return 0;
}
