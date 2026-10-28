#pragma once
#include "../logger.hpp"
#include "../nlqueue/mpmc.hpp"
#include <mutex>
#include <thread>

namespace lig::component {

namespace internal {

/// Owned buffer of formatted bytes. Moving transfers ownership of the heap
/// array; the source becomes empty. Data is not copyable.
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

/// Accumulates formatted bytes until a full batch is available, then hands
/// out an owned Data block. Used by AsyncTransport to coalesce writes.
///
/// `push` appends; whenever the internal buffer reaches `batch_size_`, a
/// filled Data is emitted and a new buffer allocated. `flush` emits whatever
/// is left (if non-empty).
class DataBuffer {
  public:
    constexpr static size_t MaxBatchSize = 8ull * 1024;
    constexpr static size_t MinBatchSize = 1ull;
    DataBuffer();
    ~DataBuffer();
    DataBuffer(DataBuffer &&other) noexcept;
    auto operator=(DataBuffer &&other) noexcept -> DataBuffer &;
    DataBuffer(const DataBuffer &) = delete;
    auto operator=(const DataBuffer &) -> DataBuffer & = delete;

    /// Emit the current partial buffer as a Data block. Returns nullopt if
    /// the buffer is empty.
    [[nodiscard("now you have the ownership of this data")]] auto flush() -> std::optional<Data>;
    /// Append `len` bytes. May emit zero or more full Data blocks.
    [[nodiscard("now you have the ownership of this data")]] auto push(const std::byte *data,
                                                                       size_t len)
        -> std::vector<Data>;
    auto len() const noexcept -> size_t;
    auto batchSize() const noexcept -> size_t;
    /// Resize the internal buffer. Must be within [MinBatchSize, MaxBatchSize]
    /// and strictly larger than the current used bytes. Returns false if not.
    auto setBatchSize(size_t size) -> bool;

  private:
    std::byte *data_{};
    size_t len_{};
    size_t batch_size_ = 2ull * 1024;

    auto checkValid() -> bool;
};

/// Wraps a Sink and handles write failures.
///
/// On a failed write, the sink is asked to reopen and the unwritten tail is
/// retried once. If the retry also fails, or the reopen fails, the unwritten
/// bytes are appended to `fail_data` (capped at MaxFailDataSize, oldest
/// bytes dropped first). The next successful `from` first attempts to drain
/// `fail_data`.
class SinkWrapper {
  public:
    constexpr static size_t MaxFailDataSize = 128ull * 1024;

    /// Takes ownership of the sink; the passed unique_ptr becomes null.
    SinkWrapper(std::unique_ptr<::lig::interface::Sink> &sink);
    ~SinkWrapper();
    SinkWrapper(const SinkWrapper &) = delete;
    auto operator=(const SinkWrapper &) -> SinkWrapper & = delete;

    /// Write `datas` to the sink, draining any buffered fail_data first.
    void from(const std::vector<std::byte> &datas);
    /// Open the sink (called by the transport at startup).
    void open();

  private:
    std::unique_ptr<::lig::interface::Sink> sink_{};
    std::vector<std::byte> fail_data{};

    void store_fail_data(const std::span<const std::byte> &data);
};

} // namespace internal

/// Synchronous transport. Formats and writes on the caller's thread,
/// buffering into a per-thread cache until `max_cache_size_` is exceeded or
/// `flush()` is called.
///
/// Cache is `thread_local`: each thread that logs gets its own buffer and
/// writes it independently. `flush()` only flushes the calling thread's
/// buffer; other threads' buffers are flushed on their next write that
/// exceeds the cache size, or when the process exits.
class SyncTransport final : public ::lig::interface::ITransport {
    struct Entity {
        std::vector<std::byte> cache;
        SyncTransport *instance;
    };
    static thread_local std::vector<std::shared_ptr<SyncTransport::Entity>> entities_;

  public:
    constexpr static size_t MinCacheSize = 10ull * 1024;
    constexpr static size_t MaxCacheSize = 64ull * 1024;
    SyncTransport(size_t max_cache_size = 0);
    ~SyncTransport() final = default;

    auto kind() -> Kind final { return Kind::Sync; };

    /// Flush the current thread's cache to all sinks.
    void flush() final;
    /// Format and append to the current thread's cache, flushing if it
    /// would exceed `max_cache_size_`.
    void from(const ::lig::logger::Task &) final;

    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &) final;
    void add_sink(std::unique_ptr<::lig::interface::Sink> &) final;

    auto set_max_cache_size(size_t size) noexcept -> bool final;

  private:
    size_t max_cache_size_{(MinCacheSize + MaxCacheSize) / 2};
    std::vector<std::unique_ptr<internal::SinkWrapper>> wrappers_{};
    std::unique_ptr<const ::lig::interface::Formatter> formatter_{};

    auto find_entity() -> std::shared_ptr<Entity>;
};

/// Asynchronous transport. `from` pushes a Task into a bounded MPMC queue
/// (VyukovMpmc, Strategy::Wait). A dedicated worker thread pops tasks,
/// formats them, batches the output via DataBuffer, and writes to the sinks.
///
/// Backpressure: if the queue is full for more than 3ms, the caller formats
/// the message inline and appends it to a mutex-protected overflow buffer,
/// which the worker drains on its next iteration. Under sustained overload
/// the front-end latency degrades from ns to µs but never drops a message.
///
/// flush() enqueues an empty Task and waits for the worker to signal; it
/// blocks until all previously-enqueued tasks have been written to the
/// sinks (still subject to the sinks' own caching, e.g. FileSink's
/// sync_interval).
///
/// Lifecycle: start() spawns the worker, stop() signals it and joins. The
/// destructor calls stop() if needed.
class AsyncTransport final : public ::lig::interface::ITransport {
    constexpr static size_t DataQueueSize = 1024;
    constexpr static size_t MaxTaskSize = 512;
    constexpr static size_t MinCacheSize = 10ull * 1024;
    constexpr static size_t MaxCacheSize = 64ull * 1024;

  public:
    AsyncTransport() = default;
    ~AsyncTransport() final;

    auto kind() -> Kind final { return ITransport::Kind::Async; };

    void stop() final;
    void flush() final;
    void start() final;

    /// Enqueue a task. If the queue stays full for > 3ms, format inline and
    /// append to the overflow buffer instead of blocking indefinitely.
    void from(const ::lig::logger::Task &) final;

    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) final;
    void add_sink(std::unique_ptr<::lig::interface::Sink> &sink) final;

    auto set_max_cache_size(size_t size) noexcept -> bool final;

  private:
    internal::DataBuffer data_buffer_{};
    nlqueue::VyukovMpmc<::lig::logger::Task, MaxTaskSize, nlqueue::Strategy::Wait> task_queue_{};

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

} // namespace lig::component
