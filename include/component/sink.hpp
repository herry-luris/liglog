#pragma once
#include "../interface.hpp"
#include <filesystem>

namespace lig::component {
// =======================================================================
class FileSink final : public ::lig::interface::Sink {

  public:
    static constexpr size_t MaxFileSize = 128 * 1024 * 1024;
    static constexpr size_t MinFileSize = 8 * 1024 * 1024;

    struct Config {
        size_t max_file_size{(FileSink::MaxFileSize + FileSink::MinFileSize) / 2};
        size_t sync_interval{10};
        std::filesystem::path file{};
        std::filesystem::path dir{std::filesystem::current_path()};
    };
    auto is_valid() const noexcept -> bool final;
    auto write(const void *data, size_t len) -> WriteRes final;
    void close() noexcept final;
    auto open() -> bool final;
    auto sync() -> bool;

    auto size() const noexcept -> size_t;
    auto config() const noexcept -> const Config &;
    auto set_directory(const std::filesystem::path &dir) noexcept -> bool;
    auto set_file(const std::filesystem::path &file, bool change_dir = false) noexcept -> bool;
    void set_sync_interval(size_t count) noexcept;
    auto set_max_file_size(size_t size) noexcept -> bool;

    FileSink(const Config &config);
    ~FileSink() final;

  private:
#if defined(_WIN32)
    void *fd_{};
#else
    int fd_{-1};
#endif
    bool is_valid_{false};
    size_t file_size_{0};
    size_t write_since_sync_{0};
    size_t rotate_count_{0};
    Config config_{};

    auto get_next_file() noexcept -> std::filesystem::path;
};
auto createFileSink(const component::FileSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;

template <typename Transport>
struct FileSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    using Base = ::lig::interface::SinkConfig<Transport>;

    FileSinkConfig(Transport &config) : Base{config} {}
    ~FileSinkConfig() final = default;

    auto filename(const std::filesystem::path &file) -> FileSinkConfig & {
        config_.file = file;
        return *this;
    }
    auto sync_interval(size_t count) -> FileSinkConfig & {
        config_.sync_interval = count;
        return *this;
    }
    auto dir(const std::filesystem::path &dir) -> FileSinkConfig & {
        config_.dir = dir;
        return *this;
    }
    auto max_file_size(size_t file_size) -> FileSinkConfig & {
        config_.max_file_size = file_size;
        return *this;
    }

    auto start() -> std::unique_ptr<interface::Sink> final { return createFileSink(config_); }

  private:
    component::FileSink::Config config_{};
};

// =======================================================================
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
auto createConsoleSink(const component::ConsoleSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;

template <typename Transport>
struct ConsoleSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    using Base = ::lig::interface::SinkConfig<Transport>;
    ConsoleSinkConfig(Transport &transport) : Base{transport} {}
    ~ConsoleSinkConfig() final = default;

    auto to_err(bool is_err) -> ConsoleSinkConfig & {
        config_.to_err = is_err;
        return *this;
    }
    auto start() -> std::unique_ptr<interface::Sink> final { return createConsoleSink(config_); }

  private:
    component::ConsoleSink::Config config_{};
};

}; // namespace lig::component
