# liglog

一个前端延迟恒定、多线程无锁争用的 C++20 日志库。

---

## 为什么选 liglog

- **前端延迟恒定。** 零参数和三个参数都是 ~22 ns，不随格式字符串复杂度增长。
- **多线程无锁争用。** 8 线程下 2.9 ns/op；spdlog 的 `stderr_sink_mt` 是 825 ns/op。
- **一个 transport 一个 formatter，多个 sink。** 可以同时输出彩色终端和轮转文件，调用点不需要重复工作。
- **运行时配置。** formatter / sink / transport 类型都在运行时选择，调用点无需模板实例化。
- **可自定义组件。** `Sink`、`Formatter`、`ITransport` 都是抽象接口，派生即可扩展。

## 什么时候不该用 liglog

- 需要 pattern formatter（`"%Y-%m-%d %H:%M:%S %l %v"`）。
- 需要按天轮转、压缩、保留策略。
- 需要工业级跨平台成熟度（liglog 是个人项目，Windows 分支未验证）。
- 对格式化吞吐比前端延迟更敏感（spdlog 用 `fmt`，比 `std::format` 快）。

---

## 30 秒上手

```cpp
#include "lig.hpp"

int main() {
    lig::config()
        .transport()
            .text_formatter().submit()
            .console_sink().submit()
            .submit()
        .start();

    lig::Info{}("hello {}", 42);
}
```

输出：

```
2026-10-28 23:20:04 - [Info] 1234567890 (main.cpp:6) mainhello 42
```

> `TextFormatter` 当前在 `funcname` 和 `body` 之间不加分隔符，所以 `main` 和 `hello 42` 会连在一起。如需分隔，用自定义 formatter 或在 body 前加空格。

---

## 安装 / 构建

```bash
git clone https://github.com/herry-luris/liglog
cd liglog

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```cmake
add_subdirectory(liglog)          # 或 add_subdirectory(third_party/liglog)
target_link_libraries(my_app PRIVATE liglog)
```

编译期级别过滤：`-DLIG_LOGGER_LEVEL=0/1/2`（0 全部保留，1 丢弃 Info 以下，2 丢弃 Warn 以下，低级别调用被 `if constexpr` 完全消除）。运行时用 `lig::config().level(...)` 调整 `g_level`。

---

## 常用场景

### 输出到控制台

```cpp
lig::config()
    .level(lig::Level::Info)
    .transport()
        .text_formatter().timestamp(true).submit()
        .console_sink().to_err(true).submit()
        .submit()
    .start();
```

### 输出到文件并轮转

```cpp
lig::config()
    .transport()
        .text_formatter().submit()
        .file_sink()
            .dir("/var/log/myapp")
            .filename("app.log")
            .max_file_size(64 * 1024 * 1024)
            .sync_interval(100)            // 每 100 次 write fsync 一次
            .submit()
        .kind(lig::interface::ITransport::Kind::Async)
        .submit()
    .start();
```

### 同时输出到控制台和文件

一个 transport 一个 formatter，多个 sink 挂在同一个 transport 上：

```cpp
lig::config()
    .transport()
        .text_formatter().timestamp(true).submit()
        .console_sink().to_err(true).submit()
        .file_sink().dir("/var/log/myapp").filename("app.log").submit()
        .kind(lig::interface::ITransport::Kind::Async)
        .submit()
    .start();
```

如果控制台要彩色、文件要纯文本，就挂两个 transport，各配一个 formatter：

```cpp
using Kind = lig::interface::ITransport::Kind;

lig::config()
    .transport()                                    // 终端，彩色，同步
        .term_formatter()
            .state<lig::Level::Error>(red_bold)
            .submit()
        .console_sink().to_err(true).submit()
        .kind(Kind::Sync)
        .submit()
    .transport()                                    // 文件，纯文本，异步
        .text_formatter().submit()
        .file_sink().dir("/var/log/myapp").filename("app.log").submit()
        .kind(Kind::Async)
        .submit()
    .start();
```

### 自定义 Sink（进阶）

```cpp
class SharedMemorySink : public lig::interface::Sink {
  public:
    auto write(const void *data, size_t len) -> WriteRes override {
        // 拷贝到共享内存环形缓冲区
        WriteRes r{};
        r.len = len;
        return r;
    }
    auto is_valid() const noexcept -> bool override { return true; }
};

class SharedMemorySinkConfig : public lig::interface::ISinkConfig {
  public:
    auto start() -> std::unique_ptr<lig::interface::Sink> override {
        return std::make_unique<SharedMemorySink>();
    }
};

auto cfg = std::make_unique<SharedMemorySinkConfig>();
lig::config()
    .transport()
        .text_formatter().submit()
        .sink(std::move(cfg))
        .submit()
    .start();
```

### 自定义 Formatter（进阶）

```cpp
class JsonFormatter : public lig::interface::Formatter {
  public:
    void format(const lig::LogEntry &entry, std::vector<std::byte> &buf) const override {
        auto s = std::format(R"({{"level":"{}","msg":"{}"}})",
                             lig::get_level_string(entry.head.level), entry.body);
        const auto *p = reinterpret_cast<const std::byte *>(s.data());
        buf.insert(buf.end(), p, p + s.size());
    }
};

