#pragma once

#include "component/formatter.hpp"
#include "component/sink.hpp"
#include "component/transport.hpp"
#include "interface.hpp"
#include "logger.hpp"
#include <atomic>
#include <memory>

namespace lig {

/// Owns the set of transports created from a Config and dispatches Tasks to
/// them. Exposed because `logger::getBackend<InnerBackend>()` needs the type;
/// user code normally only touches it through `Config`.
class InnerBackend : public ::lig::interface::IBackend<InnerBackend> {
  public:
    friend struct ::lig::interface::IBackend<InnerBackend>;

    /// Start every transport. Idempotent; returns immediately on second call.
    void start() {
        if (is_start.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        for (auto &t : transports_) {
            t->start();
        }
    }

    /// Flush every transport. For Async this blocks until the worker has
    /// drained the queue; for Sync it flushes the current thread's buffer.
    void flush() {
        for (auto &t : transports_) {
            t->flush();
        }
    }

    /// Stop every transport. Must be called before destruction if you want
    /// pending async tasks to be drained; `~InnerBackend` calls it again.
    void stop() {
        for (auto &t : transports_) {
            t->stop();
        }
    }

    /// Attach a (formatter, sinks) pair as a new transport.
    ///
    /// @param kind             Sync or Async.
    /// @param formatter        Ownership is moved in; the unique_ptr is reset.
    /// @param sinks            Ownership of each sink is moved in.
    /// @param max_cache_sizes  Per-transport byte cache size; 0 uses default.
    /// @return false if start() has already been called; add_entity must run
    ///         before start().
    auto add_entity(::lig::interface::ITransport::Kind kind,
                    std::unique_ptr<const ::lig::interface::Formatter> &formatter,
                    std::vector<std::unique_ptr<::lig::interface::Sink>> &sinks,
                    size_t max_cache_sizes = 0) -> bool {
        if (is_start.load(std::memory_order_acquire)) {
            return false;
        }
        std::unique_ptr<::lig::interface::ITransport> t{};
        if (::lig::interface::ITransport::Kind::Async == kind) {
            t = std::make_unique<::lig::component::AsyncTransport>();
        } else {
            t = std::make_unique<::lig::component::SyncTransport>();
        }
        t->set_formatter(formatter);
        t->set_max_cache_size(max_cache_sizes);
        for (auto &sink : sinks) {
            t->add_sink(sink);
        }
        transports_.push_back(std::move(t));
        return true;
    }

    ~InnerBackend() { stop(); }

  private:
    std::vector<std::unique_ptr<::lig::interface::ITransport>> transports_{};
    std::atomic_bool is_start{false};

    void submit_impl(::lig::logger::Task &&task) {
        for (auto &t : transports_) {
            t->from(task);
        }
    }
};

struct Config;

/// Fluent builder for one transport (formatter + sinks + kind + cache size).
/// Obtain via `Config::transport()`; finish with `submit()` to return to the
/// parent Config. Every setter returns a reference to the appropriate
/// sub-builder, so chains look like:
///
///     cfg.transport()
///         .text_formatter().timestamp(false).submit()
///         .console_sink().to_err(true).submit()
///         .kind(ITransport::Kind::Async)
///         .submit();
class TransportConfig {
  public:
    TransportConfig(Config &config) : config{config} {}

