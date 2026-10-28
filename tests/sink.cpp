// Tests for component/sink.hpp.
//
// Coverage:
//   - FileSink: open/close, write and read back, set_directory rejection on
//               regular file, set_max_file_size range, set_sync_interval,
//               set_file, createFileSink
//   - ConsoleSink: stdout/stderr validity, close, createConsoleSink

#include "component/sink.hpp"
#include "test_help.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path temp_dir(const std::string &name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

std::string read_first_log(const std::filesystem::path &dir) {
    for (auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".log") {
            std::ifstream in(entry.path());
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        }
    }
    return {};
}

} // namespace

void test_file_sink_open_close() {
    auto dir = temp_dir("liglog_test_file_sink_open_close");

    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;
    cfg.file = "test.log";
    lig::component::FileSink sink{cfg};

    ASSERT(!sink.is_valid());
    ASSERT(sink.open());
    ASSERT(sink.is_valid());
    sink.close();
    ASSERT(!sink.is_valid());

    std::filesystem::remove_all(dir);
}

void test_file_sink_write_and_read_back() {
    auto dir = temp_dir("liglog_test_file_sink_write");

    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;
    cfg.file = "test.log";
    cfg.sync_interval = 0;
    lig::component::FileSink sink{cfg};
    ASSERT(sink.open());

    const std::string msg = "hello, liglog";
    auto res = sink.write(msg.data(), msg.size());
    ASSERT(res);
    ASSERT(res.len == msg.size());
    ASSERT(sink.size() == msg.size());
    sink.close();

    ASSERT(read_first_log(dir) == msg);
    std::filesystem::remove_all(dir);
}

void test_file_sink_set_directory_rejects_regular_file() {
    auto dir = temp_dir("liglog_test_file_sink_set_dir");
    auto file = dir / "not_a_dir.txt";
    { std::ofstream(file) << "x"; }

    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;
    lig::component::FileSink sink{cfg};

    ASSERT(!sink.set_directory(file));
    std::filesystem::remove_all(dir);
}

void test_file_sink_set_max_file_size_range() {
    lig::component::FileSink::Config cfg{};
    lig::component::FileSink sink{cfg};

    ASSERT(!sink.set_max_file_size(1));
    ASSERT(!sink.set_max_file_size(256ull * 1024 * 1024));
    ASSERT(sink.set_max_file_size(16ull * 1024 * 1024));
}

void test_file_sink_set_sync_interval() {
    lig::component::FileSink::Config cfg{};
    lig::component::FileSink sink{cfg};
    sink.set_sync_interval(5);
    ASSERT(sink.config().sync_interval == 5);
}

void test_file_sink_set_file() {
    auto dir = temp_dir("liglog_test_file_sink_set_file");
    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;
    lig::component::FileSink sink{cfg};
    ASSERT(sink.set_file("foo.log"));
    ASSERT(sink.config().file.filename() == "foo.log");
    std::filesystem::remove_all(dir);
}

void test_console_sink_stdout_valid() {
    lig::component::ConsoleSink::Config cfg{};
    cfg.to_err = false;
    lig::component::ConsoleSink sink{cfg};
    ASSERT(sink.is_valid());

    auto res = sink.write("", 0);
    ASSERT(res);

    sink.close();
    ASSERT(!sink.is_valid());
}

void test_console_sink_stderr_valid() {
    lig::component::ConsoleSink::Config cfg{};
    cfg.to_err = true;
    lig::component::ConsoleSink sink{cfg};
    ASSERT(sink.is_valid());
}

void test_create_file_sink() {
    auto dir = temp_dir("liglog_test_create_file_sink");
    lig::component::FileSink::Config cfg{};
    cfg.dir = dir;

    auto sink = lig::component::createFileSink(cfg);
    ASSERT(sink != nullptr);
    ASSERT(!sink->is_valid());

    std::filesystem::remove_all(dir);
}

void test_create_console_sink() {
    auto sink = lig::component::createConsoleSink({});
    ASSERT(sink != nullptr);
    ASSERT(sink->is_valid());
}

int main() {
    TEST(test_file_sink_open_close);
    TEST(test_file_sink_write_and_read_back);
    TEST(test_file_sink_set_directory_rejects_regular_file);
    TEST(test_file_sink_set_max_file_size_range);
    TEST(test_file_sink_set_sync_interval);
    TEST(test_file_sink_set_file);
    TEST(test_console_sink_stdout_valid);
    TEST(test_console_sink_stderr_valid);
    TEST(test_create_file_sink);
    TEST(test_create_console_sink);
    return report();
}
