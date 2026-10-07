#include "interface.hpp"
#include <iterator>
#include <span>
#if defined(_WIN32)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "component.hpp"

namespace lig::component {
static auto write_impl(int fd, const void *data, size_t len) -> ::lig::interface::Sink::WriteRes {
    ::lig::interface::Sink::WriteRes res{};
    size_t total = 0;
    while (total < len) {
#if defined(_WIN32)
#else
        ssize_t n = ::write(fd, static_cast<const std::byte *>(data) + total, len - total);
#endif
        if (n < 0) {
            if (std::error_code{errno, std::system_category()} == std::errc::interrupted) {
                continue;
            }
            res.err = std::error_code{errno, std::system_category()};
            return res;
        }
        if (n == 0) {
            res.err = std::make_error_code(std::errc::io_error);
            return res;
        }
        total += n;
        res.len = total;
    }
    return res;
}

// sink method implement

// ------ file
FileSink::FileSink(const Config &config) {
    if (!set_directory(config.dir)) {
        config_.dir = std::filesystem::current_path();
    }
    if (set_file(config.file)) {
        config_.file = get_next_file();
    }
    if (!set_max_file_size(config.max_file_size)) {
        config_.max_file_size = ((MaxFileSize + MinFileSize) / 2);
    }
    set_sync_interval(config.sync_interval);
    // config_.rotate_format = config.rotate_format;
}
FileSink::~FileSink() { close(); }

auto FileSink::is_valid() const noexcept -> bool { return is_valid_; }
auto FileSink::write(const void *data, size_t len) -> WriteRes {
    WriteRes total_res{};
    size_t eaten = 0;
    while (file_size_ + len >= config_.max_file_size) {
        size_t left = config_.max_file_size - file_size_;
        auto res =
            ::lig::component::write_impl(fd_, static_cast<const std::byte *>(data) + eaten, left);
        if (!res) {
            return res;
        }
        eaten += res.len;
        file_size_ += res.len;
        len -= res.len;
        total_res.len += res.len;
        if (!open()) {
            res.err = std::error_code{};
            return res;
        }
    }
    if (len != 0) {
        auto res =
            ::lig::component::write_impl(fd_, static_cast<const std::byte *>(data) + eaten, len);
        if (!res) {
            return res;
        }
        file_size_ += res.len;
        total_res.len += res.len;
        write_since_sync_++;
        if (config_.sync_interval != 0 && write_since_sync_ == config_.sync_interval) {
            sync();
        }
    }
    return total_res;
}
void FileSink::close() noexcept {
    if (is_valid_) {
        sync();
#if defined(_WIN32)
#else
        ::close(fd_);
#endif
        is_valid_ = false;
        fd_ = -1;
    }
}
auto FileSink::open() -> bool {
    close();
    if (!std::filesystem::exists(config_.file.parent_path())) {
        if (!std::filesystem::create_directory(config_.file.parent_path())) {
            return false;
        }
    }
#if defined(_WIN32)
#else
    const int f = ::open(config_.file.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644);
    if (f < 0) {
        return false;
    }
    struct stat s{};
    if (::fstat(f, &s) != 0 || !S_ISREG(s.st_mode)) {
        ::close(f);
        return false;
    }
    is_valid_ = true;
    file_size_ = s.st_size;
#endif
    fd_ = f;
    config_.file = get_next_file();
    return true;
}
auto FileSink::sync() -> bool {
    if (is_valid_) {
#if defined(_WIN32)
#else
        ::fsync(fd_);
#endif
        write_since_sync_ = 0;
        return true;
    }
    return false;
}
auto FileSink::size() const noexcept -> size_t { return file_size_; }
auto FileSink::config() const noexcept -> const Config & { return config_; }
auto FileSink::set_directory(const std::filesystem::path &dir) noexcept -> bool {
    if (std::filesystem::exists(dir) && !std::filesystem::is_directory(dir)) {
        return false;
    }
    config_.dir = dir;
    return true;
}
auto FileSink::set_file(const std::filesystem::path &file, bool change_dir) noexcept -> bool {
    if (std::filesystem::is_directory(file)) {
        return false;
    }
    config_.file = file;
    if (change_dir) {
        config_.dir = file.parent_path();
    }
    return true;
}
void FileSink::set_sync_interval(size_t count) noexcept { config_.sync_interval = count; }
auto FileSink::set_max_file_size(size_t size) noexcept -> bool {
    if (size > MaxFileSize || size < MinFileSize) {
        return false;
    }
    config_.max_file_size = size;
    return true;
}

auto FileSink::get_next_file() noexcept -> std::filesystem::path {
    rotate_count_++;
    return config_.dir /
           std::string{std::format("{}_lig{}.log", rotate_count_, config_.file.filename().c_str())};
}

// -------- console
ConsoleSink::ConsoleSink(const Config &config) { fd_ = config.to_err ? 2 : 1; };
ConsoleSink::~ConsoleSink() { close(); }
auto ConsoleSink::is_valid() const noexcept -> bool { return fd_ == 1 || fd_ == 2; }
auto ConsoleSink::write(const void *data, size_t len) -> WriteRes {
    return ::lig::component::write_impl(fd_, data, len);
}
void ConsoleSink::close() noexcept { fd_ = -1; }

void TextFormatter::format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const {
    // [level now] thread_id (filename:line) funcname:
    thread_local std::string cache{};
    cache.clear();

    auto push_str = [&](std::string_view s) {
        const size_t new_capacity = cache.size() + s.size();
        if (new_capacity > cache.capacity()) {
            cache.reserve(new_capacity);
        }
        cache.insert(cache.end(), s.begin(), s.end());
    };

    push_str("[");
    push_str(get_level_string(entry.head.level));
    if (status.hasTimeStamp()) {
        std::format_to(std::back_insert_iterator<std::string>(cache), " {:%Y-%m-%d %H:%M:%S}",
                       entry.head.now);
    }
    push_str("]");
    if (status.hasThreadId()) {
        push_str(" ");
        push_str(std::to_string(entry.head.thread_id));
    }
    if (status.hasLocation()) {
        push_str(" (");
        push_str(entry.head.filename ? entry.head.filename : "<unkown>");
        push_str(" :");
        push_str(std::to_string(entry.head.line));
    }
    if (status.hasFuncname()) {
        push_str(" ");
        push_str(entry.head.funcname ? entry.head.funcname : "<unkown>");
    }
    push_str(entry.body);

    const auto data = std::as_bytes(std::span<const char>(cache));
    buf.insert(buf.end(), data.begin(), data.end());
}
} // namespace lig::component