class JsonFormatterConfig : public lig::interface::IFormatterConfig {
  public:
    auto start() -> std::unique_ptr<const lig::interface::Formatter> override {
        return std::make_unique<JsonFormatter>();
    }
};

auto fmt = std::unique_ptr<lig::interface::IFormatterConfig>(
    std::make_unique<JsonFormatterConfig>());
lig::config()
    .transport()
        .formatter(fmt)
        .console_sink().submit()
        .submit()
    .start();
```

### Flush 与关闭

```cpp
// Flush 所有 transport。Async 会阻塞到 worker 排空队列并写入 sink，不保证 fsync。
lig::logger::getBackend<lig::InnerBackend>().flush();

// 停止。Async worker 排空剩余任务后退出。
lig::logger::getBackend<lig::InnerBackend>().stop();
```

---

## 架构

四层结构：**前端 → Backend → Transport → Formatter + Sink**。

```
┌──────────────────────────────────────────────────────────────┐
│  前端（任意线程）                                              │
│    lig::Info{}("...") → Log<L, BE>::operator()               │
│      ├─ 编译期过滤（LIG_LOGGER_LEVEL）                        │
│      ├─ 运行时过滤（g_level）                                  │
│      ├─ 捕获 LogHead + 参数 → Task                            │
│      └─ getBackend<BE>().submit(Task)                        │
└────────────────────────────┬─────────────────────────────────┘
                             ▼
┌──────────────────────────────────────────────────────────────┐
│  InnerBackend（进程单例）→ 分发给每个 Transport                │
└────────────────────────────┬─────────────────────────────────┘
              ┌──────────────┴──────────────┐
              ▼                             ▼
┌──────────────────────────┐  ┌──────────────────────────┐
│  SyncTransport           │  │  AsyncTransport          │
│  thread_local cache      │  │  VyukovMpmc<Task, 512>   │
│  from() → 格式化 + 缓存    │  │  from() → try_push       │
│  flush() → 写 sink        │  │  worker → 格式化 + 批量写  │
└──────────┬───────────────┘  └──────────┬───────────────┘
           ▼                             ▼
┌──────────────────────────────────────────────────────────────┐
│  Formatter + Sink[]（每个 transport 独立）                      │
│    TextFormatter / TermFormatter / 自定义                     │
│    ConsoleSink / FileSink / 自定义                            │
└──────────────────────────────────────────────────────────────┘
```

### 关键设计

**1. 延迟格式化。** 前端不调用 `std::vformat`，只把格式字符串指针、参数（按值）、`LogHead`（level、line、filename、funcname、thread_id、now）捕获进类型擦除的 `Task`（128 字节内联缓冲，超出走堆）。`vformat` 在 transport 线程执行。

**2. 一个 transport 一个 formatter，多个 sink。** `TransportConfig` 持有恰好一个 `Formatter` 和任意多个 `Sink`。每个 transport 独立格式化，自定义 formatter 和 sink 通过同一套接口接入。

**3. transport 类型是运行时选择。** `SyncTransport` 在调用线程格式化，用 `thread_local` 缓存；`AsyncTransport` 推入有界 MPMC 队列（`VyukovMpmc<Task, 512, Strategy::Wait>`）。前端通过 `ITransport` 分发，不知道对端是哪种。

**4. 显式背压。** `AsyncTransport::from` 最多等 3 ms 获取队列槽位。队列持续满时回退到内联格式化并追加到互斥锁保护的 overflow 缓冲区，worker 下一轮排空。过载时延迟从 ns 跳到 µs，但不丢消息。

---

## 性能

测试环境：AMD Ryzen 9 7940H（Zen 4，8C/16T，最高 5.26 GHz），Arch Linux x86_64，Clang 17，libstdc++，CMake + Ninja，`-O2 -DNDEBUG`，程序内 `steady_clock` 计时，**未绑核**，spdlog 1.14.x。

| 场景 | liglog | spdlog | 谁更快 |
|---|---|---|---|
| 前端（无参数） | 22.7 ns | 34.6 ns | liglog 1.5× |
| 前端（混合参数） | 22.5 ns | 81.4 ns | liglog 3.6× |
| 多线程同步（8 线程） | 2.9 ns/op | 825 ns/op | liglog 283× |
| 异步 `from` | 91 ns | 214 ns | liglog 2.3× |
| 完整格式化 | 169 ns | 76 ns | spdlog 2.2× |

**结论**：liglog 在前端延迟和多线程扩展性上占优，因为前端只捕获参数、格式化在 transport 线程执行，且 `SyncTransport` 用 `thread_local` 缓存避免 per-write 互斥锁。spdlog 在格式化吞吐上占优，因为 `fmt` 比 `std::format` 快，且 `memory_buf_t` 预分配比 `vector::insert` 更激进。

完整基准数据见 `res.log`，可运行 `./benchmarks/compare.sh build` 复现。

---

## 已知限制

1. **`FileSink` 绝对路径 + `change_dir = false` 的轮转目录陷阱。** 如果 `file` 带父路径（如 `"/var/log/app.log"`）且 `change_dir = false`，`set_file` 会把 `rotate_file_` 设为该绝对路径，但 `get_next_file()` 用的是 `config_.dir`。第一次轮转就会写到 `config_.dir` 而不是 `/var/log`。传 `change_dir = true` 或显式 `set_directory("/var/log")` 即可。
2. **`Config::start()` 不是幂等的。** 调用两次会创建重复 formatter/sink，随后被丢弃（无泄漏，但浪费）。
3. **`TransportConfig` 在 `start()` 前必须设置 formatter**，否则空解引用。
4. **空 `Task` 不能被调用。** `Task::operator()` 在 debug 下 assert，release 下是 UB。只有 `AsyncTransport` 把空 task 用作 flush 哨兵。
5. **`AsyncTransport` 的 overflow 缓冲区无界。** 持续过载时无限增长。
6. **`Log` 构造函数里捕获 `source_location`。** `static thread_local Info logger{}` 报告的是声明位置，不是调用位置。
7. **参数按值捕获。** 大 `std::string` 参数每次调用有一次堆分配。传 `std::string_view` 或 `const char*` 可避免。
8. **没有 pattern formatter、按天轮转、压缩、保留策略。** 需要这些请用 spdlog。

---

## 构建与测试

### CMake 选项

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIGLOG_BUILD_TEST=ON \
    -DLIGLOG_BUILD_BENCHMARKS=ON \
    -DLIG_LOGGER_LEVEL=0
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `LIGLOG_BUILD_TEST` | `ON` | 构建单元测试 |
| `LIGLOG_BUILD_BENCHMARKS` | `ON` | 构建基准测试 |
| `LIG_LOGGER_LEVEL` | `0` | 编译期级别过滤：0 = 动态，1 = 丢弃 Info 以下，2 = 丢弃 Warn 以下 |

### 测试与基准

```bash
ctest --test-dir build --output-on-failure

