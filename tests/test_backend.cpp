#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

#include "lig.hpp"
#include "test_helpers.hpp"

// ---------- DataBuffer ----------
TEST(DataBufferTest, DefaultState) {
    lig::component::internal::DataBuffer buf;
    EXPECT_EQ(buf.len(), 0u);
    EXPECT_GE(buf.batchSize(), lig::component::internal::DataBuffer::MinBatchSize);
    EXPECT_LE(buf.batchSize(), lig::component::internal::DataBuffer::MaxBatchSize);
}
TEST(DataBufferTest, PushSmallKeepsInBuffer) {
    lig::component::internal::DataBuffer buf;
    std::vector<std::byte> data(100, std::byte{0xAB});
    auto out = buf.push(data.data(), data.size());
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(buf.len(), 100u);
}

TEST(DataBufferTest, PushExactlyBatchSizeEmitsOne) {
    lig::component::internal::DataBuffer buf;
    ASSERT_TRUE(buf.setBatchSize(1024));
    std::vector<std::byte> data(1024, std::byte{0xCD});
    auto out = buf.push(data.data(), data.size());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].size(), 1024u);
    EXPECT_EQ(buf.len(), 0u);
}

TEST(DataBufferTest, PushMultiBatch) {
    lig::component::internal::DataBuffer buf;
    ASSERT_TRUE(buf.setBatchSize(1024));
    std::vector<std::byte> data(3000, std::byte{0xEE});
    auto out = buf.push(data.data(), data.size());
    EXPECT_EQ(out.size(), 2u);
    EXPECT_EQ(buf.len(), 3000u - 2 * 1024u);
}

TEST(DataBufferTest, FlushBelowMinReturnsNullopt) {
    lig::component::internal::DataBuffer buf;
    auto r = buf.flush();
    EXPECT_FALSE(r.has_value());
}

TEST(DataBufferTest, FlushMovesOwnership) {
    lig::component::internal::DataBuffer buf;
    std::vector<std::byte> data(64, std::byte{0x11});
    buf.push(data.data(), data.size());
    auto r = buf.flush();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 64u);
    EXPECT_EQ(buf.len(), 0u);
}

TEST(DataBufferTest, SetBatchSizeRejectsInvalid) {
    lig::component::internal::DataBuffer buf;
    EXPECT_FALSE(buf.setBatchSize(0));
    EXPECT_FALSE(buf.setBatchSize(lig::component::internal::DataBuffer::MaxBatchSize + 1));
}

// ---------- SinkWrapper ----------
TEST(SinkWrapperTest, ForwardsToSink) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = sink.get();
    lig::component::internal::SinkWrapper wrapper{sink};

    std::vector<std::byte> data = {std::byte{'h'}, std::byte{'i'}};
    wrapper.from(data);
    EXPECT_EQ(static_cast<lig_test::MemorySink *>(raw)->str(), "hi");
}

TEST(SinkWrapperTest, EmptyDataDoesNothing) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = sink.get();
    lig::component::internal::SinkWrapper wrapper{sink};

    std::vector<std::byte> data;
    wrapper.from(data);
    EXPECT_EQ(static_cast<lig_test::MemorySink *>(raw)->write_count.load(), 0);
}

TEST(SinkWrapperTest, StoresFailDataOnError) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());
    raw->fail_writes.store(true);
    lig::component::internal::SinkWrapper wrapper{sink};

    std::vector<std::byte> data = {std::byte{'a'}, std::byte{'b'}};
    wrapper.from(data);
    EXPECT_EQ(raw->str(), ""); // 失败数据被缓存

    // 恢复写入，下一次写入会先 flush 缓存
    raw->fail_writes.store(false);
    std::vector<std::byte> data2 = {std::byte{'c'}};
    wrapper.from(data2);
    EXPECT_EQ(raw->str(), "abc");
}

// ---------- SyncTransport ----------
using SyncTransport = lig::component::SyncTransport;
namespace {
void reset_sync(SyncTransport &t) {
    // 清理 thread_local 缓存
    t.flush();
}
} // namespace

TEST(SyncTransportTest, WriteAndFlush) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::SyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    reset_sync(t);

    t.from(lig_test::make_task("hello "));
    t.from(lig_test::make_task("world"));
    t.flush();

    EXPECT_EQ(raw->str(), "hello world");
}

