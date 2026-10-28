#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lig {

/// Log severity. Ordered from least to most severe; filtering uses the
/// numeric value, so `Trace < Debug < Info < Warn < Error < Fatal`.
enum struct Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Fatal };

/// Human-readable name for a Level. Returns a static string; safe to hold.
auto inline get_level_string(Level level) -> std::string_view {
    static constexpr std::array<const char *, 6> levelStr{"Trace", "Debug", "Info",
                                                          "Warn",  "Error", "Fatal"};
    return levelStr[static_cast<uint8_t>(level)];
}

/// Bitmask telling the Formatter which header fields to emit.
///
/// Default-constructed Status enables all four fields. Each setter returns
/// `*this` for chaining, e.g.
///     status.setTimeStamp(false).setThreadId(false);
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

/// Metadata captured at the log call site. `filename` and `funcname` are
/// non-owning pointers into static storage (from `std::source_location`);
/// they never need to be freed.
struct LogHead {
    Level level{};
    uint32_t line{};
    size_t thread_id{};
    const char *filename{};
    const char *funcname{};
    std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> now{};
};

/// One fully-formed log record passed to a Formatter.
struct LogEntry {
    LogHead head{};
    std::string body{};
};

namespace logger {
struct Task;
}
} // namespace lig

namespace lig::interface {

/// Abstract output target. A Sink is owned by a Transport; user code never
/// touches it after handing it over.
///
/// Implementations must be safe to call from the transport thread. `write`
/// is called in batches, so it should handle arbitrary lengths.
class Sink {
  public:
    /// Result of a write. `len` is the number of bytes actually written
    /// (may be less than requested on partial failure). `err` is set on
    /// failure; `operator bool` returns true only when `err` is empty.
    struct WriteRes {
        size_t len{};
        std::error_code err{};
        operator bool() const { return err.value() == 0; }
    };

    virtual ~Sink() = default;

    /// Write `len` bytes from `data`. Return a WriteRes describing how many
    /// bytes were written and whether an error occurred.
    virtual auto write(const void *data, size_t len) -> WriteRes = 0;

    /// True if the sink is currently usable.
    virtual auto is_valid() const noexcept -> bool = 0;

    /// Release any OS resources. Called by the transport on shutdown.
    virtual void close() noexcept {};

    /// (Re)open the sink. Called by the transport at startup and after a
    /// `write` failure. Return true on success.
    virtual auto open() -> bool { return true; }
};

/// Factory interface for a Sink. `start()` is called once at Config::start.
struct ISinkConfig {
    virtual auto start() -> std::unique_ptr<interface::Sink> = 0;
    virtual ~ISinkConfig() = default;
};

/// CRTP base for Sink configs that need to return to the parent
/// TransportConfig via `submit()`.
template <typename Transport> struct SinkConfig : ISinkConfig {
    SinkConfig(Transport &transport) : transport_{transport} {}
    ~SinkConfig() override = default;
    auto submit() -> Transport & { return transport_; }

  protected:
    Transport &transport_;
};

/// Abstract formatter. `format` is the only required method; `on_open` /
/// `on_close` are optional hooks the transport may call.
///
/// `status` is the bitmask read by `format` to decide which header fields
/// to emit. A formatter may also choose to ignore it.
class Formatter {
  public:
    virtual void on_open(std::vector<std::byte> &buf) const {};
    virtual void format(const LogEntry &entry, std::vector<std::byte> &buf) const = 0;
    virtual void on_close(std::vector<std::byte> &buf) const {};
    virtual ~Formatter() = default;

    Status status{};
};

/// Factory interface for a Formatter. `start()` is called once at
/// Config::start.
struct IFormatterConfig {
    virtual auto start() -> std::unique_ptr<const interface::Formatter> = 0;
    virtual ~IFormatterConfig() = default;
};

/// CRTP base for Formatter configs. Provides fluent setters for the common
/// Status bits and a `submit()` that returns to the parent TransportConfig.
template <typename Derived, typename Transport> struct FormatterConfig : IFormatterConfig {
    ~FormatterConfig() override = default;
    FormatterConfig(Transport &transport) : transport_{transport} {}

    auto submit() -> Transport & { return transport_; }

    /// Enable/disable the timestamp in the header.
    auto timestamp(bool is_allow) -> Derived & {
        status_.setTimeStamp(is_allow);
        return *static_cast<Derived *>(this);
    };
    /// Enable/disable the function name in the header.
    auto function_name(bool is_allow) -> Derived & {
        status_.setFuncname(is_allow);
        return *static_cast<Derived *>(this);
    }
    /// Enable/disable the file:line location in the header.
    auto location(bool is_allow) -> Derived & {
        status_.setLocation(is_allow);
        return *static_cast<Derived *>(this);
    }
    /// Enable/disable the thread id in the header.
    auto thread_id(bool is_allow) -> Derived & {
        status_.setThreadId(is_allow);
        return *static_cast<Derived *>(this);
    }

  protected:
    Transport &transport_;
    Status status_{};
};

/// Abstract transport. A transport owns one Formatter and one or more Sinks
/// and turns `Task` objects (from the front-end) into bytes on the sinks.
///
/// Sync vs Async is chosen by the concrete subclass; `kind()` reports which.
class ITransport {
  public:
    enum struct Kind : uint8_t { Async, Sync };
    virtual ~ITransport() = default;
    virtual void stop() {};
    virtual void start() {};
    virtual void flush() = 0;

    /// Consume one Task. Called from the front-end (possibly many threads).
    /// Sync transports may buffer; Async transports push into a queue.
    virtual void from(const ::lig::logger::Task &) = 0;
    virtual void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) = 0;
    virtual void add_sink(std::unique_ptr<::lig::interface::Sink> &sink) = 0;
    virtual auto kind() -> Kind = 0;
    virtual auto set_max_cache_size(size_t size) noexcept -> bool = 0;
};

/// CRTP base for the singleton backend. Derived must provide
/// `void submit_impl(::lig::logger::Task &&)`.
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