./build/benchmarks/bench_liglog 2>/dev/null
./build/benchmarks/bench_spdlog 2>/dev/null
./benchmarks/compare.sh build
```

测试覆盖 `Level` / `Status` / `Task`（内联/堆、移动/拷贝）/ `Log` 提交与级别过滤 / 两种 formatter / 两种 sink / `DataBuffer` / `SinkWrapper` / 两种 transport / 流式配置与 `InnerBackend` 生命周期 / `TermStr`。

console sink 写 stderr，所以 `2>/dev/null` 静默日志输出，保留 stdout 上的基准表。

---

## 项目状态

**实验性库。** API 可能变化，没有 ABI/API 稳定性承诺。Windows 分支能编译但未测试。`FileSink` 有已知轮转 bug（见[已知限制](#已知限制)）。

适合场景：多生产者线程、需要恒定前端延迟、接受更简单的 formatter 集合。不适合场景：需要 pattern formatter、轮转策略、久经考验的工业级实现。

---

## 项目结构

```
liglog/
├── CMakeLists.txt
├── include/
│   ├── lig.hpp               # 入口，InnerBackend，Config，TransportConfig
│   ├── interface.hpp         # Level，Status，LogHead，LogEntry，Sink，Formatter，ITransport
│   ├── logger.hpp            # Task，Log<L, BE>，g_level
│   ├── component/
│   │   ├── formatter.hpp     # TextFormatter，TermFormatter
│   │   ├── sink.hpp          # FileSink，ConsoleSink
│   │   └── transport.hpp     # SyncTransport，AsyncTransport，DataBuffer，SinkWrapper
│   ├── nlqueue/mpmc.hpp      # VyukovMpmc（AsyncTransport 使用）
│   └── term_ctl/term_ctl.hpp # ANSI 终端颜色
├── src/                      # formatter.cpp，sink.cpp，transport.cpp
├── tests/                    # 单元测试（test_help.hpp，无框架）
├── benchmarks/               # 基准测试（bench_help.hpp，无框架）
└── docs/api.md               # 完整 API 参考
```

---

## 从 spdlog 迁移

```cpp
// spdlog
spdlog::info("hello {}", 42);
spdlog::error("failed: {}", err);

// liglog
lig::Info{}("hello {}", 42);
lig::Error{}("failed: {}", err);
```

配置从 `spdlog::set_pattern` / `spdlog::sinks::...` 改为 `lig::config().transport().text_formatter()...`，详见[常用场景](#常用场景)。注意 liglog 没有 `pattern_formatter`，需要自定义 formatter 才能得到自定义格式。

---

## API 参考

完整 API 参考见 [`docs/api.md`](docs/api.zh.md)，包含每个公开类型的参数含义、前置/后置条件、线程安全保证与边界情况表格。

---

## 许可证

MIT。详见 `LICENSE`。

---

## 致谢

- **Vyukov MPMC**：Dmitry Vyukov 的槽位代数设计。`nlqueue::VyukovMpmc` 是其直接实现。
- **spdlog**：Gabi Melman 的 C++ 日志库，`benchmarks/bench_spdlog.cpp` 用于对比。
- **fmt**：Victor Zverovich 的格式化库。spdlog 使用它；liglog 使用 `std::format`。
