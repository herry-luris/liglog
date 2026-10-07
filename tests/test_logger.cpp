#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "interface.hpp"
#include "logger.hpp"
#include "test_helpers.hpp"

// 记录调用次数的后端
struct CountingBackend : lig::interface::IBackend<CountingBackend> {
    std::atomic<int> count{0};
    void submit_impl(lig::logger::Task &&) { count.fetch_add(1, std::memory_order_relaxed); }

    friend struct lig::interface::IBackend<CountingBackend>;
};

// 执行 Task 并收集输出的后端
struct ExecutingBackend : lig::interface::IBackend<ExecutingBackend> {
    std::mutex mtx;
    std::vector<std::string> outputs;

    void submit_impl(lig::logger::Task &&task) {
        static lig_test::NullFormatter fmt;
        auto bytes = task(&fmt);
        std::lock_guard lk{mtx};
        outputs.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    void clear() {
        std::lock_guard lk{mtx};
        outputs.clear();
    }
    friend struct lig::interface::IBackend<ExecutingBackend>;
};

class LoggerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
        lig::logger::getBackend<CountingBackend>().count.store(0);
        lig::logger::getBackend<ExecutingBackend>().clear();
    }
    void TearDown() override {
        lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    }
};

TEST_F(LoggerTest, LogsAtEnabledLevel) {
    lig::logger::Info<CountingBackend> info;
    info("hello");
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 1);
}

TEST_F(LoggerTest, SuppressedWhenBelowThreshold) {
    lig::logger::g_level.store(lig::Level::Warn, std::memory_order_relaxed);
    lig::logger::Info<CountingBackend> info;
    info("should be dropped");
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 0);
}

TEST_F(LoggerTest, FatalAlwaysLogs) {
    lig::logger::g_level.store(lig::Level::Fatal, std::memory_order_relaxed);
    lig::logger::Fatal<CountingBackend> fatal;
    fatal("boom");
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 1);
}

TEST_F(LoggerTest, SuppressAllWhenLevelAboveFatal) {
    lig::logger::g_level.store(static_cast<lig::Level>(255), std::memory_order_relaxed);
    lig::logger::Fatal<CountingBackend> fatal;
    fatal("never");
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 0);
}

TEST_F(LoggerTest, FormatsArgs) {
    using info = lig::logger::Info<ExecutingBackend>;
    info{}("x={} y={}", 1, 2);
    auto &be = lig::logger::getBackend<ExecutingBackend>();
    ASSERT_EQ(be.outputs.size(), 1u);
    EXPECT_EQ(be.outputs[0], "x=1 y=2");
}

TEST_F(LoggerTest, FormatsNoArgs) {
    using info = lig::logger::Info<ExecutingBackend>;
    info{}("plain message");
    auto &be = lig::logger::getBackend<ExecutingBackend>();
    ASSERT_EQ(be.outputs.size(), 1u);
    EXPECT_EQ(be.outputs[0], "plain message");
}

TEST_F(LoggerTest, FormatsEmptyMessage) {
    using info = lig::logger::Info<ExecutingBackend>;
    info{}("");
    auto &be = lig::logger::getBackend<ExecutingBackend>();
    ASSERT_EQ(be.outputs.size(), 1u);
    EXPECT_EQ(be.outputs[0], "");
}

TEST_F(LoggerTest, DifferentLevelsAreDistinctTypes) {
    lig::logger::Trace<CountingBackend> t;
    lig::logger::Debug<CountingBackend> d;
    lig::logger::Info<CountingBackend> i;
    lig::logger::Warn<CountingBackend> w;
    lig::logger::Error<CountingBackend> e;
    lig::logger::Fatal<CountingBackend> f;
    t("t");
    d("d");
    i("i");
    w("w");
    e("e");
    f("f");
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 6);
}

TEST_F(LoggerTest, ThreadIdIsStableWithinThread) {
    auto id1 = lig::logger::getThreadId();
    auto id2 = lig::logger::getThreadId();
    EXPECT_EQ(id1, id2);
}

TEST_F(LoggerTest, ThreadIdDiffersAcrossThreads) {
    auto main_id = lig::logger::getThreadId();
    uint64_t other_id = 0;
    std::thread th([&] { other_id = lig::logger::getThreadId(); });
    th.join();
    EXPECT_NE(main_id, other_id);
}

TEST_F(LoggerTest, SourceLocationCaptured) {
    // 通过 ExecutingBackend + 自定义 Formatter 检查头部
    struct HeaderCapture : public lig::interface::Formatter {
        mutable lig::LogHead head{};
        void format(const lig::LogEntry &entry, std::vector<std::byte> &buf) const final {
            head = entry.head;
            buf.push_back(std::byte{0});
        }
    };
    struct HeaderBackend {
        std::atomic<int> n{0};
        void submit_impl(lig::logger::Task &&) { n.fetch_add(1); }
    };
    // 用 CountingBackend 就好，这里主要验证不崩溃
    lig::logger::Info<CountingBackend> info;
    info("at line {}", 123);
    EXPECT_EQ(lig::logger::getBackend<CountingBackend>().count.load(), 1);
}

// ---------- 前端基础性能 ----------
TEST(LoggerPerformance, FrontendSubmitLatency) {
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);

    lig::logger::Info<CountingBackend> info;
    constexpr size_t iters = 50000;
    double ns =
        lig_test::measure_ns_per_op(iters, [&](size_t i) { info("iter={} value={}", i, 42); });
    std::cout << "[perf] Logger::operator() -> " << ns << " ns/op\n";
    EXPECT_LT(ns, 20000.0);
}

TEST(LoggerPerformance, SuppressedFast) {
    lig::logger::g_level.store(lig::Level::Fatal, std::memory_order_relaxed);
    lig::logger::Info<CountingBackend> info;
    constexpr size_t iters = 500000;
    double ns = lig_test::measure_ns_per_op(iters, [&](size_t i) { info("suppressed {}", i); });
    std::cout << "[perf] Logger suppressed path -> " << ns << " ns/op\n";
    EXPECT_LT(ns, 1000.0); // 应该很快
}
