#pragma once

#include "component/formatter.hpp"
#include "component/sink.hpp"
#include "component/transport.hpp"
#include "interface.hpp"
#include "logger.hpp"
#include <atomic>
#include <memory>

namespace lig {

class InnerBackend : public ::lig::interface::IBackend<InnerBackend> {
  public:
    friend struct ::lig::interface::IBackend<InnerBackend>;

    void start() {
        if (is_start.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        for (auto &t : transports_) {
            t->start();
        }
    }

    void flush() {
        for (auto &t : transports_) {
            t->flush();
        }
    }

    void stop() {
        for (auto &t : transports_) {
            t->stop();
        }
    }
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
        if (transports_.size() == 1) {
            auto &t = transports_[0];
            if (t->kind() == ::lig::interface::ITransport::Kind::Sync) {
                t->from(task);
            } else {
                t->from(std::move(task));
            }
        } else {
            for (auto &t : transports_) {
                t->from(task);
            }
        }
    }
};

class Config;

struct TransportConfig {
  public:
    TransportConfig(Config &config) : config{config} {}
    auto text_formatter() -> component::TextFormatterConfig<TransportConfig> & {
        formatter_ = std::make_unique<component::TextFormatterConfig<TransportConfig>>(*this);
        return *(static_cast<component::TextFormatterConfig<TransportConfig> *>(formatter_.get()));
    }
    auto term_formatter() -> component::TermFormatterConfig<TransportConfig> & {
        formatter_ = std::make_unique<component::TermFormatterConfig<TransportConfig>>(*this);
        return *(static_cast<component::TermFormatterConfig<TransportConfig> *>(formatter_.get()));
    }
    auto console_sink() -> component::ConsoleSinkConfig<TransportConfig> & {
        auto console_sink = std::make_unique<component::ConsoleSinkConfig<TransportConfig>>(*this);
        component::ConsoleSinkConfig<TransportConfig> &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }
    auto file_sink() -> component::FileSinkConfig<TransportConfig> & {
        auto console_sink = std::make_unique<component::FileSinkConfig<TransportConfig>>(*this);
        component::FileSinkConfig<TransportConfig> &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }
    auto sink(std::unique_ptr<::lig::interface::ISinkConfig> sink) -> TransportConfig & {
        sinks_.push_back(std::move(sink));
        return *this;
    }
    auto formatter(std::unique_ptr<::lig::interface::IFormatterConfig> &formatter)
        -> TransportConfig & {
        formatter_.swap(formatter);
        return *this;
    }

    auto kind(interface::ITransport::Kind kind) -> TransportConfig & {
        kind_ = kind;
        return *this;
    }
    auto max_cache_size(size_t max_cache_size) -> TransportConfig & {
        max_cache_size_ = max_cache_size;
        return *this;
    }
    auto submit() -> Config & { return config; }

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

struct Config {
    Config() = default;
    ~Config() = default;

    auto level(Level level) -> Config & {
        logger::g_level.store(level, std::memory_order_relaxed);
        return *this;
    }
    auto transport() -> TransportConfig & {
        auto transport = std::make_unique<TransportConfig>(*this);
        TransportConfig &res = *(transport.get());
        transports_.push_back(std::move(transport));
        return res;
    }

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

inline auto config() -> Config { return Config{}; }

using Trace = logger::Trace<InnerBackend>;
using Debug = logger::Debug<InnerBackend>;
using Info = logger::Info<InnerBackend>;
using Warn = logger::Warn<InnerBackend>;
using Error = logger::Error<InnerBackend>;
using Fatal = logger::Fatal<InnerBackend>;

}; // namespace lig
