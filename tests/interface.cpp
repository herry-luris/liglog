// Tests for interface.hpp.
//
// Coverage:
//   - get_level_string: all six Level values
//   - Status: default mask, mask ctor, all setters (individual and chained)
//   - Sink::WriteRes: success and error conversion to bool

#include "interface.hpp"
#include "test_help.hpp"

void test_get_level_string() {
    ASSERT(lig::get_level_string(lig::Level::Trace) == "Trace");
    ASSERT(lig::get_level_string(lig::Level::Debug) == "Debug");
    ASSERT(lig::get_level_string(lig::Level::Info) == "Info");
    ASSERT(lig::get_level_string(lig::Level::Warn) == "Warn");
    ASSERT(lig::get_level_string(lig::Level::Error) == "Error");
    ASSERT(lig::get_level_string(lig::Level::Fatal) == "Fatal");
}

void test_status_default_all_enabled() {
    lig::Status s{};
    ASSERT(s.hasTimeStamp());
    ASSERT(s.hasThreadId());
    ASSERT(s.hasFuncname());
    ASSERT(s.hasLocation());
}

void test_status_setters_toggle() {
    lig::Status s{};
    s.setTimeStamp(false);
    ASSERT(!s.hasTimeStamp());
    ASSERT(s.hasThreadId());

    s.setTimeStamp(true);
    ASSERT(s.hasTimeStamp());

    s.setThreadId(false);
    ASSERT(!s.hasThreadId());
    s.setFuncname(false);
    ASSERT(!s.hasFuncname());
    s.setLocation(false);
    ASSERT(!s.hasLocation());

    s.setThreadId(true).setFuncname(true).setLocation(true);
    ASSERT(s.hasThreadId());
    ASSERT(s.hasFuncname());
    ASSERT(s.hasLocation());
}

void test_status_mask_ctor() {
    lig::Status zero{static_cast<uint8_t>(0)};
    ASSERT(!zero.hasTimeStamp());
    ASSERT(!zero.hasThreadId());
    ASSERT(!zero.hasFuncname());
    ASSERT(!zero.hasLocation());

    lig::Status only_ts{lig::Status::TimeStamp};
    ASSERT(only_ts.hasTimeStamp());
    ASSERT(!only_ts.hasThreadId());
    ASSERT(!only_ts.hasFuncname());
    ASSERT(!only_ts.hasLocation());
}

void test_sink_write_res_ok() {
    lig::interface::Sink::WriteRes r{};
    r.len = 42;
    ASSERT(r);
    ASSERT(r.len == 42);
    ASSERT(r.err.value() == 0);
}

void test_sink_write_res_error() {
    lig::interface::Sink::WriteRes r{};
    r.err = std::make_error_code(std::errc::io_error);
    ASSERT(!r);
    ASSERT(r.err.value() != 0);
}

int main() {
    TEST(test_get_level_string);
    TEST(test_status_default_all_enabled);
    TEST(test_status_setters_toggle);
    TEST(test_status_mask_ctor);
    TEST(test_sink_write_res_ok);
    TEST(test_sink_write_res_error);
    return report();
}
