#pragma once
#include "../interface.hpp"
#include <filesystem>
#include <mutex>
#include <atomic>

namespace lig::component {

/// Thread-safe. All public methods — including `set_*` and `config()` —
/// can be called from any thread. `write` / `open` / `close` / `sync` and
/// the setters acquire an internal `std::mutex`; `is_valid()` and `size()`
/// are lock-free atomic reads.
///
/// Filenames are monotonically increasing: `<n>_<original>.log`.
/// `config_.file` is never modified, so rotation does not produce nested
/// names.
class FileSink final : public ::lig::interface::Sink {

  public:
    static constexpr size_t MaxFileSize = 128ull * 1024 * 1024;
    static constexpr size_t MinFileSize = 8ull * 1024 * 1024;

    struct Config {
        size_t max_file_size{(FileSink::MaxFileSize + FileSink::MinFileSize) / 2};
        size_t sync_interval{10};
        std::filesystem::path file{std::filesystem::current_path() / "lig.log"};
        std::filesystem::path dir{std::filesystem::current_path()};
    };

    /// True after a successful `open()` and before `close()`.
    auto is_valid() const noexcept -> bool final;
    /// Write `len` bytes, rotating when necessary. Returns the total number
    /// of bytes accepted by the sink (may be 0 on the first error).
    auto write(const void *data, size_t len) -> WriteRes final;
    /// Flush and close the underlying fd. Safe to call when already closed.
    void close() noexcept final;
    /// Open the file and create parent directories if needed. Returns false
    /// if the path cannot be opened as a regular file.
    auto open() -> bool final;
    /// fsync the underlying fd. Resets the sync counter.
    auto sync() -> bool;

    /// Bytes written to the current file since it was opened.
    auto size() const noexcept -> size_t;
    auto config() const noexcept -> Config;

    /// Update the directory. Returns false if `dir` exists and is not a
    /// directory.
    auto set_directory(const std::filesystem::path &dir) noexcept -> bool;
    /// Update the base filename. When `change_dir` is true, the directory is
    /// also set to the file's parent.
    auto set_file(const std::filesystem::path &file, bool change_dir = false) noexcept -> bool;
    /// Set the number of writes between fsyncs. 0 disables periodic fsync.
    void set_sync_interval(size_t count) noexcept;
    /// Clamp to [MinFileSize, MaxFileSize]. Returns false if out of range.
    auto set_max_file_size(size_t size) noexcept -> bool;

    FileSink(const Config &config);
    ~FileSink() final;

  private:
#if defined(_WIN32)
    void *fd_{};
#else
    int fd_{-1};
#endif
	std::atomic_bool is_valid_{false};
	std::atomic_size_t file_size_{0};
    size_t write_since_sync_{0};
    size_t rotate_count_{0};
    std::filesystem::path rotate_file_{};
    mutable std::mutex mutex_{};
    Config config_{};

    auto get_next_file() -> std::filesystem::path;
    void inner_close() noexcept;
    auto inner_open() -> bool;
    auto inner_sync() -> bool;
};

/// Create a FileSink. Ownership is returned as unique_ptr<Sink>.
auto createFileSink(const component::FileSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;

/// Fluent builder for FileSink.
template <typename Transport>
struct FileSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    using Base = ::lig::interface::SinkConfig<Transport>;

    FileSinkConfig(Transport &config) : Base{config} {}
    ~FileSinkConfig() final = default;

    /// Base filename (without directory). Rotation adds a numeric prefix.
    auto filename(const std::filesystem::path &file) -> FileSinkConfig & {
        config_.file = file;
        return *this;
    }
    /// Number of writes between fsyncs. 0 disables periodic fsync.
    auto sync_interval(size_t count) -> FileSinkConfig & {
        config_.sync_interval = count;
        return *this;
    }
    /// Directory to write into. Created on first open if missing.
    auto dir(const std::filesystem::path &dir) -> FileSinkConfig & {
        config_.dir = dir;
        return *this;
    }
    /// Rotation threshold in bytes. Must be in [MinFileSize, MaxFileSize].
    auto max_file_size(size_t file_size) -> FileSinkConfig & {
        config_.max_file_size = file_size;
        return *this;
    }

    auto start() -> std::unique_ptr<interface::Sink> final { return createFileSink(config_); }

  private:
    component::FileSink::Config config_{};
};

/// Console sink writing to stdout (fd 1) or stderr (fd 2). The fd is not
/// owned by the sink; `close` only marks it invalid.
class ConsoleSink final : public ::lig::interface::Sink {
  public:
    struct Config {
        bool to_err{false};
    };
    ConsoleSink(const Config &config);
    ~ConsoleSink() final;

    auto is_valid() const noexcept -> bool final;
    auto write(const void *data, size_t len) -> WriteRes final;
    void close() noexcept final;

  private:
#if defined(_WIN32)
    void *fd_{};
#else
    int fd_{-1};
#endif
};

/// Create a ConsoleSink. Ownership is returned as unique_ptr<Sink>.
auto createConsoleSink(const component::ConsoleSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;

/// Fluent builder for ConsoleSink.
template <typename Transport>
struct ConsoleSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    using Base = ::lig::interface::SinkConfig<Transport>;
    ConsoleSinkConfig(Transport &transport) : Base{transport} {}
    ~ConsoleSinkConfig() final = default;

    /// Write to stderr instead of stdout.
    auto to_err(bool is_err) -> ConsoleSinkConfig & {
        config_.to_err = is_err;
        return *this;
    }
    auto start() -> std::unique_ptr<interface::Sink> final { return createConsoleSink(config_); }

  private:
    component::ConsoleSink::Config config_{};
};

}; // namespace lig::component
