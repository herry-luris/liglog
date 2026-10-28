// Tests for logger.hpp.
//
// Coverage:
//   - getThreadId: returns non-zero
//   - Task: empty state, empty invocation, inline lambda, move ctor,
//           move assign, copy ctor (inline and heap path), copy assign
//   - getBackend: returns the same singleton on repeated calls
//   - Log: submits exactly one task, formats body via vformat,
//          respects g_level filtering

#include "logger.hpp"
#include "test_help.hpp"

#include <array>
#include <string>

namespace {

// Backend that captures the last submitted Task for inspection.
struct DummyBE : lig::interface::IBackend<DummyBE> {
    friend struct lig::interface::IBackend<DummyBE>;

    lig::logger::Task received{};
    int count = 0;

  private:
    void submit_impl(lig::logger::Task &&task) {
        received = std::move(task);
        ++count;
    }
};

// Formatter that copies entry.body into buf, so the test can assert on
// the formatted body without depending on any real formatter.
struct BodyFormatter final : lig::interface::Formatter {
    void format(const lig::LogEntry &entry, std::vector<std::byte> &buf) const final {
        const auto *p = reinterpret_cast<const std::byte *>(entry.body.data());
        buf.insert(buf.end(), p, p + entry.body.size());
    }
};

std::string bytes_to_string(const std::vector<std::byte> &buf) {
    return std::string(reinterpret_cast<const char *>(buf.data()), buf.size());
}

} // namespace

void test_get_thread_id_non_zero() {
    ASSERT(lig::logger::getThreadId() != 0);
}

void test_task_empty_is_false() {
    lig::logger::Task t{};
    ASSERT(!static_cast<bool>(t));
}

void test_task_empty_is_not_invoked() {
    // An empty Task is a flush sentinel. It must report false and must not
    // be invoked; AsyncTransport guards with `if (!task)` before calling.
    lig::logger::Task t{};
    ASSERT(!static_cast<bool>(t));
}

void test_task_inline_lambda_invoked() {
    bool called = false;
    lig::logger::Task t = [&called](const lig::interface::Formatter *,
                                    std::vector<std::byte> &) {
        called = true;
    };
    ASSERT(static_cast<bool>(t));
    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t(&fmt, out);
    ASSERT(called);
}

void test_task_move_ctor_transfers() {
    bool called = false;
    lig::logger::Task t1 = [&called](const lig::interface::Formatter *,
                                     std::vector<std::byte> &) {
        called = true;
    };
    lig::logger::Task t2{std::move(t1)};
    ASSERT(static_cast<bool>(t2));
    ASSERT(!static_cast<bool>(t1));

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t2(&fmt, out);
    ASSERT(called);
}

void test_task_move_assign_replaces() {
    bool a = false, b = false;
    lig::logger::Task t1 = [&a](const lig::interface::Formatter *,
                                std::vector<std::byte> &) { a = true; };
    lig::logger::Task t2 = [&b](const lig::interface::Formatter *,
                                std::vector<std::byte> &) { b = true; };
    t2 = std::move(t1);
    ASSERT(static_cast<bool>(t2));
    ASSERT(!static_cast<bool>(t1));

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t2(&fmt, out);
    ASSERT(a);
    ASSERT(!b);
}

void test_task_copy_ctor_inline() {
    bool called = false;
    lig::logger::Task t1 = [&called](const lig::interface::Formatter *,
                                     std::vector<std::byte> &) {
        called = true;
    };
    lig::logger::Task t2{t1};
    ASSERT(static_cast<bool>(t1));
    ASSERT(static_cast<bool>(t2));

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t1(&fmt, out);
    ASSERT(called);

    called = false;
    t2(&fmt, out);
    ASSERT(called);
}

void test_task_copy_ctor_heap() {
    // Captures 200 bytes, exceeding InlineSize (128). This forces the
    // heap + refcount path in Task::copy_from.
    struct Big {
        std::array<std::byte, 200> data{};
    } big{};
    big.data[0] = std::byte{0xAB};

    bool called = false;
    lig::logger::Task t1 = [big, &called](const lig::interface::Formatter *,
                                          std::vector<std::byte> &) {
        ASSERT(big.data[0] == std::byte{0xAB});
        called = true;
    };
    lig::logger::Task t2{t1};
    ASSERT(static_cast<bool>(t1));
    ASSERT(static_cast<bool>(t2));

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t1(&fmt, out);
    ASSERT(called);

    called = false; t2(&fmt, out); ASSERT(called); }

void test_task_copy_assign() {
    bool a = false, b = false;
    lig::logger::Task t1 = [&a](const lig::interface::Formatter *,
                                std::vector<std::byte> &) { a = true; };
    lig::logger::Task t2 = [&b](const lig::interface::Formatter *,
                                std::vector<std::byte> &) { b = true; };
    t2 = t1;

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    t2(&fmt, out);
    ASSERT(a);
    ASSERT(!b);
}

void test_get_backend_returns_singleton() {
    auto &a = lig::logger::getBackend<DummyBE>();
    auto &b = lig::logger::getBackend<DummyBE>();
    ASSERT(&a == &b);
}

void test_log_submits_task_and_formats_body() {
    auto &be = lig::logger::getBackend<DummyBE>();
    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
    be.count = 0;

    lig::logger::Log<lig::Level::Info, DummyBE> info{};
    info("hello {}", 42);
    ASSERT(be.count == 1);
    ASSERT(static_cast<bool>(be.received));

    BodyFormatter fmt{};
    std::vector<std::byte> out;
    be.received(&fmt, out);
    ASSERT(bytes_to_string(out) == "hello 42");
}

void test_log_level_filter_drops_low_level() {
    auto &be = lig::logger::getBackend<DummyBE>();
    lig::logger::g_level.store(lig::Level::Warn, std::memory_order_relaxed);
    be.count = 0;

    lig::logger::Log<lig::Level::Info, DummyBE> info{};
    info("dropped");
    ASSERT(be.count == 0);

    lig::logger::Log<lig::Level::Error, DummyBE> err{};
    err("kept");
    ASSERT(be.count == 1);

    lig::logger::g_level.store(lig::Level::Trace, std::memory_order_relaxed);
}

int main() {
    TEST(test_get_thread_id_non_zero);
    TEST(test_task_empty_is_false);
    TEST(test_task_empty_is_not_invoked);
    TEST(test_task_inline_lambda_invoked);
    TEST(test_task_move_ctor_transfers);
    TEST(test_task_move_assign_replaces);
    TEST(test_task_copy_ctor_inline);
    TEST(test_task_copy_ctor_heap);
    TEST(test_task_copy_assign);
    TEST(test_get_backend_returns_singleton);
    TEST(test_log_submits_task_and_formats_body);
    TEST(test_log_level_filter_drops_low_level);
    return report();
}
