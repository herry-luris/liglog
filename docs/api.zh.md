# liglog API 参考

`liglog` 全部公开类型与函数的完整参考。

- 入口头文件：`lig.hpp`
- 命名空间：`lig`、`lig::interface`、`lig::logger`、`lig::component`、`lig::component::internal`
- C++ 标准：C++20（`std::source_location`、`std::format`、`std::span`、`std::atomic::wait`）

本文档使用的标记：

- **前置条件** — 调用前必须成立；违反是未定义行为（UB）。
- **后置条件** — 调用成功后保证成立。
- **线程安全** — 哪些线程可以并发调用。
- **边界情况** — 输入合法但行为可能出乎意料的情况。

---

## 目录

1. [核心类型](#1-核心类型)
2. [前端 API](#2-前端-api)
3. [配置 API](#3-配置-api)
4. [后端](#4-后端)
5. [组件接口](#5-组件接口)
6. [内置 Sink](#6-内置-sink)
7. [内置 Formatter](#7-内置-formatter)
8. [内置 Transport](#8-内置-transport)
9. [内部组件](#9-内部组件)
10. [边界情况汇总](#10-边界情况汇总)

---

## 1. 核心类型

### 1.1 `lig::Level`

```cpp
enum struct Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Fatal };
```

**底层类型**：`uint8_t`。数值顺序重要：`Trace < Debug < Info < Warn < Error < Fatal`。

**前置条件**：只接受 `0..5` 六个值。把其他值传给 `get_level_string` 或 `Log<L, BE>` 是未定义行为。

**过滤规则**（前端使用）：级别为 `L` 的消息只有在 `L >= g_level` 时才会被发出。比较基于数值。

**线程安全**：值类型，可跨线程自由拷贝。

---

### 1.2 `lig::get_level_string`

```cpp
auto get_level_string(Level level) -> std::string_view;
```

**参数**

| 名称 | 类型 | 含义 |
|---|---|---|
| `level` | `Level` | 六个枚举值之一。 |

**返回值**：指向静态字符串字面量（`"Trace"`、`"Debug"`、`"Info"`、`"Warn"`、`"Error"`、`"Fatal"`）的 `std::string_view`。视图在进程生命周期内有效。

**前置条件**：`level` 必须在 `[Level::Trace, Level::Fatal]` 内。越界值会索引到内部数组之外，是 UB。

**线程安全**：纯函数，任何线程安全。

**边界情况**：对合法输入永不返回空视图。

---

### 1.3 `lig::Status`

```cpp
class Status {
  public:
    static constexpr uint32_t TimeStamp = 1 << 0;  // 0b0001
    static constexpr uint32_t ThreadId  = 1 << 1;  // 0b0010
    static constexpr uint32_t Funcname  = 1 << 2;  // 0b0100
    static constexpr uint32_t Location  = 1 << 3;  // 0b1000

    Status() = default;
    Status(uint8_t mask);

    auto hasTimeStamp() const noexcept -> bool;
    auto hasThreadId()  const noexcept -> bool;
    auto hasFuncname()  const noexcept -> bool;
    auto hasLocation()  const noexcept -> bool;

    auto setTimeStamp(bool enable) noexcept -> Status &;
    auto setThreadId(bool enable)  noexcept -> Status &;
    auto setFuncname(bool enable)  noexcept -> Status &;
    auto setLocation(bool enable)  noexcept -> Status &;

  private:
    uint8_t mask_{TimeStamp | ThreadId | Funcname | Location};
};
```

**用途**：告诉 `Formatter` 要输出哪些可选的头字段。Formatter 读取这些位；它可以选择忽略。

**构造函数 `Status(uint8_t mask)`**

| 名称 | 类型 | 含义 |
|---|---|---|
| `mask` | `uint8_t` | 启用的字段位图。位 `0..3` 有意义；位 `4..7` 被忽略。 |

**默认构造函数**：`mask_ = 0b1111`（四个字段全部启用）。

**`hasTimeStamp()` / `hasThreadId()` / `hasFuncname()` / `hasLocation()`**

对应位被置位时返回 `true`。`const`、`noexcept`、无副作用。

**`setTimeStamp(bool)` / `setThreadId(bool)` / `setFuncname(bool)` / `setLocation(bool)`**

| 名称 | 类型 | 含义 |
|---|---|---|
| `enable` | `bool` | `true` 置位，`false` 清位。 |

返回 `*this`，支持链式调用：

```cpp
status.setTimeStamp(false).setThreadId(false);
```

**线程安全**：纯值类型。每个线程应操作自己的副本；跨线程共享未同步的 `Status` 是数据竞争。

**边界情况**

- `Status(0xFF)` 置位全部四位，忽略位 `4..7`。
- 忽略 `status` 的自定义 Formatter 无论 `Status` 如何都输出相同内容。

**与 `LogHead` 的交互**：`Status` **不**控制字段是否被捕获。`LogHead` 总是携带 `level`、`line`、`thread_id`、`filename`、`funcname` 和 `now`。`Status` 只控制 Formatter 把哪些字段写进输出。

---

### 1.4 `lig::LogHead`

```cpp
struct LogHead {
    Level level{};
    uint32_t line{};
    size_t thread_id{};
    const char *filename{};
    const char *funcname{};
    std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> now{};
};
```

**逐字段说明**

| 字段 | 类型 | 来源 | 生命周期 |
|---|---|---|---|
| `level` | `Level` | `Log<L, BE>` 的模板参数 `L` | 值 |
| `line` | `uint32_t` | `Log` 构造时的 `std::source_location::line()` | 值 |
| `thread_id` | `size_t` | `pthread_self()` 或 `GetCurrentThreadId()`，每线程缓存 | 值 |
| `filename` | `const char*` | `std::source_location::file_name()` | 非拥有，静态存储 |
| `funcname` | `const char*` | `std::source_location::function_name()` | 非拥有，静态存储 |
| `now` | `system_clock::time_point`（毫秒精度） | 在 `Log::operator()` 中捕获 | 值 |

**前置条件**：如果平台无法提供源位置信息，`filename` 和 `funcname` 可能为 `nullptr`。内置 Formatter 会替换为 `"<unknown>"`。

**所有权**：`filename` / `funcname` 指向运行时拥有的静态存储。**不得释放、修改或保存到进程退出之后。** 拷贝 `LogHead` 是安全的；指针保持有效。

**时间语义**：`now` 在**前端线程**上、`Log::operator()` 执行时捕获。在异步模式下，Transport 可能在几毫秒（或过载时几秒）之后才格式化。如果你需要 Transport 侧的时间戳，在 Formatter 内部自行捕获。

**线程安全**：构造后只读；跨线程拷贝安全。

---

### 1.5 `lig::LogEntry`

```cpp
struct LogEntry {
    LogHead head{};
    std::string body{};
};
```

**字段**

- `head` — 前端捕获的元数据。
- `body` — `std::vformat` 替换参数后的消息正文。

**生命周期**

- 在 `Task` 的 lambda 内部创建（每线程一个 `thread_local LogEntry`，跨调用复用）。
- 格式化前会调用 `entry.body.clear()`；`entry.head = info` 重新赋值头部。
- `formatter->format(entry, data)` 以 `entry` 和 `std::vector<std::byte>` 输出缓冲区为参数调用。
- `format` 返回后，entry 被下一条消息复用。

**前置条件**：传给 `Task::operator()` 的 `formatter` 必须非空，且必须是 Transport 配置时使用的同一个 Formatter。

**边界情况**

- `body` 可能包含嵌入的 `\0`。`std::string` 能处理；Formatter 必须用 `body.size()`，不能用 `strlen`。
- 自定义 Formatter 不得在 `format` 调用之后保留 `entry` 或 `entry.body` 的引用——存储会被复用。

---

## 2. 前端 API

### 2.1 `lig::logger::g_level`

```cpp
inline std::atomic<::lig::Level> g_level{};
```

**用途**：进程级最低日志级别。每次 `Log::operator()` 以 `memory_order_relaxed` 读取它。

**默认值**：`Level::Trace`（0）。所有消息都会发出。

**写入**：`Config::level()` 写入它。任何线程都可以随时写入；变更最终对所有前端线程可见。

**读取**：每次日志调用一次 relaxed load，x86 上约 1–2 ns。

**前置条件**：存储的值应在 `[Level::Trace, Level::Fatal]` 内。越界值不会崩溃，但数值比较失去意义。

**边界情况**

- 设置 `g_level = Level::Fatal` 会丢弃 `Fatal` 以下的所有消息。`Fatal` 本身仍会通过。
- `LIG_LOGGER_LEVEL`（编译期）在 `g_level` **之前**生效。两者都过滤时，消息被丢弃。
- 不是 per-transport：所有 Transport 共享同一个 `g_level`。

---

### 2.2 `lig::logger::getThreadId`

```cpp
inline auto getThreadId() -> uint64_t;
```

**返回值**：平台线程标识符的整数值。

- POSIX：`static_cast<uint64_t>(pthread_self())`。
- Windows：`static_cast<uint64_t>(GetCurrentThreadId())`。

**缓存**：`Log::operator()` 把结果存进 `thread_local` 变量，底层调用每线程只发生一次。

**线程安全**：任何线程安全。

**边界情况**

- 值是平台相关的，不能跨进程移植。
- 线程退出后，同一数值可能被复用。
- 如果平台的 pthread 类型超过 64 位（罕见），会截断；POSIX 保证能装下。

---

### 2.3 `lig::logger::getBackend<BE>()`

```cpp
template <typename BE> inline auto getBackend() -> BE &;
```

**模板参数**

| 名称 | 含义 |
|---|---|
| `BE` | Backend 类型。必须继承 `lig::interface::IBackend<BE>` 并提供 `void submit_impl(::lig::logger::Task &&)`。 |

**返回值**：函数局部 `static BE` 实例的引用。首次调用时构造，程序退出时析构（静态析构顺序）。

**前置条件**：`BE` 必须满足 `IBackend` 的 CRTP 契约。`IBackend<BE>::verify()` 中有一个 `static_assert` 检查 `submit_impl` 的签名。

**线程安全**：初始化线程安全（C++11 magic statics）。构造完成后，并发调用返回同一个引用。

**边界情况**

- 静态初始化顺序：如果 `BE` 的构造函数依赖其他全局对象，可能踩到静态初始化顺序陷阱。`InnerBackend` 的构造函数很简单（只有 vector 和 atomic），实践中安全。
- 用户通常不直接调用。`Log<L, BE>::operator()` 和 `Config::start()` 在内部使用它。

---

### 2.4 `lig::logger::Task`

```cpp
struct Task {
    static constexpr size_t InlineSize = 128;

    Task() = default;

    Task(Task &&other) noexcept;
    auto operator=(Task &&other) noexcept -> Task &;

    Task(const Task &other);
    auto operator=(const Task &other) -> Task &;

    ~Task();

    template <typename F, /* SFINAE */>
    Task(F &&func);

    explicit operator bool() const noexcept;
    void operator()(const interface::Formatter *formatter,
                    std::vector<std::byte> &data) const;
};
```

**用途**：前端产生的类型擦除可调用对象。持有捕获的参数和预录的 `LogHead`。延迟格式化在 Transport 调用 `Task` 时发生。

#### 2.4.1 构造

**默认构造函数**

创建一个空 `Task`，`vtable_ == nullptr`、`block_ == nullptr`。`operator bool` 返回 `false`。内部用作 flush 哨兵。

**模板构造函数 `Task(F &&func)`**

| 名称 | 类型 | 含义 |
|---|---|---|
| `func` | 任意可调用 `F` | 必须以 `void(const Formatter*, std::vector<std::byte>&)` 形式可调用。不能是 `Task` 本身。 |

SFINAE 约束（全部成立，否则构造函数从重载集中移除）：

- `!std::is_same_v<std::decay_t<F>, Task>` — 避免与拷贝/移动构造歧义。
- `std::is_invocable_v<std::decay_t<F>, const Formatter*, std::vector<std::byte>&>` — 签名检查。
- `std::is_same_v<void, std::invoke_result_t<...>>` — 返回类型必须是 `void`。

**存储决策**（编译期）：

```cpp
if constexpr (sizeof(F) <= 128 && alignof(F) <= alignof(std::max_align_t)) {
    // 内联存储于 inline_[128]
} else {
    // 堆上 ControlBlock，原子引用计数
}
```

| 情况 | `F` 存放位置 | 拷贝语义 | 分配 |
|---|---|---|---|
| `sizeof(F) <= 128` 且对齐满足 | 内联于 `Task` | 调用 `F` 的拷贝构造 | 无 |
| 否则 | `ControlBlock` 中的 `new F` | 原子引用计数 | `new ControlBlock`、`new F` |

**后置条件**：`operator bool` 返回 `true`。`vtable_` 指向 `VirtualTable<F>`。

**边界情况**

- 大捕获（例如 `std::array<std::byte, 200>`）强制走堆路径。堆路径支持廉价拷贝（引用计数递增），在 Backend 向多个 Transport 扇出时有用。
- 捕获 move-only 类型的 lambda 不可拷贝。此时 `Task` 的拷贝构造会编译失败；只有移动构造可用。前端总是把 `Task` move 进 `submit`，所以正常路径不受影响。

#### 2.4.2 移动构造 / 移动赋值

```cpp
Task(Task &&other) noexcept;
auto operator=(Task &&other) noexcept -> Task &;
```

**行为**

- 内联路径：调用 `vtable_->move(inline_, other.inline_)`，placement-move `F` 并析构源。
- 堆路径：窃取 `block_`（引用计数不变），设置 `other.block_ = nullptr`。
- 两种情况都把 `other.vtable_ = nullptr`。源变为空。

**后置条件**：源为空（`operator bool == false`），目标持有原可调用对象。

**异常安全**：`noexcept`。`F` 的移动不能抛出；前端总是使用 `noexcept` 安全的捕获（格式字符串、小整数、指针）。

#### 2.4.3 拷贝构造 / 拷贝赋值

```cpp
Task(const Task &other);
auto operator=(const Task &other) -> Task &;
```

**行为**

- 内联路径：调用 `vtable_->copy(inline_, other.inline_)`，placement-copy `F`。`F` 必须可拷贝构造。
- 堆路径：`block_ = other.block_`，`block_->ref_.fetch_add(1, relaxed)`。引用计数跨副本共享。

**后置条件**：两个 `Task` 都有效；析构一个会减少引用计数。

**边界情况**

- 拷贝捕获 move-only lambda 的 `Task` 是编译错误。前端 `submit` 路径使用 `std::move`，正常使用不受影响。
- 堆 `Task` 的多个副本共享同一个 `F` 实例。如果 `F` 有可变状态，所有副本都能看到——各 Transport 独立调用 `F`，但调用的是同一个对象。

#### 2.4.4 析构函数

```cpp
~Task();
```

**行为**

- 内联路径：`vtable_->destroy(inline_)`，调用 `~F()`。
- 堆路径：`ref_.fetch_sub(1, acq_rel)`。新值为 `0` 时销毁 `F` 并 `operator delete` `block_->heap_` 和 `block_`。

**边界情况**

- 如果最后一个副本位于 Transport worker 线程，`F` 的析构在那里发生，不是前端线程。捕获类型应能在任意线程析构。

#### 2.4.5 `operator bool`

```cpp
explicit operator bool() const noexcept;
```

**返回值**：`vtable_ != nullptr`。

**用途**：防止调用空 `Task`。`AsyncTransport` 用 `if (!task)` 检测 flush 哨兵。

**边界情况**：`explicit` 关键字阻止隐式转换为 `bool`，但 `if (static_cast<bool>(task))` 和 `if (!!task)` 都能用。

#### 2.4.6 `operator()`

```cpp
void operator()(const interface::Formatter *formatter,
                std::vector<std::byte> &data) const;
```

**参数**

| 名称 | 类型 | 含义 |
|---|---|---|
| `formatter` | `const Formatter*` | 用于渲染的 Formatter。必须是 Transport 的 Formatter。 |
| `data` | `std::vector<std::byte>&` | 输出缓冲区。Formatter **追加**到它（不清空）。 |

**行为**

1. `assert(vtable_ && "empty Task must not be invoked");` — debug 构建下空 `Task` 直接 abort。
2. `vtable_->invoke(ptr(), formatter, data)` — 以 `ptr()`（内联存储或堆）、`formatter`、`data` 调用存储的 `F`。

**前置条件**：`vtable_ != nullptr`，即 `operator bool` 为 `true`。`formatter` 必须非空。

**后置条件**：`data` 增长了 Formatter 追加的内容。

**线程安全**：任意单线程安全。对**同一个** `Task` 的并发调用只有在 `F` 线程安全时才安全（前端 lambda 是安全的：它们只写 `thread_local LogEntry` 和调用方提供的 `data`）。

**边界情况**

- release 构建下调用空 `Task` 是 UB（`nullptr` 解引用）。总是先检查 `operator bool`。
- `data` 不会被清空。Transport 在调用前会 `tmp_data.clear()`，但自定义 Transport 可能不会。
- 如果 `F` 抛异常（不应该，但可能），异常传播给调用方。`AsyncTransport` 的 worker 没有 `try`/`catch`；异常会终止进程。

---

### 2.5 `lig::logger::Log<L, BE>`

```cpp
template <Level L, typename BE> class Log {
  public:
    template <typename... Args>
    void operator()(const std::format_string<Args...> &format, Args &&...args);

    Log(std::source_location &&loc = std::source_location::current());
    Log(const Log &) = delete;
    Log(Log &&) = delete;
    auto operator=(const Log &) -> Log & = delete;
    auto operator=(Log &&) -> Log & = delete;

  private:
    std::source_location loc_{};
};
```

**用途**：前端入口。捕获格式字符串和参数，然后向 `BE` 提交一个 `Task`。

#### 2.5.1 构造函数

```cpp
Log(std::source_location &&loc = std::source_location::current());
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `loc` | `std::source_location&&` | 默认为调用方位置。不要手动传入。 |

**行为**：存储 `loc_`。`source_location` 在 `Log` 构造时捕获，不是 `operator()` 调用时。

**边界情况**：如果你跨调用持有 `Log` 对象，每次调用都会报告**构造函数**的文件/行，而不是调用点。典型用法是在每个调用点构造 `Log`（`lig::Info{}(...)`），或缓存 `static thread_local` logger（此时报告的是 logger 声明位置，通常不是想要的）。

**推荐用法**

```cpp
// 报告调用点位置。略贵（每次调用一次 source_location 构造）。
lig::Info{}("hello");

// 报告声明点位置。快，但容易误导。
static thread_local lig::Info logger{};
logger("hello");
```

#### 2.5.2 `operator()`

```cpp
template <typename... Args>
void operator()(const std::format_string<Args...> &format, Args &&...args);
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `format` | `std::format_string<Args...>` | 编译期检查的格式字符串。 |
| `args` | `Args&&...` | 代入 `format` 的参数。每个必须满足 `std::formatter`。 |

**行为**（按顺序）：

1. **编译期过滤**（`if constexpr`）：
   - `LIG_LOGGER_LEVEL == 1` 且 `L < Info` → 返回。
   - `LIG_LOGGER_LEVEL == 2` 且 `L < Warn` → 返回。
   - 该分支在编译期完全消除；无运行时检查。
2. **运行时过滤**：`if (g_level.load(relaxed) > L) return;`。
3. **捕获 `thread_id`**：`thread_local const auto thread_id = getThreadId();` — 每线程首次调用后缓存。
4. **构造 `LogHead`**：`level = L`、`line = loc_.line()`、`thread_id`、`filename = loc_.file_name()`、`funcname = loc_.function_name()`、`now = system_clock::now()` 向下取整到毫秒。
5. **构造 `Task`**：lambda 按值捕获 `info`、`fmt`（指向 `format.get()` 的 `std::string_view`）和 `args...`。
6. **提交**：`getBackend<BE>().submit(std::move(task))`。

**lambda 内部**（在 Transport 线程上运行）：

```cpp
constexpr size_t len = sizeof...(Args);
thread_local LogEntry entry{};
entry.head = info;
entry.body.clear();
if constexpr (len == 0) {
    entry.body.append(fmt);
} else {
    std::vformat_to(std::back_inserter(entry.body), fmt.data(),
                    std::make_format_args(args...));
}
formatter->format(entry, data);
```

- `LogEntry` 是 `thread_local` 且被复用；每次清空 `body`。
- `vformat_to` 执行实际替换。
- `formatter->format` 把渲染结果追加到 `data`。

**后置条件**（如果过滤通过）：向 `BE` 提交了一个 `Task`。格式化被延迟。

**线程安全**：任意线程安全。每次调用构造新 `Task` 并 move 进 `submit`。

**边界情况**

- **参数生命周期**：`args...` **按值**捕获。`std::string` 参数会被拷贝进 lambda（每次调用一次堆分配）。要避免拷贝，传 `std::string_view` 或 `const char*` —— 但引用的数据必须在 Transport 格式化之前保持存活。
- **格式字符串生命周期**：`format.get()` 返回指向 `std::format_string` 静态存储的指针。存进 `string_view` 是安全的。
- **`std::format` 抛出**：如果 lambda 内部 `vformat_to` 抛异常（例如错误的格式说明符），异常从 `Task::operator()` 传播出去。在异步 worker 上没有 catch；进程终止。同步路径上异常传播给 `Log::operator()` 的调用方。
- **`LIG_LOGGER_LEVEL`** 是编译期读取的；修改后需要重新编译包含 `logger.hpp` 的所有 TU。
- **`g_level` 竞争**：并发的 store 可能被进行中的调用看到或不看到。这是设计使然——过滤是建议性的，不是同步的。

#### 2.5.3 别名

```cpp
template <typename BE> using Trace = Log<Level::Trace, BE>;
template <typename BE> using Debug = Log<Level::Debug, BE>;
template <typename BE> using Info  = Log<Level::Info,  BE>;
template <typename BE> using Warn  = Log<Level::Warn,  BE>;
template <typename BE> using Error = Log<Level::Error, BE>;
template <typename BE> using Fatal = Log<Level::Fatal, BE>;
```

`lig.hpp` 额外提供绑定到 `InnerBackend` 的别名：

```cpp
using Trace = logger::Trace<InnerBackend>;
using Debug = logger::Debug<InnerBackend>;
using Info  = logger::Info<InnerBackend>;
using Warn  = logger::Warn<InnerBackend>;
using Error = logger::Error<InnerBackend>;
using Fatal = logger::Fatal<InnerBackend>;
```

---

## 3. 配置 API

### 3.1 `lig::config()`

```cpp
inline auto config() -> Config;
```

按值返回一个新的 `Config`。每次调用产生一个独立的 builder；可以有多个 `Config` 对象，但只有一个 `start()` 会生效（见 `InnerBackend::add_entity`）。

**线程安全**：任何线程安全。返回的 `Config` 不得跨线程共享。

---

### 3.2 `lig::Config`

```cpp
struct Config {
    Config() = default;
    ~Config() = default;

    auto level(Level level) -> Config &;
    auto transport() -> TransportConfig &;
    void start();
};
```

#### 3.2.1 `level(Level)`

```cpp
auto level(Level level) -> Config &;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `level` | `Level` | 新的全局最低级别。 |

**行为**：`g_level.store(level, memory_order_relaxed)`。立即生效。

**返回值**：`*this`，支持链式。

**边界情况**：设置 `Level::Fatal` 会对所有 Transport 丢弃 `Fatal` 以下的消息。在 `start()` 之前或之后调用都行。

#### 3.2.2 `transport()`

```cpp
auto transport() -> TransportConfig &;
```

**行为**

1. `auto t = std::make_unique<TransportConfig>(*this);`
2. `transports_.push_back(std::move(t));`
3. 返回 `*transports_.back()`。

**后置条件**：返回的引用指向 vector 内部。它保持有效直到下一次 `transport()` 调用**仅当** vector 不重新分配。因为 `transports_` 是 `std::vector<std::unique_ptr<TransportConfig>>`，指向的 `TransportConfig` 在堆上；其地址不受 vector 重分配影响。**引用在 `Config` 生命周期内保持有效**。

**边界情况**：每次调用创建一个独立的 Transport。没有"复用"或"替换"语义。

#### 3.2.3 `start()`

```cpp
void start();
```

**行为**

```cpp
for (auto &entry : transports_) entry->start();
logger::getBackend<InnerBackend>().start();
```

每个 `TransportConfig::start()` 物化 Formatter 和 Sink，然后调用 `InnerBackend::add_entity`。最后 `InnerBackend::start()` 启动所有 Transport（spawn 异步 worker）。

**后置条件**：Backend 运行中；日志生效。

**前置条件**（每条都必须成立，否则 UB 或静默异常）：

- 每个 `TransportConfig` 都设置了 Formatter（否则 `formatter_->start()` 解引用空 `unique_ptr` → 段错误）。
- 每个 Sink builder 都已 `submit()`，或 sink 通过 `sink(...)` 添加。

**边界情况**

- **不幂等**：调用两次会重新运行 `TransportConfig::start()`。第二轮创建新 Formatter/Sink，但 `InnerBackend::add_entity` 返回 `false`（`is_start == true`）。新建的对象在 `TransportConfig::start()` 结束时销毁。无泄漏，但浪费。
- **`is_start_` 是 atomic 但未使用**：`Config` 的私有成员 `std::atomic_bool is_start_{false}` 从未被读写。是死状态。

---

### 3.3 `lig::TransportConfig`

```cpp
class TransportConfig {
  public:
    TransportConfig(Config &config);

    auto text_formatter() -> component::TextFormatterConfig<TransportConfig> &;
    auto term_formatter() -> component::TermFormatterConfig<TransportConfig> &;

    auto console_sink() -> component::ConsoleSinkConfig<TransportConfig> &;
    auto file_sink()    -> component::FileSinkConfig<TransportConfig> &;
    auto sink(std::unique_ptr<::lig::interface::ISinkConfig> sink) -> TransportConfig &;
    auto formatter(std::unique_ptr<::lig::interface::IFormatterConfig> &formatter) -> TransportConfig &;

    auto kind(interface::ITransport::Kind kind) -> TransportConfig &;
    auto max_cache_size(size_t max_cache_size) -> TransportConfig &;
    auto submit() -> Config &;

    void start();
};
```

**用途**：单个 Transport 的流式 builder。每个 setter 返回一个子 builder 的引用；`submit()` 返回 `Config`。

#### 3.3.1 Formatter setter

**`text_formatter()`**

用 `TextFormatterConfig<TransportConfig>` 替换当前 Formatter，返回其引用。

**`term_formatter()`**

同上，但返回 `TermFormatterConfig<TransportConfig>`。

**`formatter(unique_ptr<IFormatterConfig>&)`**

接管用户提供的 Formatter config。`unique_ptr` 与 `formatter_` 交换，调用后调用方的 `unique_ptr` 变为空。

**返回值**（`text_formatter` / `term_formatter`）：`*static_cast<Derived*>(formatter_.get())`。

**`start()` 的前置条件**：三者至少调用一个。否则 `formatter_` 为空，`start()` 段错误。

**边界情况**

- 一个 `TransportConfig` 持有**恰好一个** Formatter。在 `term_formatter()` 之后调用 `text_formatter()` 会**替换**前一个；旧的被销毁。
- 返回的引用有效直到下一次调用 Formatter setter（它会重置 `formatter_`）。

#### 3.3.2 Sink setter

**`console_sink()`**

向 `sinks_` 追加一个 `ConsoleSinkConfig<TransportConfig>`，返回其引用。

**`file_sink()`**

追加一个 `FileSinkConfig<TransportConfig>`。

**`sink(unique_ptr<ISinkConfig>)`**

追加用户提供的 Sink config。`unique_ptr` 被 move 进 `sinks_`。

**后置条件**：`sinks_` 增长一个。允许多个 Sink；每次 flush 都会写入全部。

**边界情况**

- 一个 `TransportConfig` 可以有零个 Sink。`start()` 仍成功，`InnerBackend::add_entity` 创建一个没有 wrapper 的 Transport。格式化后的字节被丢弃。
- `sink(...)` 返回 `TransportConfig&`（不是子 builder），因此终止子 builder 链并回到 Transport 层。

#### 3.3.3 `kind`

```cpp
auto kind(interface::ITransport::Kind kind) -> TransportConfig &;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `kind` | `Kind` | `Sync`（默认）或 `Async`。 |

**行为**：存储 `kind_`。`start()` 调用 `InnerBackend::add_entity` 时读取。

**边界情况**：`start()` 之后修改 `kind_` 无效——Transport 已创建。

#### 3.3.4 `max_cache_size`

```cpp
auto max_cache_size(size_t max_cache_size) -> TransportConfig &;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `max_cache_size` | `size_t` | 字节数。`0` → 使用 Transport 默认值。 |

**行为**：存储 `max_cache_size_`。在 `add_entity` 中传给 `ITransport::set_max_cache_size`。

**边界情况**：`0` 直接透传给 `set_max_cache_size`，对 `0 < MinCacheSize` 返回 `false`。所以 Transport 保持默认值。**要实际使用某个值，传一个合法的字节数。**

#### 3.3.5 `submit`

```cpp
auto submit() -> Config &;
```

返回 `config`（父 `Config`）。用于退出流式链。

#### 3.3.6 `start`

```cpp
void start();
```

**行为**

1. `auto formatter = formatter_->start();` — 物化 Formatter。**`formatter_` 为空时段错误。**
2. 对每个 Sink config：`sinks.emplace_back(sink->start());`
3. `logger::getBackend<InnerBackend>().add_entity(kind_, formatter, sinks, max_cache_size_);`

**后置条件**：创建一个 Transport 并推入 `InnerBackend`。

**前置条件**：`formatter_` 必须非空。否则未定义行为（空解引用）。

**边界情况**

- 如果 `InnerBackend::start()` 已经调用过，`add_entity` 可能返回 `false`。此时创建的 Formatter 和 Sink 在 `start()` 返回时丢弃。
- `sinks_` 可以为空；`add_entity` 接受空 Sink vector。

---

## 4. 后端

### 4.1 `lig::InnerBackend`

```cpp
class InnerBackend : public ::lig::interface::IBackend<InnerBackend> {
  public:
    friend struct ::lig::interface::IBackend<InnerBackend>;

    void start();
    void flush();
    void stop();

    auto add_entity(::lig::interface::ITransport::Kind kind,
                    std::unique_ptr<const ::lig::interface::Formatter> &formatter,
                    std::vector<std::unique_ptr<::lig::interface::Sink>> &sinks,
                    size_t max_cache_sizes = 0) -> bool;

    ~InnerBackend();
};
```

#### 4.1.1 `start()`

```cpp
void start();
```

**行为**：`if (is_start.exchange(true, acq_rel)) return;` 然后 `for (auto &t : transports_) t->start();`。

**后置条件**：每个 Transport 已启动。`is_start == true`。后续调用是 no-op。

**线程安全**：任何线程安全，但只有第一个调用方执行工作。并发调用方在第一次 exchange 后看到 `is_start == true`。

**边界情况**

- `start()` 运行后，`add_entity` 总是返回 `false`。
- `stop()` 不会重置 `is_start`，所以 `stop()` 之后再 `start()` 是 no-op。

#### 4.1.2 `flush()`

```cpp
void flush();
```

**行为**：`for (auto &t : transports_) t->flush();`。

**按 Transport kind 的效果**

- `SyncTransport`：flush **调用线程**的 `thread_local` cache。其他线程的 cache 不受影响。
- `AsyncTransport`：推入一个空 `Task`，在条件变量上阻塞直到 worker 发出信号表示已排空所有先前排队的任务并写入 Sink。

**后置条件**：所有异步队列为空，Sink 已收到数据。

**边界情况**

- `AsyncTransport::flush()` **不**保证 `fsync`。`FileSink` 的 `sync_interval` 仍然生效。
- 在 `AsyncTransport` 上 `stop()` 之后调用 `flush()` 立即返回（`AsyncTransport::flush` 内部的 `is_stop_` 检查）。

#### 4.1.3 `stop()`

```cpp
void stop();
```

**行为**：`for (auto &t : transports_) t->stop();`。

**按 Transport kind 的效果**

- `SyncTransport::stop()` 是 no-op。
- `AsyncTransport::stop()`：
  1. `if (is_stop_.exchange(true, release)) return;`
  2. `task_queue_.push(Task{});` — 唤醒 worker。

**后置条件**：worker 已收到信号。`stop()` **不** join 线程。

**边界情况**

- worker 线程在 `~AsyncTransport` 中 join，不在 `stop()` 中。
- `stop()` 之后 `from()` 仍接受任务（它们进入队列），但 worker 在当前迭代后退出；排队的任务可能被处理也可能不被处理，取决于时序。

#### 4.1.4 `add_entity`

```cpp
auto add_entity(::lig::interface::ITransport::Kind kind,
                std::unique_ptr<const ::lig::interface::Formatter> &formatter,
                std::vector<std::unique_ptr<::lig::interface::Sink>> &sinks,
                size_t max_cache_sizes = 0) -> bool;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `kind` | `Kind` | `Async` → `AsyncTransport`；其他 → `SyncTransport`。 |
| `formatter` | `unique_ptr<const Formatter>&` | move 进去；调用后变为空。 |
| `sinks` | `vector<unique_ptr<Sink>>&` | 每个 Sink 被 move 进去；vector 元素变为空。 |
| `max_cache_sizes` | `size_t` | 传给 `set_max_cache_size`。`0` → Transport 默认值。 |

**行为**

1. `if (is_start.load(acquire)) return false;`
2. `t = make_unique<AsyncTransport>()` 或 `make_unique<SyncTransport>()`。
3. `t->set_formatter(formatter)` — 把 `formatter` 交换进 Transport；`formatter` 变为空。
4. `t->set_max_cache_size(max_cache_sizes)` — 忽略返回值。
5. 对每个 Sink：`t->add_sink(sink)` — 每个 move 进一个 `SinkWrapper`。
6. `transports_.push_back(std::move(t))`。
7. 返回 `true`。

**后置条件**：追加一个新 Transport。调用方的 `formatter` 和 `sinks` 被消费。

**边界情况**

- 返回 `false` 表示"Backend 已启动"。此时 Formatter 和 Sink **不**被消费——它们留在调用方的 `unique_ptr` 中。调用方必须清理。
- `set_max_cache_size(0)` 返回 `false`；Transport 保持默认值。
- 零 Sink 的 Transport 合法；输出被静默丢弃。

#### 4.1.5 `submit_impl`（私有，通过 `IBackend::submit` 调用）

```cpp
void submit_impl(::lig::logger::Task &&task);
```

**行为**

```cpp
for (auto &t : transports_) {
    t->from(task);
}
```

每个 Transport 拿到同一个 `Task` 的 **const 引用**。`SyncTransport::from` 直接调用；`AsyncTransport::from` 拷贝进队列。各 Transport 独立格式化，可能产生不同输出（不同 Formatter）。

**边界情况**

- 单个 `AsyncTransport` 正确处理——task 不会被丢弃。
- 单个 `SyncTransport` 也正确处理——同一代码路径。
- `Task` **不**被 move 进 `from`；前端的 `Task` 在 `submit_impl` 结束时销毁。
- `transports_` 为空时 `submit_impl` 什么都不做。这发生在 `Config::start()` 没有 Transport，或每个 `TransportConfig` 都失败时。

---

## 5. 组件接口

### 5.1 `lig::interface::Sink`

```cpp
class Sink {
  public:
    struct WriteRes {
        size_t len{};
        std::error_code err{};
        operator bool() const;
    };

    virtual ~Sink() = default;
    virtual auto write(const void *data, size_t len) -> WriteRes = 0;
    virtual auto is_valid() const noexcept -> bool = 0;
    virtual void close() noexcept {};
    virtual auto open() -> bool { return true; }
};
```

#### 5.1.1 `WriteRes`

```cpp
struct WriteRes {
    size_t len{};
    std::error_code err{};
    operator bool() const;  // err.value() == 0
};
```

| 字段 | 含义 |
|---|---|
| `len` | Sink 实际接受的字节数。可能 `<` 请求长度。 |
| `err` | 错误码。默认构造（`value() == 0`）表示成功。 |

**`operator bool`**：`err.value() == 0` 时为 `true`。注意成功的零字节写有 `len == 0` 且 `operator bool == true`。

#### 5.1.2 `write`

```cpp
virtual auto write(const void *data, size_t len) -> WriteRes = 0;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `data` | `const void*` | 要写的字节。仅当 `len == 0` 时可以为空。 |
| `len` | `size_t` | 字节数。 |

**返回**：描述接受字节数和是否出错的 `WriteRes`。

**契约**

- 必须处理部分写：返回 `0 < res.len < len` 表示"接受了一部分，重试剩余"。
- 零字节写必须返回 `len == 0, err == {}`。
- 失败时必须返回非空 `err`；`len` 可以是 0，也可以是失败前已接受的字节数。

**调用方行为**（`SinkWrapper::from`）

1. 如果上一次写失败留下了 `fail_data`，先重试写 `fail_data`。
2. 调用 `write(data, len)`。
3. 如果 `!res`，调用 `sink_->open()` 并重试 `write(data + res.len, len - res.len)`。
4. 如果仍然 `!res`，把未写的尾部追加到 `fail_data`。

**线程安全**：接口不要求。一个 Sink 由一个 Transport 拥有。但 `SyncTransport` 的 `thread_local` cache 意味着**多个线程可能并发调用同一个 Sink 的 `write`**。实现必须线程安全，或明确记录限制。内置 `FileSink` 内部有 `std::mutex`；`ConsoleSink` 依赖内核 fd 锁。

#### 5.1.3 `is_valid`

```cpp
virtual auto is_valid() const noexcept -> bool = 0;
```

**返回**：Sink 可用时为 `true`。

**调用方行为**：`SinkWrapper` 间接受其影响；`open()` 返回 `false` 时不重试。

#### 5.1.4 `close`

```cpp
virtual void close() noexcept {};
```

**用途**：释放 OS 资源。Transport 关闭时调用，`FileSink::open()` 重新打开前也调用。

**默认**：no-op。

#### 5.1.5 `open`

```cpp
virtual auto open() -> bool { return true; }
```

**用途**：（重新）打开底层资源。`SinkWrapper::from` 在写失败后调用。

**返回**：成功时 `true`。

**默认**：返回 `true`（no-op）。管理真实资源的子类必须重写。

---

### 5.2 `lig::interface::ISinkConfig` / `SinkConfig<Transport>`

```cpp
struct ISinkConfig {
    virtual auto start() -> std::unique_ptr<interface::Sink> = 0;
    virtual ~ISinkConfig() = default;
};

template <typename Transport> struct SinkConfig : ISinkConfig {
    SinkConfig(Transport &transport) : transport_{transport} {}
    ~SinkConfig() override = default;
    auto submit() -> Transport & { return transport_; }

  protected:
    Transport &transport_;
};
```

**用途**：Sink 的工厂。`start()` 在 `Config::start()` 期间调用一次。

**`submit()`**：返回父 `TransportConfig&`，允许子 builder 回到 Transport 层。

**边界情况**

- `SinkConfig` 持有其 `Transport` 的引用。如果 `TransportConfig` 被移动（不应该），引用悬空。
- 每个 `Config::start()` 恰好调用一次 `start()`。调用两次创建两个 Sink 实例；第二个被丢弃，因为 `add_entity` 只保存第一个。

---

### 5.3 `lig::interface::Formatter`

```cpp
class Formatter {
  public:
    virtual void on_open(std::vector<std::byte> &buf) const {};
    virtual void format(const LogEntry &entry, std::vector<std::byte> &buf) const = 0;
    virtual void on_close(std::vector<std::byte> &buf) const {};
    virtual ~Formatter() = default;

    Status status{};
};
```

#### 5.3.1 `format`

```cpp
virtual void format(const LogEntry &entry, std::vector<std::byte> &buf) const = 0;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `entry` | `const LogEntry&` | 要格式化的消息。 |
| `buf` | `std::vector<std::byte>&` | 输出缓冲区。**追加**到它；不要清空。 |

**契约**

- 把 `entry` 的格式化表示追加到 `buf`。
- 不要清空 `buf`；Transport 可能已预填，或可能在累积多条 entry。
- 必须能从任意线程安全调用（Sync Transport 从调用 `from` 的线程调用；Async 从 worker 调用）。

**边界情况**

- `entry.body` 可能包含嵌入的 `\0`。用 `entry.body.size()`，不用 `strlen`。
- `entry.head.filename` / `funcname` 可能为空。处理 null。
- 不要在调用之后保留 `entry` 或 `buf` 的引用；两者都会被复用。

#### 5.3.2 `on_open` / `on_close`

可选钩子。内置 Transport 不调用它们；预留给需要输出头/尾的 Formatter。

#### 5.3.3 `status`

```cpp
Status status{};
```

公开成员。`format` 读取它决定输出哪些头字段。默认：全部四位启用。

**边界情况**

- 自定义 Formatter 可以自由忽略 `status`。
- `status` **不是** const；Formatter 可以在调用之间修改它（不推荐）。

---

### 5.4 `lig::interface::IFormatterConfig` / `FormatterConfig<Derived, Transport>`

```cpp
struct IFormatterConfig {
    virtual auto start() -> std::unique_ptr<const interface::Formatter> = 0;
    virtual ~IFormatterConfig() = default;
};

template <typename Derived, typename Transport>
struct FormatterConfig : IFormatterConfig {
    FormatterConfig(Transport &transport) : transport_{transport} {}
    ~FormatterConfig() override = default;

    auto submit() -> Transport & { return transport_; }

    auto timestamp(bool is_allow) -> Derived &;
    auto function_name(bool is_allow) -> Derived &;
    auto location(bool is_allow) -> Derived &;
    auto thread_id(bool is_allow) -> Derived &;

  protected:
    Transport &transport_;
    Status status_{};
};
```

**用途**：Formatter builder 的 CRTP 基类。提供四个 `Status` setter 和 `submit()`。

**`timestamp(bool)` / `function_name(bool)` / `location(bool)` / `thread_id(bool)`**

| 名称 | 类型 | 含义 |
|---|---|---|
| `is_allow` | `bool` | `true` 启用字段，`false` 禁用。 |

返回 `Derived&`，支持 `.timestamp(false).thread_id(false)` 这样的链式调用。

**`start()`**（纯虚）：物化 Formatter。由 `Config::start()` 调用一次。

**边界情况**

- `status_` 存储在基类，不在 `Derived`。`Derived::start()` 读取 `this->status_`。
- `start()` 之后调用 setter 无效——Formatter 已经构造。

---

### 5.5 `lig::interface::ITransport`

```cpp
class ITransport {
  public:
    enum struct Kind : uint8_t { Async, Sync };

    virtual ~ITransport() = default;
    virtual void stop() {};
    virtual void start() {};
    virtual void flush() = 0;
    virtual void from(const ::lig::logger::Task &) = 0;
    virtual void set_formatter(std::unique_ptr<const Formatter> &) = 0;
    virtual void add_sink(std::unique_ptr<Sink> &) = 0;
    virtual auto kind() -> Kind = 0;
    virtual auto set_max_cache_size(size_t) noexcept -> bool = 0;
};
```

#### 5.5.1 `from`

```cpp
virtual void from(const ::lig::logger::Task &) = 0;
```

| 名称 | 类型 | 含义 |
|---|---|---|
| `task` | `const Task&` | 要消费的任务。可以为空（flush 哨兵）。 |

**契约**

- `task` 非空时，用 Transport 的 Formatter 格式化并写入 Sink。
- `task` 为空时，flush 待处理状态并通知等待者。

**边界情况**

- `task` 是 `const&`；Transport 需要保留时必须拷贝。`SyncTransport` 立即调用；`AsyncTransport` 拷贝进队列。
- 空 Task 用作 flush 哨兵。无条件格式化的 Transport 会崩溃。

#### 5.5.2 `flush`

```cpp
virtual void flush() = 0;
```

**契约**

- Sync：flush 当前线程的 cache。
- Async：阻塞直到所有排队任务处理完并写入。

#### 5.5.3 `set_max_cache_size`

```cpp
virtual auto set_max_cache_size(size_t) noexcept -> bool = 0;
```

**返回**：接受则 `true`，越界则 `false`。

**范围**：由各具体 Transport 定义。内置为 `[10 KiB, 64 KiB]`。

---

### 5.6 `lig::interface::IBackend<Derived>`

```cpp
template <typename Derived> struct IBackend {
    using Base = IBackend<Derived>;

    static constexpr void verify();  // 对 submit_impl 签名的 static_assert

    void submit(::lig::logger::Task &&task);  // 转发到 Derived::submit_impl
};
```

**用途**：Backend 单例的 CRTP 基类。

**`submit(Task&&)`**：`static_cast<Derived*>(this)->submit_impl(std::move(task))`。

**边界情况**：`Derived` 必须提供 `void submit_impl(Task&&)`。`verify()` 中的 `static_assert` 在编译期检查。

---

## 6. 内置 Sink

### 6.1 `lig::component::FileSink`

```cpp
class FileSink final : public ::lig::interface::Sink {
  public:
    static constexpr size_t MaxFileSize = 128ull * 1024 * 1024;
    static constexpr size_t MinFileSize = 8ull * 1024 * 1024;

    struct Config {
        size_t max_file_size{(MaxFileSize + MinFileSize) / 2};
        size_t sync_interval{10};
        std::filesystem::path file{std::filesystem::current_path() / "lig.log"};
        std::filesystem::path dir{std::filesystem::current_path()};
    };

    FileSink(const Config &config);
    ~FileSink() final;

    auto is_valid() const noexcept -> bool final;
    auto write(const void *data, size_t len) -> WriteRes final;
    void close() noexcept final;
    auto open() -> bool final;
    auto sync() -> bool;

    auto size() const noexcept -> size_t;
    auto config() const noexcept -> Config;

    auto set_directory(const std::filesystem::path &dir) noexcept -> bool;
    auto set_file(const std::filesystem::path &file, bool change_dir = false) noexcept -> bool;
    void set_sync_interval(size_t count) noexcept;
    auto set_max_file_size(size_t size) noexcept -> bool;
};
```

**线程安全**：`FileSink` 线程安全。`write` / `open` / `close` / `sync` / `set_directory` / `set_file` / `set_sync_interval` / `set_max_file_size` / `config()` 都获取内部 `std::mutex`。`is_valid()` 和 `size()` 是无锁原子读。调用方可以从任意线程调用这些方法。

#### 6.1.1 `Config`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `max_file_size` | `size_t` | 68 MiB | 轮转阈值，字节。限定在 `[8 MiB, 128 MiB]`。 |
| `sync_interval` | `size_t` | `10` | 两次 `fsync` 之间的 `write` 次数。`0` 禁用周期性 fsync。 |
| `file` | `path` | `current_path() / "lig.log"` | 基础文件名。轮转时加 `<n>_` 前缀。 |
| `dir` | `path` | `current_path()` | 目录。`open()` 时若不存在会创建。 |

**边界情况**

- `file` 带父路径（如 `"/var/log/app.log"`）且 `change_dir = false`：`set_file` 把 `rotate_file_` 设为绝对路径，但 `get_next_file()` 用 `config_.dir` 做后续轮转。把 `dir` 设为同一父目录，或传 `change_dir = true`，否则第一次轮转会写到 `config_.dir` 而不是文件的父目录。
- `dir` 为空：`set_directory("")` 回退到 `current_path()`。
- `max_file_size` 越界：构造函数回退到 `(Min + Max) / 2`。

#### 6.1.2 `FileSink(const Config&)`

**行为**

1. `set_directory(config.dir)` — `dir` 为空时 `config_.dir = current_path()`；`dir` 存在但不是目录时 `config_.dir = current_path()`。
2. `set_file(config.file)` — `file` 是目录时返回 `false` 并回退到 `rotate_file_ = get_next_file()`；否则 `rotate_file_ = config_.file.has_parent_path() ? config_.file : config_.dir / config_.file.filename()`。
3. `set_max_file_size(config.max_file_size)` — 越界则 `config_.max_file_size = (Min + Max) / 2`。
4. `set_sync_interval(config.sync_interval)`。

**后置条件**：`config_` 持有有效配置。**文件尚未打开。** 需要显式调用 `open()`。

**边界情况**

- 构造后 `config_.file` 保留用户传入的值。`rotate_file_` 设为第一个目标文件：`set_file("app.log")` 加 `dir = "/var/log"` 得到 `rotate_file_ = "/var/log/app.log"`。第一次 `open()` 创建该文件，然后把 `rotate_file_` 推进到 `"/var/log/1_app.log"`。

#### 6.1.3 `write`

```cpp
auto write(const void *data, size_t len) -> WriteRes;
```

**行为**

```text
if (len == 0) return {};
while (file_size_ + len >= config_.max_file_size) {
    size_t left = config_.max_file_size - file_size_;
    // 写 `left` 字节
    // 写失败则返回
    // inner_open() 下一个文件
    // inner_open 失败：返回 { total_res.len, io_error }
}
// 写剩余 `len` 字节
// write_since_sync_++；等于 sync_interval 时 inner_sync()
```

**返回**：`WriteRes`，`len` = 总写入字节数。

**边界情况**

- `len == 0` 立即返回，不碰文件，零字节写永不触发轮转。
- `file_size_ + len == max_file_size` 触发轮转（`>=`，不是 `>`）。
- 轮转调用 `inner_open()`，它先 `inner_close()`（fsync + close）再打开下一个文件。
- 轮转后 `inner_open()` 失败时，返回值 `len = total_res.len`、`err = io_error`。`SinkWrapper` 看到 `!res` 并重试未写尾部（或存入 `fail_data`）。**没有数据被静默丢弃。**

#### 6.1.4 `open`

```cpp
auto open() -> bool;
```

**行为**

1. `inner_close()` 当前 fd。
2. `rotate_file_.parent_path()` 非空时，`create_directories` 它（使用 `error_code` 重载；出错返回 `false`）。
3. `open(rotate_file_.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644)`。
4. `fstat`；拒绝非普通文件。
5. 设置 `is_valid_ = true`、`file_size_ = s.st_size`。
6. `fd_ = f`。
7. **`rotate_file_ = get_next_file()`** — 推进下一次轮转的路径。

**返回**：成功时 `true`。

**边界情况**

- 文件名**单调递增**：`app.log` → `1_app.log` → `2_app.log` → `3_app.log`。`config_.file` 从不修改，所以前缀不会嵌套。
- `get_next_file()` 每次都递增 `rotate_count_`。
- `rotate_file_.parent_path()` 为空（相对文件名如 `"app.log"`）时，跳过 `create_directories`，`open` 在当前工作目录运行。

#### 6.1.5 `close`

```cpp
void close() noexcept;
```

**行为**：获取 mutex，然后 `inner_close()`。`inner_close` 调用 `inner_sync()`、`close(fd_)`、设置 `is_valid_ = false`、`fd_ = -1`。

**后置条件**：fd 释放，`is_valid_ == false`。

**边界情况**：幂等。已关闭时调用安全。

#### 6.1.6 `sync`

```cpp
auto sync() -> bool;
```

**行为**：获取 mutex，然后 `inner_sync()`。`inner_sync` 执行 `fsync` 并重置 `write_since_sync_`。未打开时返回 `false`。

**边界情况**：未打开时返回 `false`。

#### 6.1.7 `size` / `config`

```cpp
auto size() const noexcept -> size_t;   // 无锁原子读
auto config() const noexcept -> Config; // 获取 mutex，返回拷贝
```

- `size()` 返回 `file_size_.load(acquire)`。无锁，任意线程安全。
- `config()` 获取 mutex 并返回 `config_` 的**拷贝**。调用方可自由修改拷贝；不影响 Sink。

#### 6.1.8 `set_directory`

```cpp
auto set_directory(const std::filesystem::path &dir) noexcept -> bool;
```

**行为**

- `dir.empty()` → `config_.dir = current_path()`，返回 `true`。
- `dir` 存在但不是目录 → 返回 `false`。
- 否则 `config_.dir = dir`，返回 `true`。

获取 mutex。

#### 6.1.9 `set_file`

```cpp
auto set_file(const std::filesystem::path &file, bool change_dir = false) noexcept -> bool;
```

**行为**

- `file` 是目录 → 返回 `false`。
- 否则：
  - `config_.file = file`
  - `rotate_count_ = 0`
  - `change_dir` 为真时 `config_.dir = file.parent_path()`。
  - `rotate_file_ = config_.file.has_parent_path() ? config_.file : config_.dir / config_.file.filename()`。

获取 mutex。重置轮转计数。

**边界情况**

- `set_file("app.log")` → `rotate_file_ = config_.dir / "app.log"`。第一次 `open()` 创建 `<dir>/app.log`。
- `set_file("/var/log/app.log", /*change_dir=*/true)` → `config_.dir = "/var/log"`、`rotate_file_ = "/var/log/app.log"`。轮转到 `/var/log/1_app.log`、`/var/log/2_app.log`、...
- `set_file("/var/log/app.log", /*change_dir=*/false)` → `rotate_file_ = "/var/log/app.log"`，但 `config_.dir` 不变。第一次轮转到 `<config_.dir>/1_app.log`，不是 `/var/log/1_app.log`。**如果文件带父路径，传 `change_dir = true` 或显式设置 `dir`。**

#### 6.1.10 `set_sync_interval`

```cpp
void set_sync_interval(size_t count) noexcept;
```

设置 `config_.sync_interval = count`。`0` 禁用周期性 fsync。获取 mutex。

#### 6.1.11 `set_max_file_size`

```cpp
auto set_max_file_size(size_t size) noexcept -> bool;
```

**行为**：`size > MaxFileSize || size < MinFileSize` 时返回 `false`。否则设置 `config_.max_file_size = size` 并返回 `true`。获取 mutex。

#### 6.1.12 `get_next_file`（私有）

```cpp
auto get_next_file() -> std::filesystem::path;  // 无 noexcept
```

递增 `rotate_count_` 并返回 `config_.dir / "<rotate_count>_<config_.file.filename()>"`。

**边界情况**：不是 `noexcept`——`std::format` 和 `std::filesystem::path` 构造在 OOM 时可能抛 `std::bad_alloc`。异常传播给 `inner_open()`，它在 mutex 下被调用但不 catch；`SinkWrapper` 会在下一次 `write` 时看到失败。

---

### 6.2 `lig::component::ConsoleSink`

```cpp
class ConsoleSink final : public ::lig::interface::Sink {
  public:
    struct Config { bool to_err{false}; };

    ConsoleSink(const Config &config);
    ~ConsoleSink() final;

    auto is_valid() const noexcept -> bool final;
    auto write(const void *data, size_t len) -> WriteRes final;
    void close() noexcept final;
};
```

#### 6.2.1 `Config`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `to_err` | `bool` | `false` | `true` → stderr（fd 2），`false` → stdout（fd 1）。 |

#### 6.2.2 构造

**行为**：POSIX 上 `fd_ = to_err ? 2 : 1`；Windows 上 `GetStdHandle(STD_ERROR_HANDLE | STD_OUTPUT_HANDLE)`。

**后置条件**：`is_valid() == true`（假定 fd 1 或 2 有效）。

#### 6.2.3 `write`

在循环中调用 `write(fd_, data, len)`，处理 `EINTR` 和部分写。

#### 6.2.4 `is_valid`

POSIX 上返回 `fd_ == 1 || fd_ == 2`。

#### 6.2.5 `close`

设置 `fd_ = -1`。**不**关闭底层 fd。

**边界情况**：`close()` 之后 `write` 返回 `EBADF`。`is_valid` 返回 `false`。

---

### 6.3 工厂函数与 builder

```cpp
auto createFileSink(const component::FileSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;
auto createConsoleSink(const component::ConsoleSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;
```

返回 owning `unique_ptr<Sink>`。

```cpp
template <typename Transport>
struct FileSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    FileSinkConfig(Transport &config);
    auto filename(const std::filesystem::path &file) -> FileSinkConfig &;
    auto sync_interval(size_t count) -> FileSinkConfig &;
    auto dir(const std::filesystem::path &dir) -> FileSinkConfig &;
    auto max_file_size(size_t file_size) -> FileSinkConfig &;
    auto start() -> std::unique_ptr<interface::Sink> final;
};

template <typename Transport>
struct ConsoleSinkConfig final : ::lig::interface::SinkConfig<Transport> {
    ConsoleSinkConfig(Transport &transport);
    auto to_err(bool is_err) -> ConsoleSinkConfig &;
    auto start() -> std::unique_ptr<interface::Sink> final;
};
```

每个 setter 返回 `*this` 支持链式。`start()` 物化 Sink。

---

## 7. 内置 Formatter

### 7.1 `lig::component::TextFormatter`

```cpp
class TextFormatter final : public ::lig::interface::Formatter {
  public:
    struct Config { ::lig::Status status{}; };

    TextFormatter() = default;
    TextFormatter(const Config &config);

    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
};
```

**输出格式**

```
<timestamp> - [<level>] <thread_id> (<file>:<line>) <funcname><body>
```

每个字段由 `status` 控制：

| 字段 | 由谁控制 |
|---|---|
| `<timestamp> ` | `status.hasTimeStamp()` |
| `- [<level>]` | 始终 |
| ` <thread_id>` | `status.hasThreadId()` |
| ` (<file>:<line>)` | `status.hasLocation()` |
| ` <funcname>` | `status.hasFuncname()` |
| `<body>` | 始终 |

**时间戳格式**：`%Y-%m-%d %H:%M:%S`。

**缺失字段**：`filename` / `funcname` 为空 → `"<unknown>"`。

**边界情况**：不输出尾部换行。Transport 也不追加；如果 Sink 不分隔，行可能连在一起。

---

### 7.2 `lig::component::TermFormatter`

```cpp
class TermFormatter final : public ::lig::interface::Formatter {
  public:
    struct Config {
        ::lig::Status status{};
        std::array<::term_ctl::TermStr::State, 6> states{};
    };

    TermFormatter(const Config &config);

    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
};
```

**输出格式**：与 `TextFormatter` 相同，但 `[<level>]` 标签被包裹在由 `config.states[level]` 派生的 ANSI SGR 转义中。

**构造**：预计算 `styleStrs_[i]`（每级一个）和 `clearStr_`。

**无默认构造函数**：`TermFormatter() = default` 被有意移除。必须传 `Config`，哪怕为空。

**边界情况**

- `Config{}` 产生的输出与 `TextFormatter` 相同（所有转义字符串都是 `"\033[0m"`）。
- 输出到文件或非 ANSI 终端会显示乱码。
- `styleStrs_[i]` 由 `term.state(config.states[i]).snapshot()` 构建。`config.states[i]` 默认构造时，snapshot 是 `"\033[0m"`。

---

### 7.3 工厂函数与 builder

```cpp
auto createTextFormatter(const component::TextFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;
auto createTermFormatter(const component::TermFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;
```

```cpp
template <typename Transport>
struct TextFormatterConfig final
    : ::lig::interface::FormatterConfig<TextFormatterConfig<Transport>, Transport> {
    TextFormatterConfig(Transport &config);
    auto start() -> std::unique_ptr<const interface::Formatter> final;
};

template <typename Transport>
struct TermFormatterConfig final
    : ::lig::interface::FormatterConfig<TermFormatterConfig<Transport>, Transport> {
    TermFormatterConfig(Transport &config);
    template <Level level> auto state(const ::term_ctl::TermStr::State &state);
    auto start() -> std::unique_ptr<const interface::Formatter> final;
};
```

`TermFormatterConfig::state<Level>(state)` 为一个级别设置 `TermStr::State`。

---

## 8. 内置 Transport

### 8.1 `lig::component::SyncTransport`

```cpp
class SyncTransport final : public ::lig::interface::ITransport {
  public:
    static constexpr size_t MinCacheSize = 10ull * 1024;
    static constexpr size_t MaxCacheSize = 64ull * 1024;

    SyncTransport(size_t max_cache_size = 0);
    ~SyncTransport() final;

    auto kind() -> Kind final { return Kind::Sync; };

    void flush() final;
    void from(const ::lig::logger::Task &) final;

    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &) final;
    void add_sink(std::unique_ptr<::lig::interface::Sink> &) final;

    auto set_max_cache_size(size_t size) noexcept -> bool final;
};
```

#### 8.1.1 构造

**参数**：`max_cache_size` — 字节数。`0` 表示"使用默认值"。

**行为**：`set_max_cache_size(max_cache_size)`。如果 `0`（`< MinCacheSize`），调用返回 `false`，`max_cache_size_` 保持默认 `(Min + Max) / 2 = 37 KiB`。

**后置条件**：可接受 `from` 调用。`thread_local` cache 为空。

#### 8.1.2 `from`

```cpp
void from(const ::lig::logger::Task &task);
```

**行为**

1. `auto entity = find_entity();` — 定位或创建调用线程的 `Entity`（持有 `cache`）。
2. `task(formatter_.get(), entity->cache);` — 调用 task；它格式化进 cache。
3. 如果 `entity->cache.size() > max_cache_size_`，调用 `flush()`。

**边界情况**

- `task` 为空：`Task::operator()` 在 debug 下 assert（release 下崩溃）。`SyncTransport` **不**防御空 task；只有 `AsyncTransport` 把空 task 用作 flush 哨兵。
- 多线程：每个线程在 `thread_local entities_` 中有自己的 `Entity`。线程之间无同步；写共享 Sink 可能交错。内置 `FileSink` 内部有锁；`ConsoleSink` 依赖内核 fd 锁。
- `formatter_` 为空：段错误。`add_entity` 总是设置它，所以只有直接构造 `SyncTransport` 并忘记调用 `set_formatter` 时才会发生。

#### 8.1.3 `flush`

```cpp
void flush();
```

**行为**：`auto entity = find_entity(); if (!cache.empty()) { for (auto &w : wrappers_) w->from(cache); cache.clear(); }`。

**后置条件**：调用线程的 cache 写入所有 Sink 并清空。

**边界情况**：其他线程的 cache 不受影响。要 flush 全部，从每个日志线程调用 `flush`，或用 `AsyncTransport`。

#### 8.1.4 `find_entity`（私有）

在 `thread_local entities_` 中按 `instance == this` 匹配查找调用线程的 `Entity`。不存在则创建。`Entity` 是 `shared_ptr`，跨调用存活。

#### 8.1.5 `set_formatter` / `add_sink`

`set_formatter` 与参数交换 `formatter_`；参数变为空。`add_sink` 把 Sink 包装进 `SinkWrapper` 推入 `wrappers_`。

**前置条件**：在任何 `from` / `flush` 之前调用。不强制检查。

#### 8.1.6 `set_max_cache_size`

```cpp
auto set_max_cache_size(size_t size) noexcept -> bool;
```

**返回**：`size > MaxCacheSize || size < MinCacheSize` 时 `false`。否则设置并返回 `true`。

**边界情况**：`0` 返回 `false`，大小不变。

---

### 8.2 `lig::component::AsyncTransport`

```cpp
class AsyncTransport final : public ::lig::interface::ITransport {
  public:
    static constexpr size_t DataQueueSize = 1024;
    static constexpr size_t MaxTaskSize = 512;
    static constexpr size_t MinCacheSize = 10ull * 1024;
    static constexpr size_t MaxCacheSize = 64ull * 1024;

    AsyncTransport() = default;
    ~AsyncTransport() final;

    auto kind() -> Kind final { return ITransport::Kind::Async; };

    void stop() final;
    void flush() final;
    void start() final;
    void from(const ::lig::logger::Task &) final;

    void set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) final;
    void add_sink(std::unique_ptr<::lig::interface::Sink> &sink) final;

    auto set_max_cache_size(size_t size) noexcept -> bool final;
};
```

#### 8.2.1 内部状态

- `task_queue_`：`nlqueue::VyukovMpmc<Task, 512, Strategy::Wait>`。
- `data_buffer_`：`DataBuffer`，把格式化字节合并成 2 KiB 块。
- `cache_`：`std::vector<std::byte>`，累积格式化字节直到 `max_cache_size_`。
- `overflow_mutex_` + `overflow_data_`：3 ms 内无法入队的任务走这里。
- `is_stop_`：`atomic_bool`，由 `stop()` 设置。
- `flush_notifier_`：`atomic_size_t` + `std::atomic::wait`，供 `flush()` 使用。
- `t_`：worker 线程。

#### 8.2.2 `start`

```cpp
void start();
```

**行为**：spawn 一个 worker 线程运行：

```cpp
while (!is_stop_.load(relaxed)) {
    // 预留 cache_ 到 max_cache_size_
    auto tasks = task_queue_.pop_till_empty();
    for (auto &task : tasks) {
        if (!task) {
            // flush 哨兵：drain data_buffer_，cache_flush，notify
        } else {
            tmp_data.clear();
            task(formatter_.get(), tmp_data);
            push(tmp_data);
        }
    }
    if (is_overflow_.exchange(false)) {
        std::lock_guard guard{overflow_mutex_};
        push(overflow_data_);
        overflow_data_.clear();
    }
}
// 排空剩余任务
// flush data_buffer_，cache_flush，notify
```

**后置条件**：worker 运行中。

**边界情况**

- 调用两次 `start()` 会 spawn 两个 worker。两者消费同一个队列。不支持；不要这样做。
- `start()` 不检查 `is_stop_`。`stop()` 之后调用 `start()` 会 spawn 一个立即看到 `is_stop_ == true` 并退出的 worker。

#### 8.2.3 `from`

```cpp
void from(const ::lig::logger::Task &task);
```

**行为**

```cpp
auto now = steady_clock::now();
while (!task_queue_.try_push(task) && task) {
    if (steady_clock::now() - now > 3ms) {
        // overflow 路径
        std::vector<std::byte> tmp;
        tmp.reserve(512);
        task(formatter_.get(), tmp);
        std::lock_guard guard{overflow_mutex_};
        overflow_data_.insert(overflow_data_.end(), tmp.begin(), tmp.end());
        is_overflow_.store(true, release);
        break;
    }
    std::this_thread::yield();
}
```

**后置条件**：task 要么在队列里，要么在 overflow 缓冲区里。

**边界情况**

- **空 task**：`&& task` 守卫阻止走 overflow 路径。空 task 会被无条件 push（循环条件 `!try_push(task) && task` 对空 task 在第一次 `!try_push` 时退出，不进入循环体）。这是一个微妙但正确的确保 flush 哨兵总能通过的方式。
- **3 ms 超时**：持续过载时，前端延迟从 ns 跳到 µs（format + mutex + insert）。这是设计使然；防止无限阻塞。
- **overflow 缓冲区**：无界。如果 worker 永远追不上，`overflow_data_` 无限增长。当前未做限制。

#### 8.2.4 `flush`

```cpp
void flush();
```

**行为**

```cpp
if (is_stop_.load(acquire)) return;
auto cur = flush_notifier_.load(acquire);
task_queue_.push(Task{});
flush_notifier_.wait(cur);
```

**后置条件**：在空哨兵之前排队的任务全部格式化并写入 Sink。

**边界情况**

- **空 task push 可能阻塞**：`task_queue_.push(Task{})` 是 `VyukovMpmc` 的阻塞版本（`Strategy::Wait` 的 futex 退让）。如果队列长时间满，`flush` 阻塞。
- **stop 之后**：立即返回不等待。
- **不 fsync**：`FileSink` 的 `sync_interval` 仍控制持久性。

#### 8.2.5 `stop`

```cpp
void stop();
```

**行为**：`if (is_stop_.exchange(true)) return; task_queue_.push(Task{});`。

**后置条件**：worker 将在排空队列并处理空哨兵后退出。

**边界情况**

- 不 join。`~AsyncTransport` join。
- 任何线程安全。

#### 8.2.6 `set_max_cache_size`

同 `SyncTransport`：`[MinCacheSize, MaxCacheSize]`，越界返回 `false`。

#### 8.2.7 `~AsyncTransport`

**行为**：`stop(); if (t_.joinable()) t_.join();`。

**后置条件**：worker 线程已退出；待处理任务已 flush（如果队列能排空）或丢弃。

**边界情况**：`stop` 之后调用 `from`，任务仍在队列里。worker 退出前排空它们。

---

## 9. 内部组件

以下位于 `lig::component::internal`。是实现细节，但 `SinkWrapper` 的行为对用户可见，因此记录。

### 9.1 `lig::component::internal::Data`

```cpp
struct Data {
    Data() = default;
    Data(std::byte *data, size_t size);
    ~Data();
    Data(const Data &) = delete;
    auto operator=(const Data &) = delete;
    Data(Data &&other) noexcept;
    auto operator=(Data &&other) noexcept -> Data &;
    auto data() const noexcept -> const std::byte *;
    auto size() const noexcept -> size_t;
};
```

**用途**：`new[]` 分配的字节数组的拥有包装。

**行为**

- `Data(byte*, size_t)` 接管所有权。
- `~Data()` 调用 `delete[] data_`。
- move 转移所有权；源变为 `{nullptr, 0}`。
- 拷贝被删除。

---

### 9.2 `lig::component::internal::DataBuffer`

```cpp
class DataBuffer {
  public:
    static constexpr size_t MaxBatchSize = 8ull * 1024;
    static constexpr size_t MinBatchSize = 1ull;

    DataBuffer();
    ~DataBuffer();
    DataBuffer(DataBuffer &&other) noexcept;
    auto operator=(DataBuffer &&other) noexcept -> DataBuffer &;
    DataBuffer(const DataBuffer &) = delete;
    auto operator=(const DataBuffer &) -> DataBuffer & = delete;

    [[nodiscard]] auto flush() -> std::optional<Data>;
    [[nodiscard]] auto push(const std::byte *data, size_t len) -> std::vector<Data>;
    auto len() const noexcept -> size_t;
    auto batchSize() const noexcept -> size_t;
    auto setBatchSize(size_t size) -> bool;
};
```

**默认 `batch_size_`**：`2048` 字节。

**`push(data, len)`**

- 把 `len` 字节追加到内部 buffer。
- 每次 buffer 达到 `batch_size_`，emit 一个完整 `Data` 并分配新 buffer。
- 返回 emit 的 `Data` 块（可能为空）。

**`flush()`**

- 内部 buffer 非空时，emit 为一个 `Data`。
- 空时返回 `std::nullopt`。

**`setBatchSize(size)`**

- `size < MinBatchSize`、`size > MaxBatchSize` 或 `size <= len_` 时返回 `false`。
- 否则重新分配、拷贝现有 `len_` 字节、更新 `batch_size_`。

**边界情况**

- `setBatchSize` 不能缩小到小于当前已用字节数。
- `flush` 不重置 `batch_size_`。

---

### 9.3 `lig::component::internal::SinkWrapper`

```cpp
class SinkWrapper {
  public:
    static constexpr size_t MaxFailDataSize = 128ull * 1024;

    SinkWrapper(std::unique_ptr<::lig::interface::Sink> &sink);
    ~SinkWrapper();
    SinkWrapper(const SinkWrapper &) = delete;
    auto operator=(const SinkWrapper &) -> SinkWrapper & = delete;

    void from(const std::vector<std::byte> &datas);
    void open();
};
```

**用途**：包装 `Sink` 并处理写失败。

**构造**

接管 Sink 所有权。调用方的 `unique_ptr` 变为空。

**`from(datas)`**

1. `fail_data` 非空时先尝试写它；成功后 `erase` 已写前缀。
2. 写 `datas`：
   - 成功 → 完成。
   - 失败 → 调用 `sink_->open()`：
     - `open()` 成功 → 重试写未写尾部。
     - `open()` 失败，或重试失败 → `store_fail_data` 未写尾部。

**`store_fail_data`**（私有）

- 把数据追加到 `fail_data`。
- 如果 `fail_data` 会超过 `MaxFailDataSize`，先丢弃最旧的字节。

**边界情况**

- **尽力而为**：不保证所有数据到达 Sink。持续失败时数据被丢弃。
- `fail_data` 不跨 `SinkWrapper` 析构持久化。
- `open()` 每次失败的 `from` 最多调用一次。

---

## 10. 边界情况汇总

| 场景 | 行为 | 涉及 API |
|---|---|---|
| `Config::start()` 调用两次 | 第二次创建 Formatter/Sink，然后通过 `add_entity == false` 丢弃。无泄漏，浪费。 | `Config::start` |
| `TransportConfig` 无 Formatter | `formatter_->start()` 解引用空指针 → 段错误。 | `TransportConfig::start` |
| `Log::operator()` 时 `g_level > L` | 直接返回，不构造 `Task`。 | `Log::operator()` |
| 空 `Task` 调用 `operator()` | debug 下 `assert(vtable_)`；release 下 UB。 | `Task::operator()` |
| `Task` 捕获 > 128 字节 | 堆 `ControlBlock` + 引用计数。拷贝廉价（引用计数递增）。 | `Task` 构造 / 拷贝 |
| `SyncTransport::flush()` | 只 flush 调用线程的 cache。 | `SyncTransport::flush` |
| `AsyncTransport::from` 队列满 3 ms | Overflow 路径：内联格式化 + mutex + insert。延迟跳到 µs。 | `AsyncTransport::from` |
| `AsyncTransport::flush()` | 阻塞到队列排空；不 fsync。 | `AsyncTransport::flush` |
| `FileSink::write` 时 `len == 0` | 立即返回；不轮转，不写。 | `FileSink::write` |
| `FileSink::write` 时 `file_size + len == max_file_size` | 触发轮转（`>=`，不是 `>`）。 | `FileSink::write` |
| `FileSink::write` 轮转 `open` 失败 | 返回 `{len = partial, err = io_error}`；`SinkWrapper` 重试。 | `FileSink::write` |
| `FileSink::open()` | 推进 `rotate_file_`（不是 `config_.file`）。文件名单调递增（`1_app.log`、`2_app.log`、...）。 | `FileSink::open` |
| `FileSink::set_file` 绝对路径 + `change_dir = false` | 第一次轮转写到 `config_.dir` 而不是文件的父目录。传 `change_dir = true` 或显式设 `dir`。 | `FileSink::set_file` |
| `FileSink::set_directory("")` | 回退到 `current_path()`。 | `FileSink::set_directory` |
| `FileSink` 并发 `write` | 安全。内部 `std::mutex` 串行化所有修改操作。 | `FileSink::write` |
| `FileSink::is_valid()` / `size()` | 无锁原子读；任意线程安全。 | `FileSink::is_valid` / `size` |
| `DataBuffer::setBatchSize(size <= len_)` | 返回 `false`；不变。 | `DataBuffer::setBatchSize` |
| `SinkWrapper` 持续写失败 | `fail_data` 增长到 128 KiB，然后丢最旧。 | `SinkWrapper::from` |
| `AsyncTransport` 中的空 `Task` | Flush 哨兵。`&& task` 守卫阻止走 overflow 路径。 | `AsyncTransport::from` |
| `InnerBackend::flush()` 在 `stop()` 之后 | `AsyncTransport::flush` 立即返回。 | `InnerBackend::flush` |
| `ConsoleSink::close()` 后 `write` | 返回 `EBADF`。 | `ConsoleSink::write` |
| `TermFormatter` 默认构造 | 已移除；必须传 `Config{}`。 | `TermFormatter` 构造 |
| `Level` 越界传给 `get_level_string` | UB（数组越界）。 | `get_level_string` |
| `Status` 位 4..7 置位 | 所有内置 Formatter 忽略。 | `Status` |
| `LogHead::filename == nullptr` | 内置 Formatter 输出 `"<unknown>"`。 | `TextFormatter` / `TermFormatter` |
| `Log` 对象跨调用复用 | 报告构造点位置，不是调用点。 | `Log` 构造 |
