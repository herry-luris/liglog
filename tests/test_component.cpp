#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "component/sink.hpp"
#include "component/formatter.hpp"
#include "interface.hpp"
#include "test_helpers.hpp"

namespace fs = std::filesystem;

class FileSinkTest : public ::testing::Test {
  protected:
    fs::path tmp;

    void SetUp() override {
        auto base = fs::temp_directory_path() / "lig_test_fsink";
        fs::create_directories(base);

        static std::atomic<uint64_t> counter{0};
        tmp = base / std::to_string(counter.fetch_add(1));
        fs::create_directories(tmp);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    std::vector<fs::path> list_logs() {
        std::vector<fs::path> out;
        for (auto &e : fs::directory_iterator(tmp)) {
            if (e.is_regular_file() && e.path().extension() == ".log") {
                out.push_back(e.path());
            }
        }
        return out;
    }
};

TEST_F(FileSinkTest, ConstructIsNotValidUntilOpen) {
    lig::component::FileSink::Config cfg;
    cfg.dir = tmp;
    cfg.file = "app.log";
    cfg.sync_interval = 0;

    lig::component::FileSink sink{cfg};
    EXPECT_FALSE(sink.is_valid());
    EXPECT_TRUE(sink.open());
    EXPECT_TRUE(sink.is_valid());
}

TEST_F(FileSinkTest, WriteAndReadBack) {
    lig::component::FileSink::Config cfg;
    cfg.dir = tmp;
    cfg.file = "app.log";
    cfg.sync_interval = 0;
    cfg.max_file_size = lig::component::FileSink::MinFileSize;

    lig::component::FileSink sink{cfg};
    ASSERT_TRUE(sink.open());

    const std::string msg = "hello lig logger\n";
    auto res = sink.write(msg.data(), msg.size());
    ASSERT_TRUE(res) << res.err.message();
    EXPECT_EQ(res.len, msg.size());

    sink.close();

    auto logs = list_logs();
    ASSERT_EQ(logs.size(), 1u);
    std::ifstream ifs{logs[0], std::ios::binary};
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, msg);
}

TEST_F(FileSinkTest, WriteZeroBytes) {
    lig::component::FileSink::Config cfg;
    cfg.dir = tmp;
    cfg.file = "zero.log";
    cfg.sync_interval = 0;
    lig::component::FileSink sink{cfg};
    ASSERT_TRUE(sink.open());

    auto res = sink.write("", 0);
    EXPECT_TRUE(res);
    EXPECT_EQ(res.len, 0u);
}

TEST_F(FileSinkTest, RotateOnMaxSize) {
    // 用最小合法 max_file_size（8MB）来触发轮转
    lig::component::FileSink::Config cfg;
    cfg.dir = tmp;
    cfg.file = "rot.log";
    cfg.sync_interval = 0;
    cfg.max_file_size = lig::component::FileSink::MinFileSize;

    lig::component::FileSink sink{cfg};
    ASSERT_TRUE(sink.open());

    // 写入 9MB，超过 8MB 一次
    std::vector<char> data(9 * 1024 * 1024, 'a');
    auto res = sink.write(data.data(), data.size());
    ASSERT_TRUE(res) << res.err.message();

    sink.close();

    auto logs = list_logs();
    EXPECT_GE(logs.size(), 2u) << "should have rotated into at least 2 files";

    // 所有文件的字节总数应该 >= 写入的字节数（因为路径覆盖，实际应完全等于）
    size_t total = 0;
    for (auto &p : logs)
        total += fs::file_size(p);
    EXPECT_EQ(total, data.size());
}

TEST_F(FileSinkTest, RejectTooSmallOrTooLargeMaxFileSize) {
    lig::component::FileSink sink{lig::component::FileSink::Config{}};
    EXPECT_FALSE(sink.set_max_file_size(lig::component::FileSink::MinFileSize - 1));
    EXPECT_FALSE(sink.set_max_file_size(lig::component::FileSink::MaxFileSize + 1));
    EXPECT_TRUE(sink.set_max_file_size(lig::component::FileSink::MinFileSize));
    EXPECT_TRUE(sink.set_max_file_size(lig::component::FileSink::MaxFileSize));
}

TEST_F(FileSinkTest, WriteFailWhenNotOpen) {
    lig::component::FileSink::Config cfg;
    cfg.dir = tmp;
    cfg.file = "closed.log";
    lig::component::FileSink sink{cfg};
    // 未 open
    const char *msg = "x";
    auto res = sink.write(msg, 1);
    EXPECT_FALSE(res);
}

// ---------- ConsoleSink ----------
TEST(ConsoleSinkTest, StdoutValid) {
    lig::component::ConsoleSink::Config cfg{};
    cfg.to_err = false;
    lig::component::ConsoleSink sink{cfg};
    EXPECT_TRUE(sink.is_valid());
}

