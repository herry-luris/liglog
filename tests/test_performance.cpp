#include <gtest/gtest.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "component/formatter.hpp"
#include "component/transport.hpp"
#include "test_helpers.hpp"

namespace {

struct Report {
    std::string name;
    double ns_per_op{};
    double ops_per_sec{};
};

void print(const Report &r) {
    std::cout << std::left << std::setw(40) << r.name << std::right << std::setw(10) << r.ns_per_op
              << " ns/op" << std::setw(14) << r.ops_per_sec / 1e6 << " M ops/s\n";
}

} // namespace

TEST(PerformanceSuite, Summary) {
    std::vector<Report> reports;

    // 1. TextFormatter
    {
        lig::component::TextFormatter fmt;
        lig::LogEntry e;
        e.head.level = lig::Level::Info;
        e.head.line = 1;
        e.head.thread_id = 1;
        e.head.filename = "f.cpp";
        e.head.funcname = "fn";
        e.head.now =
            std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        e.body = "payload";
        std::vector<std::byte> buf;
        size_t iters = 100000;
        double ns = lig_test::measure_ns_per_op(iters, [&](size_t) {
            buf.clear();
            fmt.format(e, buf);
        });
        reports.push_back({"TextFormatter::format", ns, 1e9 / ns});
    }

    // 2. MemorySink write
    {
        lig_test::MemorySink sink;
        std::string chunk(256, 'x');
        size_t iters = 200000;
        double ns = lig_test::measure_ns_per_op(iters, [&](size_t) {
            auto r = sink.write(chunk.data(), chunk.size());
            (void)r;
        });
        reports.push_back({"MemorySink::write 256B", ns, 1e9 / ns});
    }

    // 3. DataBuffer::push
    {
        lig::component::internal::DataBuffer buf;
        std::vector<std::byte> chunk(128, std::byte{0});
        size_t iters = 200000;
        double ns = lig_test::measure_ns_per_op(
            iters, [&](size_t) { (void)buf.push(chunk.data(), chunk.size()); });
        reports.push_back({"DataBuffer::push 128B", ns, 1e9 / ns});
    }

    // 4. SyncTransport from
    {
        std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
        lig::component::SyncTransport t;
        std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
        t.set_formatter(fmt);
        t.add_sink(sink);
        t.flush();
        size_t iters = 50000;
        double ns = lig_test::measure_ns_per_op(
            iters, [&](size_t) { t.from(lig_test::make_task("payload")); });
        t.flush();
        reports.push_back({"SyncTransport::from", ns, 1e9 / ns});
    }

    // 5. AsyncTransport from
    {
        std::unique_ptr<lig::interface::Sink> sink = std::make_unique<lig_test::MemorySink>();
        lig::component::AsyncTransport t;
        std::unique_ptr<const lig::interface::Formatter> fmt{new lig_test::NullFormatter{}};
        t.set_formatter(fmt);
        t.add_sink(sink);
        t.start();
        size_t iters = 100000;
        double ns = lig_test::measure_ns_per_op(
            iters, [&](size_t) { t.from(lig_test::make_task("payload")); });
        t.flush();
        t.stop();
        reports.push_back({"AsyncTransport::from", ns, 1e9 / ns});
    }

    std::cout << "\n=== Performance summary ===\n";
    for (auto &r : reports)
        print(r);

    SUCCEED();
}
