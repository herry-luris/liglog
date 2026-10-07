#pragma once
#include "nlqueue/utils.hpp"
#include <memory>
#include <unistd.h>
#include <vector>
#ifndef LIG_BACKEND_CPP
#define LIG_BACKEND_CPP

#include <cstdint>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <variant>
#include <optional>

#include "logger.hpp"
#include "nlqueue/mpmc.hpp"

namespace lig::backend {

namespace internal {
struct Data {
    Data() = default;
    Data(std::byte *data, size_t size);
    ~Data();
    Data(const Data &) = delete;
    auto operator=(const Data &) = delete;
    Data(Data &&other) noexcept;
    auto operator=(Data &&other) noexcept -> Data &;
    auto data() const noexcept -> const std::byte *;
    auto size() const noexcept -> size_t;

  private:
    std::byte *data_{};
    size_t size_{};
};

class DataBuffer {
  public:
    constexpr static size_t MaxBatchSize = 8 * 1024;
    constexpr static size_t MinBatchSize = 1;
    DataBuffer();
    ~DataBuffer();
    DataBuffer(DataBuffer &&other) noexcept;
    auto operator=(DataBuffer &&other) noexcept -> DataBuffer &;
    DataBuffer(const DataBuffer &) = delete;
    auto operator=(const DataBuffer &) -> DataBuffer & = delete;

    [[nodiscard("now you have the ownership of this data")]] auto flush() -> std::optional<Data>;
    [[nodiscard("now you have the ownership of this data")]] auto push(const std::byte *data,
                                                                       size_t len)
        -> std::vector<Data>;
    auto len() const noexcept -> size_t;
    auto batchSize() const noexcept -> size_t;
    auto setBatchSize(size_t size) -> bool;

  private:
    std::byte *data_{};
    size_t len_{};
    size_t batch_size_ = 2 * 1024; // 64KB

    auto checkValid() -> bool;
};
class SinkWrapper {
  public:
    constexpr static size_t MaxFailDataSize = 128 * 1024;

    SinkWrapper(std::unique_ptr<::lig::interface::Sink> &sink);
    ~SinkWrapper();
    SinkWrapper(const SinkWrapper &) = delete;
    auto operator=(const SinkWrapper &) -> SinkWrapper & = delete;
    void from(const std::vector<std::byte> &datas);
    void open();

  private:
    std::unique_ptr<::lig::interface::Sink> sink_{};
    std::vector<std::byte> fail_data{};

    void store_fail_data(const std::span<const std::byte> &data);
};

} // namespace internal
class SyncTransport {
    struct Entity {
        std::vector<std::byte> cache;
        SyncTransport *instance;
    };
    static thread_local std::vector<std::shared_ptr<SyncTransport::Entity>> entities_;

  public:
    constexpr static size_t MinCacheSize = 10 * 1024;
    constexpr static size_t MaxCacheSize = 64 * 1024;
    SyncTransport(size_t max_cache_size = 0);

    // for SyncTransport, stop() do nothing, just for normalize to Transport
    void stop() {}
    void flush();
    void from(const ::lig::logger::Task &);
    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter);
    void add_sink(std::unique_ptr<::lig::interface::Sink> &sink);
    auto set_max_cache_size(size_t size) noexcept -> bool;

  private:
    size_t max_cache_size_{(MinCacheSize + MaxCacheSize) / 2};
    std::vector<std::unique_ptr<internal::SinkWrapper>> wrappers_{};
    std::unique_ptr<const ::lig::interface::Formatter> formatter_{};

    auto find_entity() -> std::shared_ptr<Entity>;
};

class AsyncTransport {
    constexpr static size_t DataQueueSize = 1024;
    constexpr static size_t MaxTaskSize = 512;
    constexpr static size_t MinCacheSize = 10 * 1024;
    constexpr static size_t MaxCacheSize = 64 * 1024;

  public:
    AsyncTransport() = default;
    ~AsyncTransport();

    void stop();
    void flush();
    void start();
    void from(const ::lig::logger::Task &);

    // set formatter before start
    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter);
    void add_sink(std::unique_ptr<::lig::interface::Sink> &sink);
    auto set_max_cache_size(size_t size) noexcept -> bool;

  private:
    internal::DataBuffer data_buffer_{};
    nlqueue::VyukovMpmc<::lig::logger::Task, MaxTaskSize, nlqueue::Strategy::Wait> task_queue_{};
    // std::atomic_size_t write_notifier_{};

    std::vector<std::unique_ptr<internal::SinkWrapper>> wrappers_{};
    std::unique_ptr<const ::lig::interface::Formatter> formatter_{};

    std::vector<std::byte> cache_{};
    std::atomic_size_t max_cache_size_{(MinCacheSize + MaxCacheSize) / 2};

    std::thread t_{};
    std::mutex overflow_mutex_{};
    std::vector<std::byte> overflow_data_{};
    std::atomic_bool is_overflow_{false};

    std::atomic_bool is_stop_{false};
    std::atomic_size_t flush_notifier_{};

    void push(const std::vector<std::byte> &datas);
    void in_cache(internal::Data &&data);
    void cache_flush();
};

enum struct TransportKind : uint8_t { Async, Sync };
class Transport {
  public:
    using Type = std::variant<AsyncTransport, SyncTransport>;

    Transport(TransportKind kind);
    void stop();
    void flush();
    void start();
    void from(const ::lig::logger::Task &);

    // set formatter before start
    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter);
    void add_sink(std::unique_ptr<::lig::interface::Sink> &sink);
    void set_max_cache_size(size_t max_cache_sizes);

  private:
    TransportKind kind_{TransportKind::Sync};
    Type value_{};
};

struct TransportConfig {
    TransportKind kind{};
    std::unique_ptr<const ::lig::interface::Formatter> formatter{};
    std::vector<std::unique_ptr<::lig::interface::Sink>> sinks{};
    size_t max_cache_sizes{};
};

class InnerBackend : public ::lig::interface::IBackend<InnerBackend> {
  public:
    friend struct ::lig::interface::IBackend<InnerBackend>;

    void start();
    void flush();
    void stop();
    auto add_entity(TransportKind kind,
                    std::unique_ptr<const ::lig::interface::Formatter> &formatter,
                    std::vector<std::unique_ptr<::lig::interface::Sink>> &sinks,
                    size_t max_cache_sizes = 0) -> bool;
    ~InnerBackend();

  private:
    std::vector<std::unique_ptr<Transport>> transports_{};
    std::atomic_bool is_start{false};

    void submit_impl(::lig::logger::Task &&task) {
        for (auto &t : transports_) {
            t->from(task);
        }
    }
};

} // namespace lig::backend

#endif
