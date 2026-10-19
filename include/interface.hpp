#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lig {
enum struct Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Fatal };
auto inline get_level_string(Level level) -> std::string_view {
    static constexpr std::array<const char *, 6> levelStr{"Trace", "Debug", "Info",
                                                          "Warn",  "Error", "Fatal"};
    return levelStr[static_cast<uint8_t>(level)];
}

class Status {
  public:
    static constexpr uint32_t TimeStamp = 1 << 0;
    static constexpr uint32_t ThreadId = 1 << 1;
    static constexpr uint32_t Funcname = 1 << 2;
    static constexpr uint32_t Location = 1 << 3;

    Status() = default;
    Status(uint8_t mask) : mask_{mask} {}
    auto hasTimeStamp() const noexcept -> bool { return mask_ & TimeStamp; }
    auto hasThreadId() const noexcept -> bool { return mask_ & ThreadId; }
    auto hasFuncname() const noexcept -> bool { return mask_ & Funcname; }
    auto hasLocation() const noexcept -> bool { return mask_ & Location; }

    auto setTimeStamp(bool enable) noexcept -> Status & {
        mask_ = enable ? (mask_ | TimeStamp) : (mask_ & ~TimeStamp);
        return *this;
    }
    auto setThreadId(bool enable) noexcept -> Status & {
        mask_ = enable ? (mask_ | ThreadId) : (mask_ & ~ThreadId);
        return *this;
    }
    auto setFuncname(bool enable) noexcept -> Status & {
        mask_ = enable ? (mask_ | Funcname) : (mask_ & ~Funcname);
        return *this;
    }
    auto setLocation(bool enable) noexcept -> Status & {
        mask_ = enable ? (mask_ | Location) : (mask_ & ~Location);
        return *this;
    }

  private:
    uint8_t mask_{TimeStamp | ThreadId | Funcname | Location};
};

struct LogHead {
    Level level{};
    uint32_t line{};
    uint64_t thread_id{};
    const char *filename{};
    const char *funcname{};
    std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> now{};
};

struct LogEntry {
    LogHead head{};
    std::string body{};
};

namespace logger {
struct Task;
}
} // namespace lig

namespace lig::interface {

// sink
class Sink {
  public:
    struct WriteRes {
        size_t len{};
        std::error_code err{};
        operator bool() const { return err.value() == 0; }
    };

    virtual ~Sink() = default;
    virtual auto write(const void *data, size_t len) -> WriteRes = 0;
    virtual auto is_valid() const noexcept -> bool = 0;
    virtual void close() noexcept {};
    virtual auto open() -> bool { return true; }
};

struct ISinkConfig {
    virtual auto start() -> std::unique_ptr<interface::Sink> = 0;
    virtual ~ISinkConfig() = default;
};
template <typename Transport> struct SinkConfig : ISinkConfig {
    SinkConfig(Transport &transport) : transport_{transport} {}
    virtual ~SinkConfig() = default;
    auto submit() -> Transport & { return transport_; }

  protected:
    Transport &transport_;
};

// formatter
class Formatter {
  public:
    virtual void on_open(std::vector<std::byte> &buf) const {};
    virtual void format(const LogEntry &entry, std::vector<std::byte> &buf) const = 0;
    virtual void on_close(std::vector<std::byte> &buf) const {};
    virtual ~Formatter() = default;

    Status status{};
};

struct IFormatterConfig {
    virtual auto start() -> std::unique_ptr<const interface::Formatter> = 0;
    virtual ~IFormatterConfig() = default;
};
template <typename Derived, typename Transport> struct FormatterConfig : IFormatterConfig {
    virtual ~FormatterConfig() = default;
    FormatterConfig(Transport &transport) : transport_{transport} {}

    auto submit() -> Transport & { return transport_; }
    auto timestamp(bool is_allow) -> Derived & {
        status_.setTimeStamp(is_allow);
        return *static_cast<Derived *>(this);
    };
    auto function_name(bool is_allow) -> Derived & {
        status_.setFuncname(is_allow);
        return *static_cast<Derived *>(this);
    }
    auto location(bool is_allow) -> Derived & {
        status_.setLocation(is_allow);
        return *static_cast<Derived *>(this);
    }
    auto thread_id(bool is_allow) -> Derived & {
        status_.setThreadId(is_allow);
        return *static_cast<Derived *>(this);
    }

  protected:
    Transport &transport_;
    Status status_{};
};

// tranpsort
class ITransport {
  public:
    enum struct Kind : uint8_t { Async, Sync };
    virtual ~ITransport() = default;
    virtual void stop() {};
    virtual void start() {};
    virtual void flush() = 0;
    virtual void from(const ::lig::logger::Task&) = 0;
    virtual void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) = 0;
    virtual void add_sink(std::unique_ptr<::lig::interface::Sink> &sink) = 0;
    virtual auto kind() -> Kind = 0;
    virtual auto set_max_cache_size(size_t size) noexcept -> bool = 0;
};

template <typename Derived> struct IBackend {
    using Base = IBackend<Derived>;
    static constexpr void verify() {
        static_assert(std::is_same_v<std::invoke_result_t<decltype(&Derived::submit_impl),
                                                          Derived &, ::lig::logger::Task &&>,
                                     void>);
    }
    void submit(::lig::logger::Task &&task) {
        static_cast<Derived *>(this)->submit_impl(std::move(task));
    };
};

} // namespace lig::interface
