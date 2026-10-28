#include "component/sink.hpp"
#include <mutex>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lig::component {
#if defined(_WIN32)
static auto write_impl(void *fd, const void *data, size_t len) -> ::lig::interface::Sink::WriteRes {
#else
static auto write_impl(int fd, const void *data, size_t len) -> ::lig::interface::Sink::WriteRes {
#endif
    ::lig::interface::Sink::WriteRes res{};
    size_t total = 0;
    while (total < len) {
#if defined(_WIN32)
        const DWORD remaining = std::min(static_cast<DWORD>(len - total), 0xfffffffful);
        DWORD n{};
        if (!::WriteFile(reinterpret_cast<HANDLE>(fd), static_cast<const std::byte *>(data) + total,
                         len - total, &n, nullptr)) {
            res.len = n;
            res.err = std::error_code{static_cast<int>(::GetLastError()), std::system_category()};
        }
#else
        ssize_t n = ::write(fd, static_cast<const std::byte *>(data) + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            res.err = std::error_code{errno, std::system_category()};
            return res;
        }
#endif
        if (n == 0) {
            res.err = std::make_error_code(std::errc::io_error);
            return res;
        }
        total += n;
        res.len = total;
    }
    return res;
}
// file sink begin
FileSink::FileSink(const Config &config) {
    if (!set_directory(config.dir)) {
        config_.dir = std::filesystem::current_path();
    }
    if (!set_file(config.file)) {
        rotate_file_ = get_next_file();
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
    std::scoped_lock guard{mutex_};
    WriteRes total_res{};
    if (len == 0) {
        return total_res;
    }
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
        if (!inner_open()) {
            total_res.err = std::make_error_code(std::errc::io_error); // 非空 → SinkWrapper 会重试
            return total_res;
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
            inner_sync();
        }
    }
    return total_res;
}
void FileSink::close() noexcept {
    std::scoped_lock lock{mutex_};
    inner_close();
}
auto FileSink::open() -> bool {
    std::scoped_lock lock{mutex_};
    return inner_open();
}
auto FileSink::sync() -> bool {
    std::scoped_lock lock{mutex_};
    return inner_sync();
}
auto FileSink::size() const noexcept -> size_t { return file_size_; }
auto FileSink::config() const noexcept -> Config {
    std::scoped_lock lock{mutex_};
    return config_;
}
auto FileSink::set_directory(const std::filesystem::path &dir) noexcept -> bool {
    std::scoped_lock lock{mutex_};
    if (dir.empty()) {
        config_.dir = std::filesystem::current_path();
        return true;
    }
    if (std::filesystem::exists(dir) && !std::filesystem::is_directory(dir)) {
        return false;
    }
    config_.dir = dir;
    return true;
}
auto FileSink::set_file(const std::filesystem::path &file, bool change_dir) noexcept -> bool {
    std::scoped_lock lock{mutex_};
    if (std::filesystem::is_directory(file)) {
        return false;
    }
    config_.file = file;
    rotate_count_ = 0;
    if (change_dir) {
        config_.dir = file.parent_path();
    }
    rotate_file_ =
        config_.file.has_parent_path() ? config_.file : config_.dir / config_.file.filename();
    return true;
}
void FileSink::set_sync_interval(size_t count) noexcept {
    std::scoped_lock lock{mutex_};
    config_.sync_interval = count;
}
auto FileSink::set_max_file_size(size_t size) noexcept -> bool {
    std::scoped_lock lock{mutex_};
    if (size > MaxFileSize || size < MinFileSize) {
        return false;
    }
    config_.max_file_size = size;
    return true;
}

auto FileSink::get_next_file() -> std::filesystem::path {
    rotate_count_++;
    return config_.dir /
           std::string{std::format("{}_{}.log", rotate_count_, config_.file.filename().string())};
}

void FileSink::inner_close() noexcept {
    if (is_valid_) {
        inner_sync();
#if defined(_WIN32)
        ::CloseHandle(reinterpret_cast<HANDLE>(fd_));
        is_valid_ = false;
        fd_ = nullptr;
#else
        ::close(fd_);
        is_valid_ = false;
        fd_ = -1;
#endif
    }
}
auto FileSink::inner_open() -> bool {
    inner_close();
    if (!rotate_file_.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(rotate_file_.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
#if defined(_WIN32)
    HANDLE f = ::CreateFileW(rotate_file_.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (::GetFileType(f) != FILE_TYPE_DISK && !::GetFileSizeEx(f, &size)) {
        ::CloseHandle(f);
        return false;
    }
    is_valid_ = true;
    file_size_ = static_cast<uint64_t>(size.QuadPart);

#else
    const int f = ::open(rotate_file_.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644);
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
    rotate_file_ = get_next_file();
    return true;
}
auto FileSink::inner_sync() -> bool {
    if (is_valid_) {
#if defined(_WIN32)
        ::FlushFileBuffers(reinterpret_cast<HANDLE>(fd_));
#else
        ::fsync(fd_);
#endif
        write_since_sync_ = 0;
        return true;
    }
    return false;
}

auto createFileSink(const component::FileSink::Config &config)
    -> std::unique_ptr<::lig::interface::Sink> {
    auto sink = std::make_unique<component::FileSink>(config);
    return std::move(sink);
}

// console sink begin
ConsoleSink::ConsoleSink(const Config &config) {
#if defined(_WIN32)
    DWORD id = config.to_err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    HANDLE h = GetStdHandle(id);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        fd_ = h;
    }
#else
    fd_ = config.to_err ? 2 : 1;
#endif
}
ConsoleSink::~ConsoleSink() { close(); }
auto ConsoleSink::is_valid() const noexcept -> bool {
#if defined(_WIN32)
    return fd_ != nullptr;
#else
    return fd_ == 1 || fd_ == 2;
#endif
}
auto ConsoleSink::write(const void *data, size_t len) -> WriteRes {
    return ::lig::component::write_impl(fd_, data, len);
}
void ConsoleSink::close() noexcept {
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(fd_));
#else
    fd_ = -1;
#endif
}
auto createConsoleSink(const component::ConsoleSink::Config &config)
    -> std::unique_ptr<::lig::interface::Sink> {
    auto sink = std::make_unique<component::ConsoleSink>(config);
    return std::move(sink);
}
} // namespace lig::component
