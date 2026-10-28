# liglog

A C++20 logging library with constant front-end latency and no per-write mutex contention across producer threads.

`LIGLOG` focuses on one thing: **keep the cost of a front-end log call flat regardless of argument count, and avoid per-write mutex contention across producer threads.** Formatting, file I/O, and rotation all happen on transport threads.

---

## Why liglog

- **Constant front-end latency.** Zero args and three args both take ~22 ns. The cost does not grow with format-string complexity.
- **No per-write mutex contention.** 2.9 ns/op at 8 threads; spdlog's `stderr_sink_mt` is 825 ns/op.
- **One formatter per transport, many sinks per transport.** Write to a colored terminal and a rotating file simultaneously without duplicating work at the call site.
- **Runtime configuration.** Formatter, sink, and transport kind are chosen at runtime. The call site needs no template instantiation.
- **Custom components.** `Sink`, `Formatter`, and `ITransport` are abstract interfaces. Derive and plug in.

## When not to use liglog

- You need a pattern formatter (`"%Y-%m-%d %H:%M:%S %l %v"`).
- You need daily rotation, compression, or retention policies.
- You need industrial-grade cross-platform maturity (liglog is a personal project; the Windows branches are unverified).
- You care more about formatting throughput than front-end latency (spdlog uses `fmt`, which is faster than `std::format`).

---

## 30-second start

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

Output:

```
2026-10-28 23:20:04 - [Info] 1234567890 (main.cpp:6) mainhello 42
```

> `TextFormatter` currently inserts no separator between `funcname` and `body`, so `main` and `hello 42` run together. To separate them, use a custom formatter or prepend a space to the body.

---

## Install / Build

```bash
git clone https://github.com/herry-luris/liglog
cd liglog

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```cmake
add_subdirectory(liglog)          # or add_subdirectory(third_party/liglog)
target_link_libraries(my_app PRIVATE liglog)
```

Compile-time level filter: `-DLIG_LOGGER_LEVEL=0/1/2` (0 keeps everything, 1 drops below Info, 2 drops below Warn; the low-level calls are completely eliminated by `if constexpr`). Runtime filtering: `lig::config().level(...)` adjusts `g_level`.

---

## Common scenarios

### Write to the console

```cpp
lig::config()
    .level(lig::Level::Info)
    .transport()
        .text_formatter().timestamp(true).submit()
        .console_sink().to_err(true).submit()
        .submit()
    .start();
```

### Write to a rotating file

```cpp
lig::config()
    .transport()
        .text_formatter().submit()
        .file_sink()
            .dir("/var/log/myapp")
            .filename("app.log")
            .max_file_size(64 * 1024 * 1024)
            .sync_interval(100)            // fsync every 100 writes
            .submit()
        .kind(lig::interface::ITransport::Kind::Async)
        .submit()
    .start();
```

> When `dir` and `filename` are set separately, `file` has no parent path, so `rotate_file_ = dir / "app.log"` and rotated files are `dir / "1_app.log"`, etc. If you instead use `filename("/var/log/myapp/app.log")` with a parent path, pass `change_dir = true` or set `dir` explicitly in `FileSinkConfig`.

### Write to both console and file

One formatter per transport, many sinks per transport:

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

If the console should be colored and the file should be plain text, use two transports, each with its own formatter:

```cpp
using Kind = lig::interface::ITransport::Kind;

lig::config()
    .transport()                                    // terminal, colored, sync
        .term_formatter()
            .state<lig::Level::Error>(red_bold)
            .submit()
        .console_sink().to_err(true).submit()
        .kind(Kind::Sync)
        .submit()
    .transport()                                    // file, plain text, async
        .text_formatter().submit()
        .file_sink().dir("/var/log/myapp").filename("app.log").submit()
        .kind(Kind::Async)
        .submit()
    .start();
