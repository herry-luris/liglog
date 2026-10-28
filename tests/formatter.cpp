// Tests for component/formatter.hpp.
//
// Coverage:
//   - TextFormatter: default output, each Status bit disabled individually,
//                    all bits disabled, Config ctor, createTextFormatter
//   - TermFormatter: default output includes ANSI escapes, custom per-level
//                    state, createTermFormatter

#include "component/formatter.hpp"
#include "test_help.hpp"

#include <string>

namespace {

std::string bytes_to_string(const std::vector<std::byte> &buf) {
    return std::string(reinterpret_cast<const char *>(buf.data()), buf.size());
}

lig::LogEntry make_entry() {
    lig::LogEntry e{};
    e.head.level = lig::Level::Info;
    e.head.line = 42;
    e.head.thread_id = 1234;
    e.head.filename = "test.cpp";
    e.head.funcname = "test_fn";
    e.head.now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    e.body = "hello world";
    return e;
}

} // namespace

void test_text_formatter_default_contains_all_parts() {
    lig::component::TextFormatter fmt{};
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    const auto s = bytes_to_string(buf);
    ASSERT(s.find("[Info]") != std::string::npos);
    ASSERT(s.find("1234") != std::string::npos);
    ASSERT(s.find("test.cpp:42") != std::string::npos);
    ASSERT(s.find("test_fn") != std::string::npos);
    ASSERT(s.find("hello world") != std::string::npos);
}

void test_text_formatter_no_timestamp() {
    lig::component::TextFormatter fmt{};
    fmt.status.setTimeStamp(false);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    const auto s = bytes_to_string(buf);
    ASSERT(s.find(" - [Info]") != std::string::npos);
    ASSERT(s.find("20") != 0);
}

void test_text_formatter_no_thread_id() {
    lig::component::TextFormatter fmt{};
    fmt.status.setThreadId(false);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    ASSERT(bytes_to_string(buf).find("1234") == std::string::npos);
}

void test_text_formatter_no_location() {
    lig::component::TextFormatter fmt{};
    fmt.status.setLocation(false);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    ASSERT(bytes_to_string(buf).find("test.cpp") == std::string::npos);
}

void test_text_formatter_no_funcname() {
    lig::component::TextFormatter fmt{};
    fmt.status.setFuncname(false);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    ASSERT(bytes_to_string(buf).find("test_fn") == std::string::npos);
}

void test_text_formatter_all_disabled() {
    lig::component::TextFormatter fmt{};
    fmt.status.setTimeStamp(false).setThreadId(false).setFuncname(false).setLocation(false);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    const auto s = bytes_to_string(buf);
    ASSERT(s.find(" - [Info]") != std::string::npos);
    ASSERT(s.find("hello world") != std::string::npos);
    ASSERT(s.find("1234") == std::string::npos);
    ASSERT(s.find("test.cpp") == std::string::npos);
    ASSERT(s.find("test_fn") == std::string::npos);
}

void test_text_formatter_from_config() {
    lig::component::TextFormatter::Config cfg{};
    cfg.status.setThreadId(false);
    lig::component::TextFormatter fmt{cfg};
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    ASSERT(bytes_to_string(buf).find("1234") == std::string::npos);
}

void test_create_text_formatter() {
    auto fmt = lig::component::createTextFormatter({});
    ASSERT(fmt != nullptr);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt->format(e, buf);
    ASSERT(!buf.empty());
}

void test_term_formatter_default_contains_ansi() {
    lig::component::TermFormatter fmt{lig::component::TermFormatter::Config{}};
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    const auto s = bytes_to_string(buf);
    ASSERT(s.find("[Info]") != std::string::npos);
    ASSERT(s.find("hello world") != std::string::npos);
    ASSERT(s.find("\033[0m") != std::string::npos);
}

void test_term_formatter_with_red_state() {
    lig::component::TermFormatter::Config cfg{};
    term_ctl::TermStr::State red{};
    red.fg_color.set_value(term_ctl::Color16::Red);
    cfg.states[static_cast<size_t>(lig::Level::Info)] = red;
    lig::component::TermFormatter fmt{cfg};

    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt.format(e, buf);
    const auto s = bytes_to_string(buf);
    // ANSI SGR for foreground red (16-color) is 31.
    ASSERT(s.find("31") != std::string::npos);
}

void test_create_term_formatter() {
    auto fmt = lig::component::createTermFormatter({});
    ASSERT(fmt != nullptr);
    auto e = make_entry();
    std::vector<std::byte> buf;
    fmt->format(e, buf);
    ASSERT(!buf.empty());
}

int main() {
    TEST(test_text_formatter_default_contains_all_parts);
    TEST(test_text_formatter_no_timestamp);
    TEST(test_text_formatter_no_thread_id);
    TEST(test_text_formatter_no_location);
    TEST(test_text_formatter_no_funcname);
    TEST(test_text_formatter_all_disabled);
    TEST(test_text_formatter_from_config);
    TEST(test_create_text_formatter);
    TEST(test_term_formatter_default_contains_ansi);
    TEST(test_term_formatter_with_red_state);
    TEST(test_create_term_formatter);
    return report();
}
