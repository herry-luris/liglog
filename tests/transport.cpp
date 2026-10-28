// Tests for component/transport.hpp.
//
// Coverage:
//   - internal::Data: move ctor, move assign
//   - internal::DataBuffer: push, flush, setBatchSize range
//   - internal::SinkWrapper: ownership takeover, single-failure recovery,
//                            persistent-failure buffering
//   - SyncTransport: kind, from/flush with a mock sink, set_max_cache_size
//   - AsyncTransport: kind, from/flush with a mock sink, set_max_cache_size

#include "component/formatter.hpp"
#include "component/transport.hpp"
#include "test_help.hpp"

#include <string>

namespace {

struct MockSink final : lig::interface::Sink {
    std::vector<std::byte> written{};
    bool fail_next{false};
    int open_calls{0};

    auto write(const void *data, size_t len) -> WriteRes final {
        if (fail_next) {
            fail_next = false;
            WriteRes r{};
            r.err = std::make_error_code(std::errc::io_error);
            return r;
        }
        auto *p = static_cast<const std::byte *>(data);
        written.insert(written.end(), p, p + len);
        WriteRes r{};
        r.len = len;
        return r;
    }
    auto is_valid() const noexcept -> bool final { return true; }
    auto open() -> bool final { ++open_calls; return true; }
};

struct AlwaysFailSink final : lig::interface::Sink {
    auto write(const void *, size_t) -> WriteRes final {
        WriteRes r{};
        r.err = std::make_error_code(std::errc::io_error);
        return r;
    }
    auto is_valid() const noexcept -> bool final { return false; }
    auto open() -> bool final { return false; }
};

lig::logger::Task make_task(const char *body) {
    return [body](const lig::interface::Formatter *f,
                  std::vector<std::byte> &buf) {
        lig::LogEntry e{};
        e.head.level = lig::Level::Info;
        e.body = body;
        f->format(e, buf);
    };
}

std::string bytes_to_string(const std::vector<std::byte> &buf) {
    return std::string(reinterpret_cast<const char *>(buf.data()), buf.size());
}

} // namespace

void test_data_move_ctor() {
    auto *raw = new std::byte[10]{std::byte{0xAB}};
    lig::component::internal::Data d1{raw, 10};
    ASSERT(d1.data() == raw);
    ASSERT(d1.size() == 10);

    lig::component::internal::Data d2{std::move(d1)};
    ASSERT(d2.data() == raw);
    ASSERT(d2.size() == 10);
    ASSERT(d1.data() == nullptr);
    ASSERT(d1.size() == 0);
}

void test_data_move_assign() {
    auto *raw = new std::byte[10]{std::byte{0xAB}};
    lig::component::internal::Data d1{raw, 10};
    lig::component::internal::Data d2{};
    d2 = std::move(d1);
    ASSERT(d2.data() == raw);
    ASSERT(d2.size() == 10);
    ASSERT(d1.data() == nullptr);
}

void test_data_buffer_push_and_flush() {
    lig::component::internal::DataBuffer db{};
    ASSERT(db.setBatchSize(1024));
    ASSERT(db.batchSize() == 1024);

    std::byte src[1500];
    for (size_t i = 0; i < 1500; ++i) src[i] = static_cast<std::byte>(i & 0xFF);

    auto datas = db.push(src, 1500);
    ASSERT(datas.size() == 1);
    ASSERT(datas[0].size() == 1024);
    ASSERT(db.len() == 476);

    auto rest = db.flush();
    ASSERT(rest.has_value());
    ASSERT(rest->size() == 476);
    ASSERT(db.len() == 0);
}

void test_data_buffer_set_batch_size_range() {
    lig::component::internal::DataBuffer db{};
    ASSERT(db.batchSize() == 2048);
    ASSERT(!db.setBatchSize(0));
    ASSERT(!db.setBatchSize(16 * 1024));
    ASSERT(db.setBatchSize(4096));
    ASSERT(db.batchSize() == 4096);
}