    /// Replace the current formatter with a TextFormatter builder.
    auto text_formatter() -> component::TextFormatterConfig<TransportConfig> & {
        formatter_ = std::make_unique<component::TextFormatterConfig<TransportConfig>>(*this);
        return *(static_cast<component::TextFormatterConfig<TransportConfig> *>(formatter_.get()));
    }
    /// Replace the current formatter with a TermFormatter builder.
    auto term_formatter() -> component::TermFormatterConfig<TransportConfig> & {
        formatter_ = std::make_unique<component::TermFormatterConfig<TransportConfig>>(*this);
        return *(static_cast<component::TermFormatterConfig<TransportConfig> *>(formatter_.get()));
    }
    /// Append a ConsoleSink and return its builder.
    auto console_sink() -> component::ConsoleSinkConfig<TransportConfig> & {
        auto console_sink = std::make_unique<component::ConsoleSinkConfig<TransportConfig>>(*this);
        component::ConsoleSinkConfig<TransportConfig> &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }
    /// Append a FileSink and return its builder.
    auto file_sink() -> component::FileSinkConfig<TransportConfig> & {
        auto console_sink = std::make_unique<component::FileSinkConfig<TransportConfig>>(*this);
        component::FileSinkConfig<TransportConfig> &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }
    /// Attach a user-provided sink config.
    auto sink(std::unique_ptr<::lig::interface::ISinkConfig> sink) -> TransportConfig & {
        sinks_.push_back(std::move(sink));
        return *this;
    }
    /// Attach a user-provided formatter config.
    auto formatter(std::unique_ptr<::lig::interface::IFormatterConfig> &formatter)
        -> TransportConfig & {
        formatter_.swap(formatter);
        return *this;
    }

    /// Sync or Async. Defaults to Sync. Set before Config::start().
    auto kind(interface::ITransport::Kind kind) -> TransportConfig & {
        kind_ = kind;
        return *this;
    }
    /// Maximum byte cache size for this transport. 0 uses the transport's
    /// built-in default (see SyncTransport / AsyncTransport).
    auto max_cache_size(size_t max_cache_size) -> TransportConfig & {
        max_cache_size_ = max_cache_size;
        return *this;
    }

    /// Return to the parent Config.
    auto submit() -> Config & { return config; }

    /// Materialize the formatter and sinks and hand them to the backend.
    /// Called by Config::start().
    void start() {
        auto formatter = formatter_->start();
        std::vector<std::unique_ptr<interface::Sink>> sinks{};
        sinks.reserve(sinks_.size());
        for (auto &sink : sinks_) {
            sinks.emplace_back(sink->start());
        }
        logger::getBackend<InnerBackend>().add_entity(kind_, formatter, sinks, max_cache_size_);
    }

  private:
    std::unique_ptr<::lig::interface::IFormatterConfig> formatter_;
    interface::ITransport::Kind kind_{};
    std::vector<std::unique_ptr<::lig::interface::ISinkConfig>> sinks_{};
    size_t max_cache_size_{};
    Config &config;
};

/// Top-level configuration. Build one with `lig::config()`, add one or more
/// transports, then call `start()` once. After `start()` the backend is
/// frozen and logging begins.
struct Config {
    Config() = default;
    ~Config() = default;

    /// Set the global minimum log level. Messages below this level are
    /// dropped at the front-end before a Task is even constructed.
    auto level(Level level) -> Config & {
        logger::g_level.store(level, std::memory_order_relaxed);
        return *this;
    }

    /// Start building a new transport. Each call appends a new transport.
    auto transport() -> TransportConfig & {
        auto transport = std::make_unique<TransportConfig>(*this);
        TransportConfig &res = *(transport.get());
        transports_.push_back(std::move(transport));
        return res;
    }

    /// Materialize every transport and start the backend. After this call,
    /// logging is active. Safe to call only once.
    void start() {
        for (auto &entry : transports_) {
            entry->start();
        }
        logger::getBackend<InnerBackend>().start();
    }

  private:
    std::vector<std::unique_ptr<TransportConfig>> transports_{};
    std::atomic_bool is_start_{false};
};

/// Entry point. Returns a fresh Config.
inline auto config() -> Config { return Config{}; }

// Convenience aliases bound to the default backend.
using Trace = logger::Trace<InnerBackend>;
using Debug = logger::Debug<InnerBackend>;
using Info = logger::Info<InnerBackend>;
using Warn = logger::Warn<InnerBackend>;
using Error = logger::Error<InnerBackend>;
using Fatal = logger::Fatal<InnerBackend>;

}; // namespace lig