```

### Custom Sink (advanced)

```cpp
class SharedMemorySink : public lig::interface::Sink {
  public:
    auto write(const void *data, size_t len) -> WriteRes override {
        // copy into a shared-memory ring buffer
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

### Custom Formatter (advanced)

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

### Flush and shutdown

```cpp
// Flush all transports. For Async this blocks until the worker has drained the queue
// and written to the sinks; it does not fsync.
lig::logger::getBackend<lig::InnerBackend>().flush();

// Stop. Async workers drain remaining tasks and exit.
lig::logger::getBackend<lig::InnerBackend>().stop();
```

---

## Architecture

Four layers: **front-end → backend → transport → formatter + sink**.

```
┌──────────────────────────────────────────────────────────────┐
│  Front-end (any thread)                                      │
│    lig::Info{}("...") → Log<L, BE>::operator()               │
│      ├─ compile-time filter (LIG_LOGGER_LEVEL)               │
│      ├─ runtime filter (g_level)                             │
│      ├─ capture LogHead + arguments → Task                   │
│      └─ getBackend<BE>().submit(Task)                        │
└────────────────────────────┬─────────────────────────────────┘
                             ▼
┌──────────────────────────────────────────────────────────────┐
│  InnerBackend (process singleton) → dispatch to each Transport │
└────────────────────────────┬─────────────────────────────────┘
              ┌──────────────┴──────────────┐
              ▼                             ▼
┌──────────────────────────┐  ┌──────────────────────────┐
│  SyncTransport           │  │  AsyncTransport          │
│  thread_local cache      │  │  VyukovMpmc<Task, 512>   │
│  from() → format + cache │  │  from() → try_push       │
│  flush() → write sinks   │  │  worker → format + batch │
└──────────┬───────────────┘  └──────────┬───────────────┘
           ▼                             ▼
┌──────────────────────────────────────────────────────────────┐
│  Formatter + Sink[] (per transport)                          │
│    TextFormatter / TermFormatter / custom                    │
│    ConsoleSink / FileSink / custom                           │
└──────────────────────────────────────────────────────────────┘
```

### Key design points

**1. Deferred formatting.** The front-end does not call `std::vformat`. It captures the format-string pointer, the arguments (by value), and a `LogHead` (level, line, filename, funcname, thread_id, now) into a type-erased `Task` (128-byte inline buffer; heap fallback for larger captures). `vformat` runs on the transport thread.

**2. One formatter per transport, many sinks per transport.** `TransportConfig` holds exactly one `Formatter` and any number of `Sink`s. Each transport formats independently. Custom formatters and sinks plug in through the same interfaces.

**3. Transport kind is a runtime choice.** `SyncTransport` formats on the calling thread with a `thread_local` cache; `AsyncTransport` pushes into a bounded MPMC queue (`VyukovMpmc<Task, 512, Strategy::Wait>`). The front-end dispatches through `ITransport` and does not know which kind it is talking to.

**4. Explicit backpressure.** `AsyncTransport::from` waits up to 3 ms for a queue slot. If the queue stays full, it falls back to formatting inline and appending to a mutex-protected overflow buffer, which the worker drains on its next iteration. Under sustained overload, latency degrades from ns to µs, but no message is dropped.

---

## Performance

Test environment: AMD Ryzen 9 7940H (Zen 4, 8C/16T, up to 5.26 GHz), Arch Linux x86_64, Clang 17, libstdc++, CMake + Ninja, `-O2 -DNDEBUG`, in-process `steady_clock` timing, **no CPU pinning**, spdlog 1.14.x.

| Scenario | liglog | spdlog | Faster |
|---|---|---|---|
| Front-end (no args) | 22.7 ns | 34.6 ns | liglog 1.5× |
| Front-end (mixed args) | 22.5 ns | 81.4 ns | liglog 3.6× |
| Multi-thread sync (8 threads) | 2.9 ns/op | 825 ns/op | liglog 283× |
| Async `from` | 91 ns | 214 ns | liglog 2.3× |
| Full formatting | 169 ns | 76 ns | spdlog 2.2× |

**Summary**: liglog wins on front-end latency and multi-thread scaling because the front-end only captures arguments, formatting runs on the transport thread, and `SyncTransport` uses a `thread_local` cache to avoid a per-write mutex. spdlog wins on formatting throughput because `fmt` is faster than `std::format`, and `memory_buf_t` pre-allocates more aggressively than `vector::insert`.

Full benchmark data is in `res.log`. Reproduce with `./benchmarks/compare.sh build`.

---

## Known limitations

1. **`FileSink` absolute-path + `change_dir = false` rotation trap.** If `file` has a parent path (e.g. `"/var/log/app.log"`) and `change_dir = false`, `set_file` sets `rotate_file_` to the absolute path, but `get_next_file()` uses `config_.dir`. The first rotation goes to `config_.dir` instead of `/var/log`. Pass `change_dir = true` or explicitly `set_directory("/var/log")`.
2. **`Config::start()` is not idempotent.** Calling it twice creates duplicate formatters/sinks, then discards them (no leak, wasted work).
3. **`TransportConfig` must have a formatter before `start()`.** Otherwise the null pointer is dereferenced.
4. **Empty `Task` must not be invoked.** `Task::operator()` asserts in debug builds; release builds are undefined behaviour. Only `AsyncTransport` uses empty tasks as flush sentinels.
5. **`AsyncTransport`'s overflow buffer is unbounded.** Under sustained overload it grows without limit.
6. **`Log`'s constructor captures `source_location`.** A `static thread_local Info logger{}` reports the declaration site, not the call site.
7. **Arguments are captured by value.** Large `std::string` arguments cost one heap allocation per call. Pass `std::string_view` or `const char*` to avoid the copy.
8. **No pattern formatter, daily rotation, compression, or retention policies.** Use spdlog if you need these.

---

## Build and test

### CMake options

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIGLOG_BUILD_TEST=ON \
    -DLIGLOG_BUILD_BENCHMARKS=ON \
    -DLIG_LOGGER_LEVEL=0
```

| Option | Default | Description |
|---|---|---|
| `LIGLOG_BUILD_TEST` | `ON` | Build unit tests |
| `LIGLOG_BUILD_BENCHMARKS` | `ON` | Build benchmarks |
| `LIG_LOGGER_LEVEL` | `0` | Compile-time level filter: 0 = dynamic, 1 = drop below Info, 2 = drop below Warn |

### Tests and benchmarks

```bash
ctest --test-dir build --output-on-failure

./build/benchmarks/bench_liglog 2>/dev/null
./build/benchmarks/bench_spdlog 2>/dev/null
./benchmarks/compare.sh build
```

Tests cover `Level` / `Status` / `Task` (inline/heap, move/copy) / `Log` submission and level filtering / both formatters / both sinks / `DataBuffer` / `SinkWrapper` / both transports / fluent config and `InnerBackend` lifecycle / `TermStr`.

The console sink writes to stderr, so `2>/dev/null` silences log output and keeps the benchmark tables on stdout.

---

## Project status

**Experimental library.** The API may change; there is no ABI/API stability promise. The Windows branches compile but are untested.

Suitable for: multiple producer threads, constant front-end latency, a simpler formatter set. Not suitable for: pattern formatter, rotation policies, or a battle-tested industrial implementation.

---

## Project layout

```
liglog/
├── CMakeLists.txt
├── include/
│   ├── lig.hpp               # Entry point, InnerBackend, Config, TransportConfig
│   ├── interface.hpp         # Level, Status, LogHead, LogEntry, Sink, Formatter, ITransport
│   ├── logger.hpp            # Task, Log<L, BE>, g_level
│   ├── component/
│   │   ├── formatter.hpp     # TextFormatter, TermFormatter
│   │   ├── sink.hpp          # FileSink, ConsoleSink
│   │   └── transport.hpp     # SyncTransport, AsyncTransport, DataBuffer, SinkWrapper
│   ├── nlqueue/mpmc.hpp      # VyukovMpmc (used by AsyncTransport)
│   └── term_ctl/term_ctl.hpp # ANSI terminal color
├── src/                      # formatter.cpp, sink.cpp, transport.cpp
├── tests/                    # unit tests (test_help.hpp, no framework)
├── benchmarks/               # benchmarks (bench_help.hpp, no framework)
└── docs/api.md               # full API reference
```

---

## Migrating from spdlog

```cpp
// spdlog
spdlog::info("hello {}", 42);
spdlog::error("failed: {}", err);

// liglog
lig::Info{}("hello {}", 42);
lig::Error{}("failed: {}", err);
```

Configuration moves from `spdlog::set_pattern` / `spdlog::sinks::...` to `lig::config().transport().text_formatter()...`. See [Common scenarios](#common-scenarios). Note that liglog has no `pattern_formatter`; a custom formatter is required for custom output formats.

---

## API reference

See [`docs/api.md`](docs/api.md) for the complete API reference, including parameter meanings, preconditions and postconditions, thread-safety guarantees, and edge-case tables.

---

## License

MIT. See `LICENSE` for details.

---

## Acknowledgements

- **Vyukov MPMC**: Dmitry Vyukov's slot-generation design. `nlqueue::VyukovMpmc` is a direct implementation.
- **spdlog**: Gabi Melman's C++ logging library. `benchmarks/bench_spdlog.cpp` compares against it.
- **fmt**: Victor Zverovich's formatting library. spdlog uses it; liglog uses `std::format`.
