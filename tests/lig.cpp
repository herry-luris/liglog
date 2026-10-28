// Tests for lig.hpp.
//
// Coverage:
//   - Config: fluent chain returns the same Config reference, level() writes
//             the global g_level
//   - InnerBackend: add_entity before start, reject add_entity after start,
//                   start/flush/stop for Sync and Async transports

#include "lig.hpp"
#include "test_help.hpp"

void test_config_fluent_chain_returns_config() {
    lig::Config cfg = lig::config();
    auto &result = cfg.level(lig::Level::Info)
                       .transport()
                       .text_formatter()
                       .timestamp(false)
                       .submit()
                       .console_sink()
                       .to_err(true)
                       .submit()
                       .kind(lig::interface::ITransport::Kind::Sync)
                       .max_cache_size(16ul * 1024)
                       .submit();
    ASSERT(&result == &cfg);
}

void test_config_level_sets_g_level() {
    lig::Config cfg = lig::config();
    cfg.level(lig::Level::Error);
    ASSERT(lig::logger::g_level.load(std::memory_order_relaxed) == lig::Level::Error);
    cfg.level(lig::Level::Trace);
}

void test_inner_backend_add_entity_before_start() {
    lig::InnerBackend be{};
    auto formatter = lig::component::createTextFormatter({});
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks{};
    sinks.push_back(lig::component::createConsoleSink({.to_err = true}));

    ASSERT(be.add_entity(lig::interface::ITransport::Kind::Sync, formatter, sinks, 16ul * 1024));
}

void test_inner_backend_add_entity_after_start_rejected() {
    lig::InnerBackend be{};
    auto formatter = lig::component::createTextFormatter({});
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks{};
    be.add_entity(lig::interface::ITransport::Kind::Sync, formatter, sinks, 16ul * 1024);

    be.start();

    auto formatter2 = lig::component::createTextFormatter({});
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks2{};
    ASSERT(!be.add_entity(lig::interface::ITransport::Kind::Sync, formatter2, sinks2, 16ul * 1024));

    be.stop();
}

void test_inner_backend_start_stop_flush() {
    lig::InnerBackend be{};
    auto formatter = lig::component::createTextFormatter({});
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks{};
    sinks.push_back(lig::component::createConsoleSink({.to_err = true}));
    be.add_entity(lig::interface::ITransport::Kind::Sync, formatter, sinks, 16ul * 1024);

    be.start();
    be.flush();
    be.stop();
}

void test_inner_backend_async_start_stop() {
    lig::InnerBackend be{};
    auto formatter = lig::component::createTextFormatter({});
    std::vector<std::unique_ptr<lig::interface::Sink>> sinks{};
    sinks.push_back(lig::component::createConsoleSink({.to_err = true}));
    be.add_entity(lig::interface::ITransport::Kind::Async, formatter, sinks, 16ul * 1024);

    be.start();
    be.flush();
    be.stop();
}

int main() {
    TEST(test_config_fluent_chain_returns_config);
    TEST(test_config_level_sets_g_level);
    TEST(test_inner_backend_add_entity_before_start);
    TEST(test_inner_backend_add_entity_after_start_rejected);
    TEST(test_inner_backend_start_stop_flush);
    TEST(test_inner_backend_async_start_stop);
    return report();
}