TEST(ConsoleSinkTest, StderrValid) {
    lig::component::ConsoleSink::Config cfg{};
    cfg.to_err = true;
    lig::component::ConsoleSink sink{cfg};
    EXPECT_TRUE(sink.is_valid());
}

TEST(ConsoleSinkTest, WriteSomeBytes) {
    lig::component::ConsoleSink::Config cfg{};
    lig::component::ConsoleSink sink{cfg};
    // 写入 stderr 也行，避免污染 stdout 测试输出
    const char *msg = "";
    auto res = sink.write(msg, 0);
    EXPECT_TRUE(res);
}

// ---------- TextFormatter ----------
class TextFormatterTest : public ::testing::Test {
  protected:
    lig::LogEntry make_entry() {
        lig::LogEntry entry;
        entry.head.level = lig::Level::Info;
        entry.head.line = 42;
        entry.head.thread_id = 12345;
        entry.head.filename = "file.cpp";
        entry.head.funcname = "func";
        entry.head.now =
            std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        entry.body = "hello world";
        return entry;
    }
};

TEST_F(TextFormatterTest, DefaultStatusIncludesEverything) {
    lig::component::TextFormatter fmt;
    auto entry = make_entry();
    std::vector<std::byte> buf;
    fmt.format(entry, buf);
    std::string s(reinterpret_cast<const char *>(buf.data()), buf.size());

    EXPECT_NE(s.find("Info"), std::string::npos);
    EXPECT_NE(s.find("hello world"), std::string::npos);
    EXPECT_NE(s.find("file.cpp"), std::string::npos);
    EXPECT_NE(s.find("func"), std::string::npos);
    EXPECT_NE(s.find("12345"), std::string::npos);
}

TEST_F(TextFormatterTest, DisableAllOptionalFields) {
    lig::component::TextFormatter fmt;
    fmt.status.setTimeStamp(false).setThreadId(false).setFuncname(false).setLocation(false);
    auto entry = make_entry();
    std::vector<std::byte> buf;
    fmt.format(entry, buf);
    std::string s(reinterpret_cast<const char *>(buf.data()), buf.size());

    EXPECT_NE(s.find("Info"), std::string::npos);
    EXPECT_NE(s.find("hello world"), std::string::npos);
    EXPECT_EQ(s.find("file.cpp"), std::string::npos);
    EXPECT_EQ(s.find("func"), std::string::npos);
    EXPECT_EQ(s.find("12345"), std::string::npos);
}

TEST_F(TextFormatterTest, EmptyBody) {
    lig::component::TextFormatter fmt;
    auto entry = make_entry();
    entry.body.clear();
    std::vector<std::byte> buf;
    fmt.format(entry, buf);
    EXPECT_GT(buf.size(), 0u);
}

// ---------- 基础性能 ----------
TEST(ComponentPerformance, FileSinkThroughput) {
    auto dir = fs::temp_directory_path() / "lig_perf_fsink";
    fs::remove_all(dir);
    fs::create_directories(dir);

    lig::component::FileSink::Config cfg;
    cfg.dir = dir;
    cfg.file = "perf.log";
    cfg.sync_interval = 0;
    cfg.max_file_size = lig::component::FileSink::MinFileSize;

    lig::component::FileSink sink{cfg};
    ASSERT_TRUE(sink.open());

    const std::string chunk(512, 'x');
    constexpr size_t iters = 20000;
    double ns = lig_test::measure_ns_per_op(iters, [&](size_t) {
        auto res = sink.write(chunk.data(), chunk.size());
        (void)res;
    });
    sink.close();

    double mb_per_s = (chunk.size() * 1e9) / ns / (1024.0 * 1024.0);
    std::cout << "[perf] FileSink::write 512B -> " << ns << " ns/op, " << mb_per_s << " MiB/s\n";
    SUCCEED();
}

TEST(ComponentPerformance, TextFormatterThroughput) {
    lig::component::TextFormatter fmt;
    lig::LogEntry entry;
    entry.head.level = lig::Level::Info;
    entry.head.line = 1;
    entry.head.thread_id = 1;
    entry.head.filename = "f.cpp";
    entry.head.funcname = "fn";
    entry.head.now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    entry.body = "some log message body";

    constexpr size_t iters = 100000;
    std::vector<std::byte> buf;
    buf.reserve(256);
    double ns = lig_test::measure_ns_per_op(iters, [&](size_t) {
        buf.clear();
        fmt.format(entry, buf);
    });
    std::cout << "[perf] TextFormatter::format -> " << ns << " ns/op\n";
    EXPECT_LT(ns, 20000.0); // 20us 宽松上限
}
