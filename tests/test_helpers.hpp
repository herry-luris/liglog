#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <system_error>
#include <vector>

#include "interface.hpp"
#include "logger.hpp"

namespace lig_test {

// 一个内存 Sink，用来在测试中替代真实文件/控制台
class MemorySink final : public lig::interface::Sink {
  public:
    std::vector<std::byte> data;
    std::atomic<int> write_count{0};
    std::atomic<int> open_count{0};
    std::atomic<bool> fail_writes{false};
    std::atomic<bool> valid{true};

    auto is_valid() const noexcept -> bool final { return valid.load(std::memory_order_relaxed); }

    auto write(const void *d, size_t len) -> WriteRes final {
        write_count.fetch_add(1, std::memory_order_relaxed);
        if (fail_writes.load(std::memory_order_relaxed)) {
            return WriteRes{0, std::make_error_code(std::errc::io_error)};
        }
        auto p = static_cast<const std::byte *>(d);
        data.insert(data.end(), p, p + len);
        return WriteRes{len, {}};
    }
    void close() noexcept final { valid.store(false); }
    auto open() -> bool final {
        open_count.fetch_add(1, std::memory_order_relaxed);
        valid.store(true);
        return true;
    }

    std::string str() const {
        return std::string(reinterpret_cast<const char *>(data.data()), data.size());
    }
    void clear() { data.clear(); }
};

// 直接把 body 原样输出的 Formatter
class NullFormatter final : public lig::interface::Formatter {
  public:
    void format(const lig::LogEntry &entry, std::vector<std::byte> &buf) const final {
        buf.insert(buf.end(), reinterpret_cast<const std::byte *>(entry.body.data()),
                   reinterpret_cast<const std::byte *>(entry.body.data() + entry.body.size()));
    }
};

// 生成一个可提交到 backend 的 Task，body 会被 formatter 消费
inline lig::logger::Task make_task(std::string body) {
    lig::logger::Task task([body = std::move(body)](const lig::interface::Formatter *f,
                                                    std::vector<std::byte> &buf) -> void {
        lig::LogEntry entry;
        entry.head.level = lig::Level::Info;
        entry.head.line = 0;
        entry.head.thread_id = 0;
        entry.head.filename = "test.cpp";
        entry.head.funcname = "test_func";
        entry.head.now =
            std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        entry.body = body;
        f->format(entry, buf);
    });
    return task;
}

// 简单计时：返回每次操作平均耗时（纳秒）
template <typename F> double measure_ns_per_op(size_t iters, F &&f) {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iters; ++i) {
        f(i);
    }
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() /
           static_cast<double>(iters);
}

} // namespace lig_test