void test_sink_wrapper_takes_ownership_and_writes() {
    auto mock = std::make_unique<MockSink>();
    auto *mock_ptr = mock.get();
    std::unique_ptr<lig::interface::Sink> base = std::move(mock);

    lig::component::internal::SinkWrapper wrapper{base};
    ASSERT(base == nullptr);

    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}};
    wrapper.from(data);
    ASSERT(mock_ptr->written.size() == 3);

    wrapper.open();
    ASSERT(mock_ptr->open_calls == 1);
}

void test_sink_wrapper_recovers_on_first_failure() {
    auto mock = std::make_unique<MockSink>();
    auto *mock_ptr = mock.get();
    mock_ptr->fail_next = true;
    std::unique_ptr<lig::interface::Sink> base = std::move(mock);

    lig::component::internal::SinkWrapper wrapper{base};
    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}};
    wrapper.from(data);

    ASSERT(mock_ptr->open_calls == 1);
    ASSERT(mock_ptr->written.size() == 3);
}

void test_sink_wrapper_stores_on_persistent_failure() {
    // No crash / no leak expected even when the sink never succeeds.
    auto mock = std::make_unique<AlwaysFailSink>();
    std::unique_ptr<lig::interface::Sink> base = std::move(mock);

    lig::component::internal::SinkWrapper wrapper{base};
    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}};
    wrapper.from(data);
}

void test_sync_transport_kind() {
    lig::component::SyncTransport t{};
    ASSERT(t.kind() == lig::interface::ITransport::Kind::Sync);
}

void test_sync_transport_from_and_flush() {
    auto mock = std::make_unique<MockSink>();
    auto *mock_ptr = mock.get();
    std::unique_ptr<lig::interface::Sink> base = std::move(mock);

    lig::component::SyncTransport t{};
    auto formatter = lig::component::createTextFormatter({});
    t.set_formatter(formatter);
    ASSERT(formatter == nullptr);

    t.add_sink(base);
    ASSERT(base == nullptr);

    auto task = make_task("hello sync");
    t.from(task);
    t.flush();

    ASSERT(mock_ptr->written.size() > 0);
    ASSERT(bytes_to_string(mock_ptr->written).find("hello sync") != std::string::npos);
}

void test_sync_transport_set_max_cache_size_range() {
    lig::component::SyncTransport t{};
    ASSERT(!t.set_max_cache_size(1024));
    ASSERT(!t.set_max_cache_size(128 * 1024));
    ASSERT(t.set_max_cache_size(32 * 1024));
}

void test_async_transport_kind() {
    lig::component::AsyncTransport t{};
    ASSERT(t.kind() == lig::interface::ITransport::Kind::Async);
}

void test_async_transport_from_and_flush() {
    auto mock = std::make_unique<MockSink>();
    auto *mock_ptr = mock.get();
    std::unique_ptr<lig::interface::Sink> base = std::move(mock);

    lig::component::AsyncTransport t{};
    auto formatter = lig::component::createTextFormatter({});
    t.set_formatter(formatter);
    t.add_sink(base);
    t.start();

    auto task = make_task("hello async");
    t.from(task);
    t.flush();

    ASSERT(mock_ptr->written.size() > 0);
    ASSERT(bytes_to_string(mock_ptr->written).find("hello async") != std::string::npos);

    t.stop();
}

void test_async_transport_set_max_cache_size_range() {
    lig::component::AsyncTransport t{};
    ASSERT(!t.set_max_cache_size(1024));
    ASSERT(!t.set_max_cache_size(128 * 1024));
    ASSERT(t.set_max_cache_size(32 * 1024));
}

int main() {
    TEST(test_data_move_ctor);
    TEST(test_data_move_assign);
    TEST(test_data_buffer_push_and_flush);
    TEST(test_data_buffer_set_batch_size_range);
    TEST(test_sink_wrapper_takes_ownership_and_writes);
    TEST(test_sink_wrapper_recovers_on_first_failure);
    TEST(test_sink_wrapper_stores_on_persistent_failure);
    TEST(test_sync_transport_kind);
    TEST(test_sync_transport_from_and_flush);
    TEST(test_sync_transport_set_max_cache_size_range);
    TEST(test_async_transport_kind);
    TEST(test_async_transport_from_and_flush);
    TEST(test_async_transport_set_max_cache_size_range);
    return report();
}
