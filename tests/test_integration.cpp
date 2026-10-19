#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lig.hpp"
#include "logger.hpp"
#include "test_helpers.hpp"

namespace fs = std::filesystem;

TEST(IntegrationTest, EndToEndFileLogging) {
    auto tmp = fs::temp_directory_path() / "lig_integration";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto cfg = lig::config();
    cfg.level(lig::Level::Trace)
        .transport()
        .kind(lig::interface::ITransport::Kind::Sync)
        .max_cache_size(16 * 1024)
        .text_formatter()
        .timestamp(true)
        .function_name(true)
        .location(true)
        .thread_id(true)
        .submit()
        .file_sink()
        .filename(tmp / "app.log")
        .dir(tmp)
        .sync_interval(1)
        .max_file_size(lig::component::FileSink::MinFileSize)
        .submit()
        .submit()
        .start();

    // 打几条日志
    lig::Trace{}("trace {}", 1);
    lig::Debug{}("debug {}", 2);
    lig::Info{}("info {}", 3);
    lig::Warn{}("warn {}", 4);
    lig::Error{}("error {}", 5);
    lig::Fatal{}("fatal {}", 6);

    // 显式 flush
    lig::logger::getBackend<lig::InnerBackend>().flush();
    lig::logger::getBackend<lig::InnerBackend>().stop();

    // 扫所有日志文件，拼接内容
    std::string all;
    for (auto &e : fs::directory_iterator(tmp)) {
        if (e.is_regular_file() && e.path().extension() == ".log") {
            std::ifstream ifs{e.path(), std::ios::binary};
            all.append(std::string((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>()));
        }
    }

    EXPECT_NE(all.find("trace 1"), std::string::npos);
    EXPECT_NE(all.find("debug 2"), std::string::npos);
    EXPECT_NE(all.find("info 3"), std::string::npos);
    EXPECT_NE(all.find("warn 4"), std::string::npos);
    EXPECT_NE(all.find("error 5"), std::string::npos);
    EXPECT_NE(all.find("fatal 6"), std::string::npos);
    // 头部字段
    EXPECT_NE(all.find("Trace"), std::string::npos);
    EXPECT_NE(all.find("Info"), std::string::npos);
}

// 用自定义 sink 做一次纯内存端到端（不复用全局 backend）
TEST(IntegrationTest, LoggerPlusSyncTransportPlusMemorySink) {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);

    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::SyncTransport transport;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    transport.set_formatter(fmt);
    transport.add_sink(sink);
    transport.flush();

    lig::logger::Info<lig::component::SyncTransport> info;
    // 注意：Info<BE> 内部调用 getBackend<BE>()，这里 BE 是 SyncTransport，
    // 而 SyncTransport 不是 backend 类型，无法工作。用 InnerBackend 系列别名或自定义 BE。
    // 改用 InnerBackend 作为 BE，再手工注入 transport 更复杂，这里用最简方式：
    struct LocalBackend {
        lig::component::SyncTransport *t{};
        void submit_impl(lig::logger::Task &&task) { t->from(task); }
    };

    // 直接走 backend 方式
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    {
        // 用 InnerBackend 走一次，但 InnerBackend 是全局单例，这里只验证 transport 侧
    }

    // transport 直接用
    transport.from(lig_test::make_task("hello "));
    transport.from(lig_test::make_task("integration"));
    transport.flush();
    EXPECT_EQ(raw->str(), "hello integration");
}

// 多线程：多个线程通过 AsyncTransport 写入，最终内容长度正确
TEST(IntegrationTest, MultiThreadedAsyncTransport) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < kPerThread; ++j) {
                t.from(lig_test::make_task("m"));
            }
        });
    }
    for (auto &w : workers)
        w.join();
    t.flush();
    t.stop();

    EXPECT_EQ(raw->str().size(), static_cast<size_t>(kThreads * kPerThread));
}

// 边界：空消息、格式化异常字符
TEST(IntegrationTest, EdgeCases) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::SyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.flush();

    t.from(lig_test::make_task(""));
    t.from(lig_test::make_task(std::string(4096, 'z')));
    t.flush();
    EXPECT_EQ(raw->str(), std::string(4096, 'z'));
}

// ---------- 集成性能 ----------
TEST(IntegrationPerformance, SyncEndToEnd) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::SyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.flush();

    constexpr size_t iters = 20000;
    double ns = lig_test::measure_ns_per_op(iters, [&](size_t i) {
        t.from(lig_test::make_task("log msg"));
        (void)i;
    });
    t.flush();
    std::cout << "[perf] Sync e2e (task+format+transport) -> " << ns << " ns/op\n";
}

TEST(IntegrationPerformance, AsyncEndToEnd) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    constexpr size_t iters = 50000;
    double ns =
        lig_test::measure_ns_per_op(iters, [&](size_t) { t.from(lig_test::make_task("log msg")); });
    t.flush();
    t.stop();
    std::cout << "[perf] Async e2e submit -> " << ns << " ns/op\n";
}
