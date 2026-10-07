#pragma once
#include <filesystem>
#include "interface.hpp"

namespace lig::component {
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
    int fd_{-1};
    bool is_valid_{false};
    size_t file_size_{0};
    size_t write_since_sync_{0};
    size_t rotate_count_{0};
    Config config_{};

    auto get_next_file() noexcept -> std::filesystem::path;
};

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
    int fd_{-1};
};

class TextFormatter final : public ::lig::interface::Formatter {
  public:
    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final ;
    ~TextFormatter() final = default;
};

} // namespace lig::component
