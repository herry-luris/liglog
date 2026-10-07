#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "interface.hpp"
#include "component.hpp"

namespace lig_bench {

// 只读计数，不写内容；用于测量"纯队列/传输"开销
class NullSink final : public lig::interface::Sink {
  public:
    std::atomic<uint64_t> bytes{0};

    auto is_valid() const noexcept -> bool final { return true; }

    auto write(const void *data, size_t len) -> WriteRes final {
        bytes.fetch_add(len, std::memory_order_relaxed);
        return WriteRes{len, {}};
    }
    void close() noexcept final {}
    auto open() -> bool final { return true; }
};

// 真正存储内容；用于验证正确性（在 benchmark 里也可以顺带校验）
class CountingSink final : public lig::interface::Sink {
  public:
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> calls{0};

    auto is_valid() const noexcept -> bool final { return true; }
    auto write(const void *data, size_t len) -> WriteRes final {
        bytes.fetch_add(len, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
        return WriteRes{len, {}};
    }
    void close() noexcept final {}
    auto open() -> bool final { return true; }
};

// 一个典型的带时间戳/线程/位置/函数名的 TextFormatter 配置
inline auto make_full_formatter() -> std::unique_ptr<const lig::interface::Formatter> {
    auto f = std::make_unique<lig::component::TextFormatter>();
    f->status.setTimeStamp(true).setThreadId(true).setFuncname(true).setLocation(true);
    return f;
}

inline auto make_minimal_formatter() -> std::unique_ptr<const lig::interface::Formatter> {
    auto f = std::make_unique<lig::component::TextFormatter>();
    f->status.setTimeStamp(false).setThreadId(false).setFuncname(false).setLocation(false);
    return f;
}

} // namespace lig_bench
