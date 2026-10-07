#pragma once

#include "backend.hpp"
#include "component.hpp"
#include "interface.hpp"
#include "logger.hpp"
#include <atomic>
#include <memory>

namespace lig {

auto createFileSink(const component::FileSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink> {
    auto sink = std::make_unique<component::FileSink>(config);
    return std::move(sink);
}
auto createConsoleSink(const component::ConsoleSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink> {
    auto sink = std::make_unique<component::ConsoleSink>(config);
    return std::move(sink);
}
auto createTextFileContext(interface::Formatter::Status status)
    -> std::unique_ptr<const ::lig::interface::Formatter> {
    auto formatter = std::make_unique<component::TextFormatter>();
    formatter->status = status;
    return std::unique_ptr<const ::lig::interface::Formatter>(formatter.release());
}

struct Config;
struct EntryConfig;

struct FormatterConfig {
    virtual auto submit() -> EntryConfig & = 0;
    virtual auto start() -> std::unique_ptr<const interface::Formatter> = 0;
    virtual ~FormatterConfig() = default;
};
struct TextFormatterConfig final : FormatterConfig {
    TextFormatterConfig(EntryConfig &config) : config{config} {}
    ~TextFormatterConfig() final = default;
    auto timestamp(bool is_allow) -> TextFormatterConfig & {
        status.setTimeStamp(is_allow);
        return *this;
    };
    auto function_name(bool is_allow) -> TextFormatterConfig & {
        status.setFuncname(is_allow);
        return *this;
    }
    auto location(bool is_allow) -> TextFormatterConfig & {
        status.setLocation(is_allow);
        return *this;
    }
    auto thread_id(bool is_allow) -> TextFormatterConfig & {
        status.setThreadId(is_allow);
        return *this;
    }
    auto submit() -> EntryConfig & final { return config; }

    auto start() -> std::unique_ptr<const interface::Formatter> final {
        return createTextFileContext(status);
    }

  private:
    interface::Formatter::Status status{};
    EntryConfig &config;
};
struct SinkConfig {
    virtual auto submit() -> EntryConfig & = 0;
    virtual auto start() -> std::unique_ptr<interface::Sink> = 0;
    virtual ~SinkConfig() = default;
};
struct ConsoleSinkConfig final : SinkConfig {
    ConsoleSinkConfig(EntryConfig &config) : entry_{config} {}
    ~ConsoleSinkConfig() final = default;
    auto to_err(bool is_err) -> ConsoleSinkConfig & {
        config_.to_err = is_err;
        return *this;
    }
    auto submit() -> EntryConfig & final { return entry_; }

    auto start() -> std::unique_ptr<interface::Sink> final { return createConsoleSink(config_); }

  private:
    EntryConfig &entry_;
    component::ConsoleSink::Config config_{};
};
struct FileSinkConfig final : SinkConfig {
    FileSinkConfig(EntryConfig &config) : entry_{config} {}
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

    auto submit() -> EntryConfig & final { return entry_; }
    auto start() -> std::unique_ptr<interface::Sink> final { return createFileSink(config_); }

  private:
    EntryConfig &entry_;
    component::FileSink::Config config_{};
};

struct EntryConfig {
  public:
    EntryConfig(Config &config) : config{config} {}
    auto create_text_formatter_config() -> TextFormatterConfig & {
        formatter_ = std::make_unique<TextFormatterConfig>(*this);
        return *(static_cast<TextFormatterConfig *>(formatter_.get()));
    }
    auto create_console_sink_config() -> ConsoleSinkConfig & {
        auto console_sink = std::make_unique<ConsoleSinkConfig>(*this);
        ConsoleSinkConfig &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }
    auto create_file_sink_config() -> FileSinkConfig & {
        auto console_sink = std::make_unique<FileSinkConfig>(*this);
        FileSinkConfig &res = *(console_sink.get());
        sinks_.push_back(std::move(console_sink));
        return res;
    }

    auto kind(backend::TransportKind kind) -> EntryConfig & {
        kind_ = kind;
        return *this;
    }
    auto max_cache_size(size_t max_cache_size) -> EntryConfig & {
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
        logger::getBackend<backend::InnerBackend>().add_entity(kind_, formatter, sinks,
                                                               max_cache_size_);
    }

  private:
    std::unique_ptr<FormatterConfig> formatter_;
    backend::TransportKind kind_{};
    std::vector<std::unique_ptr<SinkConfig>> sinks_{};
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
    auto create_entry_config() -> EntryConfig & {
        auto entry = std::make_unique<EntryConfig>(*this);
        EntryConfig &res = *(entry.get());
        entrys_.push_back(std::move(entry));
        return res;
    }

    void start() {
        for (auto &entry : entrys_) {
            entry->start();
        }
        logger::getBackend<backend::InnerBackend>().start();
    }

  private:
    std::vector<std::unique_ptr<EntryConfig>> entrys_{};
    std::atomic_bool is_start_{false};
};

inline auto config() -> Config { return Config{}; }

using Trace = logger::Trace<backend::InnerBackend>;
using Debug = logger::Debug<backend::InnerBackend>;
using Info = logger::Info<backend::InnerBackend>;
using Warn = logger::Warn<backend::InnerBackend>;
using Error = logger::Error<backend::InnerBackend>;
using Fatal = logger::Fatal<backend::InnerBackend>;

}; // namespace lig