TEST(SyncTransportTest, SetMaxCacheSizeRejectsInvalid) {
    lig::component::SyncTransport t;
    EXPECT_FALSE(t.set_max_cache_size(0));
    EXPECT_FALSE(t.set_max_cache_size(lig::component::SyncTransport::MaxCacheSize + 1));
    EXPECT_TRUE(t.set_max_cache_size(lig::component::SyncTransport::MinCacheSize));
}

// ---------- AsyncTransport ----------
TEST(AsyncTransportTest, WriteAndFlush) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    t.from(lig_test::make_task("async "));
    t.from(lig_test::make_task("log"));
    t.flush();

    EXPECT_EQ(raw->str(), "async log");
    t.stop();
}

TEST(AsyncTransportTest, FlushOnEmptyIsSafe) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();
    t.flush();
    t.flush();
    t.stop();
    SUCCEED();
}

TEST(AsyncTransportTest, MultipleProducers) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i] {
            for (int j = 0; j < kPerThread; ++j) {
                t.from(lig_test::make_task("x"));
            }
        });
    }
    for (auto &w : workers)
        w.join();
    t.flush();

    EXPECT_EQ(raw->str().size(), static_cast<size_t>(kThreads * kPerThread));
    t.stop();
}

// ---------- Transport ----------
TEST(TransportTest, SyncDispatch) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    std::unique_ptr<lig::interface::ITransport> t{
        std::make_unique<lig::component::SyncTransport>()};
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t->set_formatter(fmt);
    t->add_sink(sink);
    t->start();
    t->from(lig_test::make_task("s"));
    t->flush();
    EXPECT_EQ(raw->str(), "s");
    t->stop();
}

TEST(TransportTest, AsyncDispatch) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    auto *raw = static_cast<lig_test::MemorySink *>(sink.get());

    std::unique_ptr<lig::interface::ITransport> t{
        std::make_unique<lig::component::AsyncTransport>()};
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t->set_formatter(fmt);
    t->add_sink(sink);
    t->start();
    t->from(lig_test::make_task("a"));
    t->flush();
    EXPECT_EQ(raw->str(), "a");
    t->stop();
}

// ---------- InnerBackend ----------
TEST(InnerBackendTest, RejectAddEntityAfterStart) {
    lig::InnerBackend component;
    component.start();
    auto sink = std::make_unique<lig_test::MemorySink>();
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks;
    sinks.push_back(std::move(sink));
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    EXPECT_FALSE(component.add_entity(lig::interface::ITransport::Kind::Sync, fmt, sinks, 0));
}

// ---------- 后端基础性能 ----------
TEST(BackendPerformance, DataBufferPushThroughput) {
    lig::component::internal::DataBuffer buf;
    const std::vector<std::byte> chunk(128, std::byte{0x42});
    constexpr size_t iters = 200000;

    double ns = lig_test::measure_ns_per_op(
        iters, [&](size_t) { (void)buf.push(chunk.data(), chunk.size()); });
    std::cout << "[perf] DataBuffer::push 128B -> " << ns << " ns/op\n";
    EXPECT_LT(ns, 20000.0);
}

TEST(BackendPerformance, SyncTransportThroughput) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::SyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.flush();

    constexpr size_t iters = 50000;
    double ns =
        lig_test::measure_ns_per_op(iters, [&](size_t) { t.from(lig_test::make_task("payload")); });
    t.flush();

    double ops_per_s = 1e9 / ns;
    std::cout << "[perf] SyncTransport::from -> " << ns << " ns/op (" << ops_per_s / 1e6
              << " M ops/s)\n";
}

TEST(BackendPerformance, AsyncTransportThroughput) {
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    constexpr size_t iters = 100000;
    double ns =
        lig_test::measure_ns_per_op(iters, [&](size_t) { t.from(lig_test::make_task("payload")); });
    t.flush();
    t.stop();

    double ops_per_s = 1e9 / ns;
    std::cout << "[perf] AsyncTransport::from -> " << ns << " ns/op (" << ops_per_s / 1e6
              << " M ops/s)\n";
}

TEST(BackendPerformance, AsyncTransportDrainLatency) {
    // 测 flush 的端到端延迟
    std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
    lig::component::AsyncTransport t;
    std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
    t.set_formatter(fmt);
    t.add_sink(sink);
    t.start();

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        t.from(lig_test::make_task("msg"));
        t.flush();
    }
    auto end = std::chrono::steady_clock::now();
    t.stop();

    double ns = std::chrono::duration<double, std::nano>(end - start).count() / 1000.0;
    std::cout << "[perf] AsyncTransport from+flush -> " << ns << " ns/round\n";
}
