# liglog API Reference

Complete reference for every public type and function in `liglog`.

- Entry header: `lig.hpp`
- Namespaces: `lig`, `lig::interface`, `lig::logger`, `lig::component`, `lig::component::internal`
- Standard: C++20 (`std::source_location`, `std::format`, `std::span`, `std::atomic::wait`)

Notation used below:

- **Precondition** — must hold before the call; violating it is undefined behaviour.
- **Postcondition** — guaranteed after a successful call.
- **Thread safety** — which threads may call the function concurrently.
- **Edge cases** — inputs that are legal but produce surprising results.

---

## Table of Contents

1. [Core types](#1-core-types)
2. [Front-end API](#2-front-end-api)
3. [Configuration API](#3-configuration-api)
4. [Backend](#4-backend)
5. [Component interfaces](#5-component-interfaces)
6. [Built-in sinks](#6-built-in-sinks)
7. [Built-in formatters](#7-built-in-formatters)
8. [Built-in transports](#8-built-in-transports)
9. [Internal components](#9-internal-components)
10. [Edge-case summary](#10-edge-case-summary)

---

## 1. Core types

### 1.1 `lig::Level`

```cpp
enum struct Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Fatal };
```

**Underlying type**: `uint8_t`. Numeric order matters: `Trace < Debug < Info < Warn < Error < Fatal`.

**Precondition**: only values `0..5` are valid. Passing anything else into `get_level_string` or into `Log<L, BE>` is undefined behaviour.

**Filtering rule** (used by the front-end): a message at level `L` is emitted only when `L >= g_level`. Comparison is done on the numeric value.

**Thread safety**: value type, freely copyable across threads.

---

### 1.2 `lig::get_level_string`

```cpp
auto get_level_string(Level level) -> std::string_view;
```

**Parameters**

| Name | Type | Meaning |
|---|---|---|
| `level` | `Level` | One of the six enum values. |

**Returns**: a `std::string_view` pointing to a static string literal (`"Trace"`, `"Debug"`, `"Info"`, `"Warn"`, `"Error"`, `"Fatal"`). The view is valid for the lifetime of the process.

**Precondition**: `level` must be in `[Level::Trace, Level::Fatal]`. An out-of-range value indexes outside the internal array and is UB.

**Thread safety**: pure function, safe from any thread.

**Edge cases**: never returns an empty view for valid inputs.

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

**Purpose**: tells a `Formatter` which optional header fields to emit. The formatter reads the bits; it is free to ignore them.

**Constructor `Status(uint8_t mask)`**

| Name | Type | Meaning |
|---|---|---|
| `mask` | `uint8_t` | Bit pattern of enabled fields. Bits `0..3` are meaningful; bits `4..7` are ignored. |

**Default constructor**: `mask_ = 0b1111` (all four fields enabled).

**`hasTimeStamp()` / `hasThreadId()` / `hasFuncname()` / `hasLocation()`**

Return `true` iff the corresponding bit is set. `const`, `noexcept`, no side effects.

**`setTimeStamp(bool)` / `setThreadId(bool)` / `setFuncname(bool)` / `setLocation(bool)`**

| Name | Type | Meaning |
|---|---|---|
| `enable` | `bool` | `true` sets the bit, `false` clears it. |

Returns `*this`, so calls can be chained:

```cpp
status.setTimeStamp(false).setThreadId(false);
```

**Thread safety**: plain value type. Each thread should operate on its own copy; sharing a `Status` across threads without synchronization is a data race.

**Edge cases**

- `Status(0xFF)` sets all four bits and ignores bits `4..7`.
- A formatter that ignores `status` (e.g. a custom one) will emit the same output regardless.

**Interaction with `LogHead`**: `Status` does **not** control whether the fields are captured. `LogHead` always carries `level`, `line`, `thread_id`, `filename`, `funcname`, and `now`. `Status` only controls which of them the formatter writes to the output.

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

**Field-by-field**

| Field | Type | Source | Lifetime |
|---|---|---|---|
| `level` | `Level` | template parameter `L` of `Log<L, BE>` | value |
| `line` | `uint32_t` | `std::source_location::line()` at `Log` construction | value |
| `thread_id` | `size_t` | `pthread_self()` or `GetCurrentThreadId()`, cached per-thread | value |
| `filename` | `const char*` | `std::source_location::file_name()` | non-owning, static storage |
| `funcname` | `const char*` | `std::source_location::function_name()` | non-owning, static storage |
| `now` | `system_clock::time_point` (ms precision) | captured in `Log::operator()` | value |

**Precondition**: `filename` and `funcname` may be `nullptr` if the platform cannot supply source location info. Built-in formatters substitute `"<unknown>"`.

**Ownership**: `filename` / `funcname` point into static storage owned by the runtime. **Do not free, modify, or store past process exit.** Copying a `LogHead` is safe; the pointers remain valid.

**Time semantics**: `now` is captured on the **front-end thread** at the moment `Log::operator()` runs. In async mode, the transport may format the entry several milliseconds (or seconds, under overload) later. If you need the transport-side timestamp, capture it inside your formatter.

**Thread safety**: read-only after construction; safe to copy across threads.

---

### 1.5 `lig::LogEntry`

```cpp
struct LogEntry {
    LogHead head{};
    std::string body{};
};
```

**Fields**

- `head` — metadata captured by the front-end.
- `body` — the message text after `std::vformat` has substituted the arguments.

**Lifecycle**

- Created inside the `Task` lambda (one `thread_local LogEntry` per thread, reused across calls).
- `entry.body.clear()` is called before formatting; `entry.head = info` reassigns the header.
- `formatter->format(entry, data)` is invoked with `entry` and a `std::vector<std::byte>` output buffer.
- After `format` returns, the entry is reused for the next message.

**Precondition**: `formatter` passed to `Task::operator()` must be non-null and must be the same formatter the transport was configured with.

**Edge cases**

- `body` may contain embedded `\0`. `std::string` handles it; the formatter must use `body.size()`, not `strlen`.
- A custom formatter must not retain a reference to `entry` or `entry.body` past the `format` call — the storage is reused.

---

## 2. Front-end API

### 2.1 `lig::logger::g_level`

```cpp
inline std::atomic<::lig::Level> g_level{};
```

**Purpose**: process-wide minimum log level. Every `Log::operator()` reads it with `memory_order_relaxed`.

**Default**: `Level::Trace` (0). All messages are emitted.

**Writes**: `Config::level()` stores into it. Any thread may store at any time; the change is eventually visible to all front-end threads.

**Reads**: one relaxed load per log call, ~1–2 ns on x86.

**Precondition**: the stored value should be in `[Level::Trace, Level::Fatal]`. Out-of-range values do not crash, but the numeric comparison becomes meaningless.

**Edge cases**

- Setting `g_level = Level::Fatal` drops everything below `Fatal`. `Fatal` itself still goes through.
- `LIG_LOGGER_LEVEL` (compile-time) is applied **before** `g_level`. If both filter, the message is dropped.
- Not per-transport: all transports share the same `g_level`.

---

### 2.2 `lig::logger::getThreadId`

```cpp
inline auto getThreadId() -> uint64_t;
```

**Returns**: the platform thread identifier as an integer.

- On POSIX: `static_cast<uint64_t>(pthread_self())`.
- On Windows: `static_cast<uint64_t>(GetCurrentThreadId())`.

**Caching**: `Log::operator()` stores the result in a `thread_local` variable, so the underlying call happens once per thread.

**Thread safety**: safe from any thread.

**Edge cases**

- The value is platform-specific and not portable across processes.
- The same numeric value may be reused after a thread exits.
- If the platform pthread type is larger than 64 bits (rare), truncation occurs; POSIX guarantees it fits.

---

### 2.3 `lig::logger::getBackend<BE>()`

```cpp
template <typename BE> inline auto getBackend() -> BE &;
```

**Template parameter**

| Name | Meaning |
|---|---|
| `BE` | Backend type. Must derive from `lig::interface::IBackend<BE>` and provide `void submit_impl(::lig::logger::Task &&)`. |

**Returns**: a reference to a function-local `static BE` instance. Constructed on first call, destroyed at program exit (static destruction order).

**Precondition**: `BE` must satisfy the `IBackend` CRTP contract. `IBackend<BE>::verify()` contains a `static_assert` that checks `submit_impl`'s signature.

**Thread safety**: initialisation is thread-safe (C++11 magic statics). After construction, concurrent calls return the same reference.

**Edge cases**

- Static-initialisation order: if `BE`'s constructor depends on other globals, you may hit the static init order fiasco. `InnerBackend` has a trivial constructor (only vectors and atomics), so this is safe in practice.
- Users normally never call this directly. `Log<L, BE>::operator()` and `Config::start()` use it internally.

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

**Purpose**: type-erased callable produced by the front-end. Holds captured arguments plus a pre-recorded `LogHead`. Deferred formatting happens when a transport invokes the `Task`.

#### 2.4.1 Construction

**Default constructor**

Creates an empty `Task` with `vtable_ == nullptr` and `block_ == nullptr`. `operator bool` returns `false`. Used internally as a flush sentinel.

**Template constructor `Task(F &&func)`**

| Name | Type | Meaning |
|---|---|---|
| `func` | any callable `F` | Must be invocable as `void(const Formatter*, std::vector<std::byte>&)`. Must not be a `Task`. |

SFINAE constraints (all must hold, otherwise the constructor is removed from overload resolution):

- `!std::is_same_v<std::decay_t<F>, Task>` — prevents ambiguity with copy/move ctor.
- `std::is_invocable_v<std::decay_t<F>, const Formatter*, std::vector<std::byte>&>` — signature check.
- `std::is_same_v<void, std::invoke_result_t<...>>` — return type must be `void`.

**Storage decision** (compile-time):

```cpp
if constexpr (sizeof(F) <= 128 && alignof(F) <= alignof(std::max_align_t)) {
    // inline storage in inline_[128]
} else {
    // heap ControlBlock with atomic refcount
}
```

| Case | Where `F` lives | Copy semantics | Allocation |
|---|---|---|---|
| `sizeof(F) <= 128` and alignment ok | inline in `Task` | `F`'s copy ctor called | none |
| otherwise | `new F` in `ControlBlock` | atomic refcount | `new ControlBlock`, `new F` |

**Postcondition**: `operator bool` returns `true`. `vtable_` points to `VirtualTable<F>`.

**Edge cases**

- Large captures (e.g. a `std::array<std::byte, 200>`) force the heap path. The heap path supports cheap copies (refcount bump), useful when the backend fans out to multiple transports.
- A lambda that captures a move-only type cannot be copied. The copy ctor of `Task` will fail to compile in that case; only the move ctor is usable. The front-end always moves the `Task` into `submit`, so this is fine for the normal path.

#### 2.4.2 Move constructor / move assignment

```cpp
Task(Task &&other) noexcept;
auto operator=(Task &&other) noexcept -> Task &;
```

**Behaviour**

- Inline path: calls `vtable_->move(inline_, other.inline_)`, which placement-moves `F` and destroys the source.
- Heap path: steals `block_` (refcount unchanged), sets `other.block_ = nullptr`.
- Sets `other.vtable_ = nullptr` in both cases. Source becomes empty.

**Postcondition**: source is empty (`operator bool == false`), destination holds the original callable.

**Exception safety**: `noexcept`. Move of `F` must not throw; the front-end always uses `noexcept`-safe captures (format strings, small integers, pointers).

#### 2.4.3 Copy constructor / copy assignment

```cpp
Task(const Task &other);
auto operator=(const Task &other) -> Task &;
```

**Behaviour**

- Inline path: calls `vtable_->copy(inline_, other.inline_)`, which placement-copies `F`. `F` must be copy-constructible.
- Heap path: `block_ = other.block_`, `block_->ref_.fetch_add(1, relaxed)`. Refcount shared across copies.

**Postcondition**: both `Task`s are valid; destroying one decrements the refcount.

**Edge cases**

- Copying a `Task` that captures a move-only lambda is a compile error. The front-end `submit` path uses `std::move`, so normal usage is unaffected.
- Multiple copies of a heap `Task` share the same `F` instance. If `F` has mutable state, all copies see it — the transports each invoke `F` independently, but they call into the same object.

#### 2.4.4 Destructor

```cpp
~Task();
```

**Behaviour**

- Inline path: `vtable_->destroy(inline_)`, which calls `~F()`.
- Heap path: `ref_.fetch_sub(1, acq_rel)`. If the new value is `0`, destroy `F` and `operator delete` both `block_->heap_` and `block_`.

**Edge cases**

- If the last copy lives on a transport worker thread, the destruction of `F` happens there, not on the front-end thread. Captured types should be destructible on any thread.

#### 2.4.5 `operator bool`

```cpp
explicit operator bool() const noexcept;
```

**Returns**: `vtable_ != nullptr`.

**Use**: guard against invoking an empty `Task`. `AsyncTransport` uses this as `if (!task)` to detect the flush sentinel.

**Edge cases**: the `explicit` keyword prevents implicit conversion to `bool` in conditions like `if (task)`, but `if (static_cast<bool>(task))` and `if (!!task)` work.

#### 2.4.6 `operator()`

```cpp
void operator()(const interface::Formatter *formatter,
                std::vector<std::byte> &data) const;
```

**Parameters**

| Name | Type | Meaning |
|---|---|---|
| `formatter` | `const Formatter*` | The formatter to use for rendering. Must be the transport's formatter. |
| `data` | `std::vector<std::byte>&` | Output buffer. The formatter **appends** to it (does not clear). |

**Behaviour**

1. `assert(vtable_ && "empty Task must not be invoked");` — debug builds abort on empty `Task`.
2. `vtable_->invoke(ptr(), formatter, data)` — calls the stored `F` with `ptr()` (inline storage or heap), `formatter`, `data`.

**Precondition**: `vtable_ != nullptr`, i.e. `operator bool` is `true`. `formatter` must be non-null.

**Postcondition**: `data` has grown by whatever the formatter appended.

**Thread safety**: safe to call from any single thread. Concurrent calls on the **same** `Task` are safe only if `F` is thread-safe (the front-end's lambdas are: they only write to a `thread_local LogEntry` and to the caller-supplied `data`).

**Edge cases**

- Calling on an empty `Task` in release builds is UB (`nullptr` dereference). Always check `operator bool` first.
- `data` is not cleared. Transports do `tmp_data.clear()` before invoking, but a custom transport might not.
- If `F` throws (should not, but possible), the exception propagates to the caller. `AsyncTransport`'s worker has no `try`/`catch` around it; an exception would terminate.

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

**Purpose**: front-end entry point. Captures the format string and arguments, then submits a `Task` to `BE`.

#### 2.5.1 Constructor

```cpp
Log(std::source_location &&loc = std::source_location::current());
```

| Name | Type | Meaning |
|---|---|---|
| `loc` | `std::source_location&&` | Defaults to the caller's location. Do not pass manually. |

**Behaviour**: stores `loc_`. The `source_location` is captured at `Log` construction, not at `operator()` call time.

**Edge cases**: if you hold a `Log` object across calls, every call reports the **constructor's** file/line, not the call site. Typical usage constructs `Log` at each call site (`lig::Info{}(...)`) or caches a `static thread_local` logger (in which case the reported location is where the logger was declared, which is usually not what you want).

**Recommended usage**

```cpp
// Reports the call site's location. Slightly more expensive (one source_location ctor per call).
lig::Info{}("hello");

// Reports the declaration site's location. Fast, but misleading.
static thread_local lig::Info logger{};
logger("hello");
```

#### 2.5.2 `operator()`

```cpp
template <typename... Args>
void operator()(const std::format_string<Args...> &format, Args &&...args);
```

| Name | Type | Meaning |
|---|---|---|
| `format` | `std::format_string<Args...>` | Compile-time-checked format string. |
| `args` | `Args&&...` | Arguments substituted into `format`. Each must satisfy `std::formatter`. |

**Behaviour** (in order):

1. **Compile-time filter** (`if constexpr`):
   - `LIG_LOGGER_LEVEL == 1` and `L < Info` → return.
   - `LIG_LOGGER_LEVEL == 2` and `L < Warn` → return.
   - This branch is entirely elided at compile time; no runtime check.
2. **Runtime filter**: `if (g_level.load(relaxed) > L) return;`.
3. **Capture `thread_id`**: `thread_local const auto thread_id = getThreadId();` — cached after the first call per thread.
4. **Build `LogHead`**: `level = L`, `line = loc_.line()`, `thread_id`, `filename = loc_.file_name()`, `funcname = loc_.function_name()`, `now = system_clock::now()` floored to milliseconds.
5. **Build `Task`**: a lambda capturing `info`, `fmt` (as `std::string_view` over `format.get()`), and `args...` by value.
6. **Submit**: `getBackend<BE>().submit(std::move(task))`.

**Inside the lambda** (runs on the transport thread):

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

- `LogEntry` is `thread_local` and reused; `body` is cleared each time.
- `vformat_to` performs the actual substitution.
- `formatter->format` appends the rendered output to `data`.

**Postcondition** (if the filters pass): a `Task` is submitted to `BE`. Formatting is deferred.

**Thread safety**: safe from any thread. Each call constructs a new `Task` and moves it into `submit`.

**Edge cases**

- **Argument lifetime**: `args...` are captured **by value**. `std::string` arguments are copied into the lambda (one allocation per call for heap-sized strings). Pass `std::string_view` or `const char*` if you want to avoid the copy — but then the referenced data must outlive the transport's formatting.
- **Format string lifetime**: `format.get()` returns a pointer to the `std::format_string`'s static storage. Safe to store in a `string_view`.
- **Throwing `std::format`**: if `vformat_to` throws inside the lambda (e.g. bad format spec), the exception propagates out of `Task::operator()`. On the async worker, there is no catch; the process terminates. On the sync path, it propagates to the caller of `Log::operator()`.
- **`LIG_LOGGER_LEVEL`** is read at compile time; changing it requires recompiling all TUs that include `logger.hpp`.
- **`g_level` race**: a concurrent store may be observed or not by in-flight calls. This is by design — filtering is advisory, not synchronous.

#### 2.5.3 Aliases

```cpp
template <typename BE> using Trace = Log<Level::Trace, BE>;
template <typename BE> using Debug = Log<Level::Debug, BE>;
template <typename BE> using Info  = Log<Level::Info,  BE>;
template <typename BE> using Warn  = Log<Level::Warn,  BE>;
template <typename BE> using Error = Log<Level::Error, BE>;
template <typename BE> using Fatal = Log<Level::Fatal, BE>;
```

`lig.hpp` adds aliases bound to `InnerBackend`:

```cpp
using Trace = logger::Trace<InnerBackend>;
using Debug = logger::Debug<InnerBackend>;
using Info  = logger::Info<InnerBackend>;
using Warn  = logger::Warn<InnerBackend>;
using Error = logger::Error<InnerBackend>;
using Fatal = logger::Fatal<InnerBackend>;
```

---

## 3. Configuration API

### 3.1 `lig::config()`

```cpp
inline auto config() -> Config;
```

Returns a fresh `Config` by value. Each call produces an independent builder; you can have multiple `Config` objects, but only one `start()` will take effect (see `InnerBackend::add_entity`).

**Thread safety**: safe to call from any thread. The returned `Config` must not be shared across threads.

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

| Name | Type | Meaning |
|---|---|---|
| `level` | `Level` | New global minimum level. |

**Behaviour**: `g_level.store(level, memory_order_relaxed)`. Immediate effect.

**Returns**: `*this`, for chaining.

**Edge cases**: setting `Level::Fatal` drops everything below `Fatal` for all transports. Call it before or after `start()`; both work.

#### 3.2.2 `transport()`

```cpp
auto transport() -> TransportConfig &;
```

**Behaviour**

1. `auto t = std::make_unique<TransportConfig>(*this);`
2. `transports_.push_back(std::move(t));`
3. Returns `*transports_.back()`.

**Postcondition**: the returned reference points into the vector. It remains valid until the next `transport()` call **only if** the vector doesn't reallocate. Because `transports_` is `std::vector<std::unique_ptr<TransportConfig>>`, the pointed-to `TransportConfig` lives on the heap; its address is stable regardless of vector reallocation. **The reference stays valid** for the lifetime of the `Config`.

**Edge cases**: each call creates a distinct transport. There is no "reuse" or "replace" semantics.

#### 3.2.3 `start()`

```cpp
void start();
```

**Behaviour**

```cpp
for (auto &entry : transports_) entry->start();
logger::getBackend<InnerBackend>().start();
```

Each `TransportConfig::start()` materialises the formatter and sinks, then calls `InnerBackend::add_entity`. Finally, `InnerBackend::start()` starts all transports (spawns async workers).

**Postcondition**: the backend is running; logging is active.

**Precondition** (each must hold, otherwise UB or silent misbehaviour):

- Every `TransportConfig` has a formatter set (otherwise `formatter_->start()` dereferences a null `unique_ptr` → segfault).
- Every sink builder has been `submit()`ted or the sink was added via `sink(...)`.

**Edge cases**

- **Not idempotent**: calling twice re-runs `TransportConfig::start()`. The second round creates new formatters/sinks, but `InnerBackend::add_entity` returns `false` because `is_start == true`. The freshly created objects are destroyed at the end of `TransportConfig::start()`. No leak, but wasted work.
- **`is_start_` is atomic but unused**: the private `std::atomic_bool is_start_{false}` in `Config` is never read or written. It's dead state.

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

**Purpose**: fluent builder for one transport. Every setter returns a reference to a sub-builder; `submit()` returns to `Config`.

#### 3.3.1 Formatter setters

**`text_formatter()`**

Replaces the current formatter with a `TextFormatterConfig<TransportConfig>` and returns a reference to it.

**`term_formatter()`**

Same, but `TermFormatterConfig<TransportConfig>`.

**`formatter(unique_ptr<IFormatterConfig>&)`**

Takes a user-supplied formatter config. The `unique_ptr` is `swap`ped with `formatter_`, so the caller's `unique_ptr` becomes null after the call.

**Returns** (for `text_formatter` / `term_formatter`): `*static_cast<Derived*>(formatter_.get())`.

**Precondition for `start()`**: at least one of the three must have been called. Otherwise `formatter_` is null and `start()` segfaults.

**Edge cases**

- A `TransportConfig` holds **exactly one** formatter. Calling `text_formatter()` after `term_formatter()` **replaces** the previous one; the old formatter is destroyed.
- The returned reference is valid until the next formatter setter is called (which resets `formatter_`).

#### 3.3.2 Sink setters

**`console_sink()`**

Appends a `ConsoleSinkConfig<TransportConfig>` to `sinks_` and returns a reference to it.

**`file_sink()`**

Appends a `FileSinkConfig<TransportConfig>`.

**`sink(unique_ptr<ISinkConfig>)`**

Appends a user-supplied sink config. The `unique_ptr` is moved into `sinks_`.

**Postcondition**: `sinks_` grows by one. Multiple sinks are allowed; they are all written to on every flush.

**Edge cases**

- A `TransportConfig` may have zero sinks. `start()` still succeeds, and `InnerBackend::add_entity` creates a transport with no wrappers. The formatted bytes are discarded.
- `sink(...)` returns `TransportConfig&` (not the sub-builder), so it terminates a sub-builder chain and returns to the transport level.

#### 3.3.3 `kind`

```cpp
auto kind(interface::ITransport::Kind kind) -> TransportConfig &;
```

| Name | Type | Meaning |
|---|---|---|
| `kind` | `Kind` | `Sync` (default) or `Async`. |

**Behaviour**: stores `kind_`. Read by `start()` when calling `InnerBackend::add_entity`.

**Edge cases**: changing `kind_` after `start()` has no effect — the transport was already created.

#### 3.3.4 `max_cache_size`

```cpp
auto max_cache_size(size_t max_cache_size) -> TransportConfig &;
```

| Name | Type | Meaning |
|---|---|---|
| `max_cache_size` | `size_t` | Bytes. `0` → use transport's default. |

**Behaviour**: stores `max_cache_size_`. Passed to `ITransport::set_max_cache_size` in `add_entity`.

**Edge cases**: `0` is passed through to `set_max_cache_size`, which returns `false` for `0 < MinCacheSize`. So the transport keeps its default. **To actually use a value, pass a valid byte count.**

#### 3.3.5 `submit`

```cpp
auto submit() -> Config &;
```

Returns `config` (the parent `Config`). Used to exit the fluent chain.

#### 3.3.6 `start`

```cpp
void start();
```

**Behaviour**

1. `auto formatter = formatter_->start();` — materialise the formatter. **Segfaults if `formatter_` is null.**
2. For each sink config: `sinks.emplace_back(sink->start());`
3. `logger::getBackend<InnerBackend>().add_entity(kind_, formatter, sinks, max_cache_size_);`

**Postcondition**: a transport is created and pushed into `InnerBackend`.

**Precondition**: `formatter_` must be non-null. Otherwise undefined behaviour (null deref).

**Edge cases**

- `add_entity` may return `false` if `InnerBackend::start()` has already been called. In that case the created formatter and sinks are discarded when `start()` returns.
- `sinks_` may be empty; `add_entity` accepts an empty sink vector.

---

## 4. Backend

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

**Behaviour**: `if (is_start.exchange(true, acq_rel)) return;` then `for (auto &t : transports_) t->start();`.

**Postcondition**: every transport is started. `is_start == true`. Subsequent calls are no-ops.

**Thread safety**: safe to call from any thread, but only the first caller does work. Concurrent callers see `is_start == true` after the first exchange.

**Edge cases**

- Once `start()` has run, `add_entity` always returns `false`.
- `stop()` does not reset `is_start`, so `start()` after `stop()` is a no-op.

#### 4.1.2 `flush()`

```cpp
void flush();
```

**Behaviour**: `for (auto &t : transports_) t->flush();`.

**Effect by transport kind**

- `SyncTransport`: flushes the **calling thread's** `thread_local` cache. Other threads' caches are untouched.
- `AsyncTransport`: pushes an empty `Task` and blocks on a condition variable until the worker signals it has drained all previously queued tasks and written to the sinks.

**Postcondition**: all async queues are empty and the sinks have received the data.

**Edge cases**

- `AsyncTransport::flush()` does **not** guarantee `fsync`. `FileSink`'s `sync_interval` still applies.
- Calling `flush()` after `stop()` on an `AsyncTransport` returns immediately (the `is_stop_` check inside `AsyncTransport::flush`).

#### 4.1.3 `stop()`

```cpp
void stop();
```

**Behaviour**: `for (auto &t : transports_) t->stop();`.

**Effect by transport kind**

- `SyncTransport::stop()` is a no-op.
- `AsyncTransport::stop()`:
  1. `if (is_stop_.exchange(true, release)) return;`
  2. `task_queue_.push(Task{});` — wake the worker.

**Postcondition**: the worker has been signalled. `stop()` does **not** join the thread.

**Edge cases**

- The worker thread joins in `~AsyncTransport`, not in `stop()`.
- After `stop()`, `from()` still accepts tasks (they go into the queue), but the worker will exit after the current iteration; queued tasks may be processed or not depending on timing.

#### 4.1.4 `add_entity`

```cpp
auto add_entity(::lig::interface::ITransport::Kind kind,
                std::unique_ptr<const ::lig::interface::Formatter> &formatter,
                std::vector<std::unique_ptr<::lig::interface::Sink>> &sinks,
                size_t max_cache_sizes = 0) -> bool;
```

| Name | Type | Meaning |
|---|---|---|
| `kind` | `Kind` | `Async` → `AsyncTransport`; anything else → `SyncTransport`. |
| `formatter` | `unique_ptr<const Formatter>&` | Moved in; becomes null after the call. |
| `sinks` | `vector<unique_ptr<Sink>>&` | Each sink is moved in; the vector's elements become null. |
| `max_cache_sizes` | `size_t` | Passed to `set_max_cache_size`. `0` → transport default. |

**Behaviour**

1. `if (is_start.load(acquire)) return false;`
2. `t = make_unique<AsyncTransport>()` or `make_unique<SyncTransport>()`.
3. `t->set_formatter(formatter)` — swaps `formatter` into the transport; `formatter` becomes null.
4. `t->set_max_cache_size(max_cache_sizes)` — return value ignored.
5. For each sink: `t->add_sink(sink)` — moves each into a `SinkWrapper`.
6. `transports_.push_back(std::move(t))`.
7. Return `true`.

**Postcondition**: a new transport is appended. The caller's `formatter` and `sinks` are consumed.

**Edge cases**

- Return value `false` means "backend already started". The formatter and sinks are **not** consumed in that case — they remain in the caller's `unique_ptr`s. The caller must clean up.
- `set_max_cache_size(0)` returns `false`; the transport keeps its default.
- A transport with zero sinks is legal; output is silently discarded.

#### 4.1.5 `submit_impl` (private, called via `IBackend::submit`)

```cpp
void submit_impl(::lig::logger::Task &&task);
```

**Behaviour**

```cpp
for (auto &t : transports_) {
    t->from(task);
}
```

Every transport receives a **const reference** to the same `Task`. `SyncTransport::from` invokes it directly; `AsyncTransport::from` copies it into the queue. Each transport formats independently, producing potentially different output (different formatters).

**Edge cases**

- A single `AsyncTransport` is handled correctly — the task is not dropped.
- A single `SyncTransport` is handled correctly — same code path.
- The `Task` is **not** moved into `from`; the front-end's `Task` is destroyed at the end of `submit_impl`.
- If `transports_` is empty, `submit_impl` does nothing. This happens if `Config::start()` was called without any transport, or if every `TransportConfig` failed to materialise.

---

## 5. Component interfaces

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

| Field | Meaning |
|---|---|
| `len` | Bytes actually accepted by the sink. May be `<` the requested length. |
| `err` | Error code. Default-constructed (`value() == 0`) means success. |

**`operator bool`**: `true` iff `err.value() == 0`. Note that a successful zero-byte write has `len == 0` and `operator bool == true`.

#### 5.1.2 `write`

```cpp
virtual auto write(const void *data, size_t len) -> WriteRes = 0;
```

| Name | Type | Meaning |
|---|---|---|
| `data` | `const void*` | Bytes to write. May be null only if `len == 0`. |
| `len` | `size_t` | Number of bytes. |

**Return**: a `WriteRes` describing how many bytes were accepted and whether an error occurred.

**Contract**

- Must handle partial writes: a return with `0 < res.len < len` means "accepted some, retry the rest".
- Must return `len == 0, err == {}` for a zero-byte write.
- Must return a non-empty `err` on failure; `len` may be 0 or the number of bytes accepted before the failure.

**Caller behaviour** (`SinkWrapper::from`)

1. If the previous write failed and left `fail_data`, retry writing `fail_data` first.
2. Call `write(data, len)`.
3. If `!res`, call `sink_->open()` and retry `write(data + res.len, len - res.len)`.
4. If still `!res`, append the unwritten tail to `fail_data`.

**Thread safety**: not required by the interface. A sink is owned by one transport. However, `SyncTransport`'s `thread_local` cache means **multiple threads can call `write` on the same sink concurrently**. Implementations must either be thread-safe or document the limitation.

#### 5.1.3 `is_valid`

```cpp
virtual auto is_valid() const noexcept -> bool = 0;
```

**Returns**: `true` if the sink is usable.

**Caller behaviour**: `SinkWrapper` uses this indirectly; if `open()` returns `false`, the wrapper does not retry.

#### 5.1.4 `close`

```cpp
virtual void close() noexcept {};
```

**Purpose**: release OS resources. Called by the transport on shutdown, and by `FileSink::open()` before reopening.

**Default**: no-op.

#### 5.1.5 `open`

```cpp
virtual auto open() -> bool { return true; }
```

**Purpose**: (re)open the underlying resource. Called by `SinkWrapper::from` after a write failure.

**Returns**: `true` on success.

**Default**: returns `true` (no-op). Subclasses that manage real resources must override.

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

**Purpose**: factory for a sink. `start()` is called once, during `Config::start()`.

**`submit()`**: returns the parent `TransportConfig&`, allowing a sub-builder to return to the transport level.

**Edge cases**

- A `SinkConfig` holds a reference to its `Transport`. If the `TransportConfig` is moved (it shouldn't be), the reference dangles.
- `start()` is called exactly once per `Config::start()`. Calling it twice creates two sink instances; the second is discarded because `add_entity` only stores the first.

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

| Name | Type | Meaning |
|---|---|---|
| `entry` | `const LogEntry&` | The message to format. |
| `buf` | `std::vector<std::byte>&` | Output buffer. **Append** to it; do not clear. |

**Contract**

- Append the formatted representation of `entry` to `buf`.
- Do not clear `buf`; the transport may have pre-filled it or may be accumulating multiple entries.
- Must be safe to call from any thread (Sync transport calls it from whichever thread invokes `from`; Async from the worker).

**Edge cases**

- `entry.body` may contain embedded `\0`. Use `entry.body.size()`, not `strlen`.
- `entry.head.filename` / `funcname` may be null. Handle null.
- Do not retain a reference to `entry` or `buf` past the call; both are reused.

#### 5.3.2 `on_open` / `on_close`

Optional hooks. Built-in transports do not call them; they are reserved for formatters that need to emit a header or footer.

#### 5.3.3 `status`

```cpp
Status status{};
```

Public member. Read by `format` to decide which header fields to emit. Default: all four fields enabled.

**Edge cases**

- A custom formatter is free to ignore `status`.
- `status` is **not** const; a formatter may modify it between calls (not recommended).

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

**Purpose**: CRTP base for formatter builders. Provides the four `Status` setters and `submit()`.

**`timestamp(bool)` / `function_name(bool)` / `location(bool)` / `thread_id(bool)`**

| Name | Type | Meaning |
|---|---|---|
| `is_allow` | `bool` | `true` enables the field, `false` disables. |

Returns `Derived&`, enabling chains like `.timestamp(false).thread_id(false)`.

**`start()`** (pure virtual): materialises the formatter. Called once by `Config::start()`.

**Edge cases**

- `status_` is stored in the base, not in `Derived`. `Derived::start()` reads `this->status_`.
- Calling a setter after `start()` has no effect — the formatter has already been constructed.

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

| Name | Type | Meaning |
|---|---|---|
| `task` | `const Task&` | The task to consume. May be empty (flush sentinel). |

**Contract**

- If `task` is non-empty, format it using the transport's formatter and write to the sinks.
- If `task` is empty, flush any pending state and notify waiters.

**Edge cases**

- `task` is a `const&`; the transport must copy if it needs to retain it. `SyncTransport` invokes it immediately; `AsyncTransport` copies it into the queue.
- Empty tasks are used as flush sentinels. A transport that formats unconditionally would crash.

#### 5.5.2 `flush`

```cpp
virtual void flush() = 0;
```

**Contract**

- Sync: flush the current thread's cache.
- Async: block until all queued tasks are processed and written.

#### 5.5.3 `set_max_cache_size`

```cpp
virtual auto set_max_cache_size(size_t) noexcept -> bool = 0;
```

**Returns**: `true` if the size was accepted, `false` if out of range.

**Range**: defined by each concrete transport. Built-ins use `[10 KiB, 64 KiB]`.

---

### 5.6 `lig::interface::IBackend<Derived>`

```cpp
template <typename Derived> struct IBackend {
    using Base = IBackend<Derived>;

    static constexpr void verify();  // static_assert on submit_impl's signature

    void submit(::lig::logger::Task &&task);  // forwards to Derived::submit_impl
};
```

**Purpose**: CRTP base for the backend singleton.

**`submit(Task&&)`**: `static_cast<Derived*>(this)->submit_impl(std::move(task))`.

**Edge cases**: `Derived` must provide `void submit_impl(Task&&)`. The `static_assert` in `verify()` checks this at compile time.

---

## 6. Built-in sinks

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

**Thread safety**: `FileSink` is thread-safe. `write` / `open` / `close` / `sync` / `set_directory` / `set_file` / `set_sync_interval` / `set_max_file_size` / `config()` all acquire an internal `std::mutex`. `is_valid()` and `size()` are lock-free atomic reads. Callers may invoke any of these from any thread.

#### 6.1.1 `Config`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `max_file_size` | `size_t` | 68 MiB | Rotation threshold in bytes. Clamped to `[8 MiB, 128 MiB]`. |
| `sync_interval` | `size_t` | `10` | Number of `write` calls between `fsync`s. `0` disables periodic fsync. |
| `file` | `path` | `current_path() / "lig.log"` | Base filename. Rotation prepends `<n>_`. |
| `dir` | `path` | `current_path()` | Directory. Created by `open()` if missing. |

**Edge cases**

- `file` with a parent path (e.g. `"/var/log/app.log"`) and `change_dir = false`: `rotate_file_` is set to the absolute path on `set_file`, but `get_next_file()` uses `config_.dir` for subsequent rotations. Set `dir` to the same parent, or pass `change_dir = true`, otherwise the first rotation writes to `config_.dir` instead of the file's parent.
- `dir` empty: `set_directory("")` falls back to `current_path()`.
- `max_file_size` out of range: constructor falls back to `(Min + Max) / 2`.

#### 6.1.2 `FileSink(const Config&)`

**Behaviour**

1. `set_directory(config.dir)` — if `dir` is empty, `config_.dir = current_path()`; if `dir` exists but is not a directory, `config_.dir = current_path()`.
2. `set_file(config.file)` — if `file` is a directory, return `false` and fall back to `rotate_file_ = get_next_file()`; otherwise `rotate_file_ = config_.file.has_parent_path() ? config_.file : config_.dir / config_.file.filename()`.
3. `set_max_file_size(config.max_file_size)` — if out of range, `config_.max_file_size = (Min + Max) / 2`.
4. `set_sync_interval(config.sync_interval)`.

**Postcondition**: `config_` holds the effective configuration. **The file is not opened yet.** Call `open()` explicitly.

**Edge cases**

- After construction, `config_.file` retains the user-supplied value. `rotate_file_` is set to the first target file: `set_file("app.log")` with `dir = "/var/log"` gives `rotate_file_ = "/var/log/app.log"`. The first `open()` creates that file, then advances `rotate_file_` to `"/var/log/1_app.log"`.

#### 6.1.3 `write`

```cpp
auto write(const void *data, size_t len) -> WriteRes;
```

**Behaviour**

```text
if (len == 0) return {};
while (file_size_ + len >= config_.max_file_size) {
    size_t left = config_.max_file_size - file_size_;
    // write `left` bytes
    // if write fails, return
    // inner_open() the next file
    // if inner_open fails: return { total_res.len, io_error }
}
// write the remaining `len` bytes
// increment write_since_sync_; if == sync_interval, inner_sync()
```

**Return**: `WriteRes` with `len` = total bytes written.

**Edge cases**

- `len == 0` returns immediately without touching the file, so a zero-byte write never triggers rotation.
- `file_size_ + len == max_file_size` triggers rotation (`>=`, not `>`).
- Rotation calls `inner_open()`, which calls `inner_close()` (fsync + close) and opens the next file.
- If `inner_open()` fails after rotation, the return has `len = total_res.len` and `err = io_error`. `SinkWrapper` sees `!res` and retries the unwritten tail (or stores it in `fail_data`). **No data is silently dropped.**

#### 6.1.4 `open`

```cpp
auto open() -> bool;
```

**Behaviour**

1. `inner_close()` the current fd.
2. If `rotate_file_.parent_path()` is non-empty, `create_directories` it (uses the `error_code` overload; returns `false` on error).
3. `open(rotate_file_.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644)`.
4. `fstat`; reject non-regular files.
5. Set `is_valid_ = true`, `file_size_ = s.st_size`.
6. `fd_ = f`.
7. **`rotate_file_ = get_next_file()`** — advance the path for the next rotation.

**Return**: `true` on success.

**Edge cases**

- Filenames are **monotonically increasing**: `app.log` → `1_app.log` → `2_app.log` → `3_app.log`. `config_.file` is never modified, so the prefix does not nest.
- `get_next_file()` increments `rotate_count_` each time.
- If `rotate_file_.parent_path()` is empty (relative filename like `"app.log"`), `create_directories` is skipped and `open` runs in the current working directory.

#### 6.1.5 `close`

```cpp
void close() noexcept;
```

**Behaviour**: locks the mutex, then `inner_close()`. `inner_close` calls `inner_sync()`, `close(fd_)`, sets `is_valid_ = false`, `fd_ = -1`.

**Postcondition**: fd released, `is_valid_ == false`.

**Edge cases**: idempotent. Safe to call when already closed.

#### 6.1.6 `sync`

```cpp
auto sync() -> bool;
```

**Behaviour**: locks the mutex, then `inner_sync()`. `inner_sync` does the `fsync` and resets `write_since_sync_`. Returns `false` if not open.

**Edge cases**: returns `false` if not open.

#### 6.1.7 `size` / `config`

```cpp
auto size() const noexcept -> size_t;   // lock-free atomic read
auto config() const noexcept -> Config; // takes the mutex, returns a copy
```

- `size()` returns `file_size_.load(acquire)`. No lock, safe from any thread.
- `config()` locks the mutex and returns a **copy** of `config_`. The caller may freely mutate the copy; it does not affect the sink.

#### 6.1.8 `set_directory`

```cpp
auto set_directory(const std::filesystem::path &dir) noexcept -> bool;
```

**Behaviour**

- `dir.empty()` → `config_.dir = current_path()`, return `true`.
- `dir` exists and is not a directory → return `false`.
- Otherwise `config_.dir = dir`, return `true`.

Locks the mutex.

#### 6.1.9 `set_file`

```cpp
auto set_file(const std::filesystem::path &file, bool change_dir = false) noexcept -> bool;
```

**Behaviour**

- `file` is a directory → return `false`.
- Otherwise:
  - `config_.file = file`
  - `rotate_count_ = 0`
  - If `change_dir`, `config_.dir = file.parent_path()`.
  - `rotate_file_ = config_.file.has_parent_path() ? config_.file : config_.dir / config_.file.filename()`.

Locks the mutex. Resets the rotation counter.

**Edge cases**

- `set_file("app.log")` → `rotate_file_ = config_.dir / "app.log"`. First `open()` creates `<dir>/app.log`.
- `set_file("/var/log/app.log", /*change_dir=*/true)` → `config_.dir = "/var/log"`, `rotate_file_ = "/var/log/app.log"`. Rotations go to `/var/log/1_app.log`, `/var/log/2_app.log`, ...
- `set_file("/var/log/app.log", /*change_dir=*/false)` → `rotate_file_ = "/var/log/app.log"`, but `config_.dir` is unchanged. First rotation goes to `<config_.dir>/1_app.log`, not `/var/log/1_app.log`. **Pass `change_dir = true` or set `dir` explicitly if the file has a parent path.**

#### 6.1.10 `set_sync_interval`

```cpp
void set_sync_interval(size_t count) noexcept;
```

Sets `config_.sync_interval = count`. `0` disables periodic fsync. Locks the mutex.

#### 6.1.11 `set_max_file_size`

```cpp
auto set_max_file_size(size_t size) noexcept -> bool;
```

**Behaviour**: if `size > MaxFileSize || size < MinFileSize`, return `false`. Otherwise set `config_.max_file_size = size` and return `true`. Locks the mutex.

#### 6.1.12 `get_next_file` (private)

```cpp
auto get_next_file() -> std::filesystem::path;  // no noexcept
```

Increments `rotate_count_` and returns `config_.dir / "<rotate_count>_<config_.file.filename()>"`.

**Edge cases**: not `noexcept` — `std::format` and `std::filesystem::path` construction may throw `std::bad_alloc` on OOM. The exception propagates to `inner_open()`, which catches nothing but is called under the mutex; `SinkWrapper` will see the failure on the next `write`.

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

| Field | Type | Default | Meaning |
|---|---|---|---|
| `to_err` | `bool` | `false` | `true` → stderr (fd 2), `false` → stdout (fd 1). |

#### 6.2.2 Construction

**Behaviour**: `fd_ = to_err ? 2 : 1` on POSIX; `GetStdHandle(STD_ERROR_HANDLE | STD_OUTPUT_HANDLE)` on Windows.

**Postcondition**: `is_valid() == true` (fd 1 or 2 is assumed valid).

#### 6.2.3 `write`

Calls `write(fd_, data, len)` in a loop, handling `EINTR` and partial writes.

#### 6.2.4 `is_valid`

Returns `fd_ == 1 || fd_ == 2` (POSIX).

#### 6.2.5 `close`

Sets `fd_ = -1`. Does **not** close the underlying fd.

**Edge cases**: after `close()`, `write` returns `EBADF`. `is_valid` returns `false`.

---

### 6.3 Factory functions and builders

```cpp
auto createFileSink(const component::FileSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;
auto createConsoleSink(const component::ConsoleSink::Config &config = {})
    -> std::unique_ptr<::lig::interface::Sink>;
```

Return an owning `unique_ptr<Sink>`.

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

Each setter returns `*this` for chaining. `start()` materialises the sink.

---

## 7. Built-in formatters

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

**Output format**

```
<timestamp> - [<level>] <thread_id> (<file>:<line>) <funcname><body>
```

Each field is gated by `status`:

| Field | Gated by |
|---|---|
| `<timestamp> ` | `status.hasTimeStamp()` |
| `- [<level>]` | always |
| ` <thread_id>` | `status.hasThreadId()` |
| ` (<file>:<line>)` | `status.hasLocation()` |
| ` <funcname>` | `status.hasFuncname()` |
| `<body>` | always |

**Timestamp format**: `%Y-%m-%d %H:%M:%S`.

**Missing fields**: `filename` / `funcname` null → `"<unknown>"`.

**Edge cases**: no trailing newline. The transport does not append one either; lines may run together if the sink doesn't separate them.

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

**Output format**: same as `TextFormatter`, but the `[<level>]` tag is wrapped in ANSI SGR escapes derived from `config.states[level]`.

**Construction**: precomputes `styleStrs_[i]` (one per level) and `clearStr_`.

**No default constructor**: `TermFormatter() = default` was removed intentionally. You must pass a `Config`, even if empty.

**Edge cases**

- `Config{}` produces the same output as `TextFormatter` (all escape strings are `"\033[0m"`).
- Outputting to a file or a non-ANSI terminal shows garbage.
- `styleStrs_[i]` is built from `term.state(config.states[i]).snapshot()`. If `config.states[i]` is default-constructed, the snapshot is `"\033[0m"`.

---

### 7.3 Factory functions and builders

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

`TermFormatterConfig::state<Level>(state)` sets the `TermStr::State` for one level.

---

## 8. Built-in transports

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

#### 8.1.1 Construction

**Parameter**: `max_cache_size` — bytes. `0` means "use default".

**Behaviour**: `set_max_cache_size(max_cache_size)`. If `0` (which is `< MinCacheSize`), the call returns `false` and `max_cache_size_` stays at the default `(Min + Max) / 2 = 37 KiB`.

**Postcondition**: ready to accept `from` calls. `thread_local` caches are empty.

#### 8.1.2 `from`

```cpp
void from(const ::lig::logger::Task &task);
```

**Behaviour**

1. `auto entity = find_entity();` — locate or create the calling thread's `Entity` (holding the `cache`).
2. `task(formatter_.get(), entity->cache);` — invoke the task; it formats into the cache.
3. If `entity->cache.size() > max_cache_size_`, call `flush()`.

**Edge cases**

- `task` empty: `Task::operator()` asserts (debug) or crashes (release). `SyncTransport` does **not** guard against empty tasks; only `AsyncTransport` uses empty tasks as flush sentinels.
- Multiple threads: each has its own `Entity` in `thread_local entities_`. No synchronization between threads; writes to the shared sinks may interleave. Built-in `FileSink` is internally locked; `ConsoleSink` relies on the kernel fd lock.
- `formatter_` null: segfault. `add_entity` always sets it, so this only happens if you construct `SyncTransport` directly and forget to call `set_formatter`.

#### 8.1.3 `flush`

```cpp
void flush();
```

**Behaviour**: `auto entity = find_entity(); if (!cache.empty()) { for (auto &w : wrappers_) w->from(cache); cache.clear(); }`.

**Postcondition**: the calling thread's cache is written to all sinks and cleared.

**Edge cases**: other threads' caches are untouched. To flush everything, call `flush` from each logging thread, or use `AsyncTransport`.

#### 8.1.4 `find_entity` (private)

Looks up the calling thread's `Entity` in `thread_local entities_` by matching `instance == this`. Creates one if missing. The `Entity` is a `shared_ptr`, so it survives across calls.

#### 8.1.5 `set_formatter` / `add_sink`

`set_formatter` swaps `formatter_` with the argument; the argument becomes null. `add_sink` wraps the sink in a `SinkWrapper` and pushes it into `wrappers_`.

**Precondition**: called before any `from` / `flush`. Not enforced.

#### 8.1.6 `set_max_cache_size`

```cpp
auto set_max_cache_size(size_t size) noexcept -> bool;
```

**Return**: `false` if `size > MaxCacheSize || size < MinCacheSize`. Otherwise set and return `true`.

**Edge cases**: `0` returns `false` and leaves the size unchanged.

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

#### 8.2.1 Internal state

- `task_queue_`: `nlqueue::VyukovMpmc<Task, 512, Strategy::Wait>`.
- `data_buffer_`: `DataBuffer`, coalesces formatted bytes into 2 KiB blocks.
- `cache_`: `std::vector<std::byte>`, accumulates formatted bytes until `max_cache_size_`.
- `overflow_mutex_` + `overflow_data_`: protected buffer for tasks that couldn't be queued within 3 ms.
- `is_stop_`: `atomic_bool`, set by `stop()`.
- `flush_notifier_`: `atomic_size_t` + `std::atomic::wait` for `flush()`.
- `t_`: worker thread.

#### 8.2.2 `start`

```cpp
void start();
```

**Behaviour**: spawns a worker thread running:

```cpp
while (!is_stop_.load(relaxed)) {
    // reserve cache_ to max_cache_size_
    auto tasks = task_queue_.pop_till_empty();
    for (auto &task : tasks) {
        if (!task) {
            // flush sentinel: drain data_buffer_, cache_flush, notify
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
// drain remaining tasks
// flush data_buffer_, cache_flush, notify
```

**Postcondition**: worker is running.

**Edge cases**

- Calling `start()` twice spawns two workers. Both consume from the same queue. This is not supported; don't do it.
- `start()` does not check `is_stop_`. Calling `start()` after `stop()` spawns a worker that immediately sees `is_stop_ == true` and exits.

#### 8.2.3 `from`

```cpp
void from(const ::lig::logger::Task &task);
```

**Behaviour**

```cpp
auto now = steady_clock::now();
while (!task_queue_.try_push(task) && task) {
    if (steady_clock::now() - now > 3ms) {
        // overflow path
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

**Postcondition**: the task is either in the queue or in the overflow buffer.

**Edge cases**

- **Empty task**: the `&& task` guard prevents the overflow path. An empty task is pushed unconditionally (loop condition is `!try_push(task) && task`, so for empty tasks, the loop exits on the first `!try_push` without entering). This is a subtle but correct way to ensure flush sentinels always go through.
- **3 ms timeout**: under sustained overload, front-end latency jumps from ns to µs (format + mutex + insert). This is by design; it prevents unbounded blocking.
- **Overflow buffer**: unbounded. If the worker never catches up, `overflow_data_` grows without limit. Not currently bounded.

#### 8.2.4 `flush`

```cpp
void flush();
```

**Behaviour**

```cpp
if (is_stop_.load(acquire)) return;
auto cur = flush_notifier_.load(acquire);
task_queue_.push(Task{});
flush_notifier_.wait(cur);
```

**Postcondition**: all tasks queued before the empty sentinel have been formatted and written to the sinks.

**Edge cases**

- **Empty task push may block**: `task_queue_.push(Task{})` is the blocking variant of `try_push` (from `VyukovMpmc`), which uses `Strategy::Wait`'s futex back-off. If the queue is full for a long time, `flush` blocks.
- **After stop**: returns immediately without waiting.
- **Does not fsync**: `FileSink`'s `sync_interval` still controls durability.

#### 8.2.5 `stop`

```cpp
void stop();
```

**Behaviour**: `if (is_stop_.exchange(true)) return; task_queue_.push(Task{});`.

**Postcondition**: worker will exit after draining the queue and processing the empty sentinel.

**Edge cases**

- Does not join. `~AsyncTransport` joins.
- Safe to call from any thread.

#### 8.2.6 `set_max_cache_size`

Same as `SyncTransport`: `[MinCacheSize, MaxCacheSize]`, returns `false` otherwise.

#### 8.2.7 `~AsyncTransport`

**Behaviour**: `stop(); if (t_.joinable()) t_.join();`.

**Postcondition**: worker thread has exited; pending tasks are flushed (or dropped if the queue couldn't drain).

**Edge cases**: if `from` was called after `stop`, the tasks are still in the queue. The worker drains them before exiting.

---

## 9. Internal components

These live in `lig::component::internal`. They are implementation details but are documented because `SinkWrapper` behaviour is user-visible.

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

**Purpose**: owning wrapper around a `new[]`-allocated byte array.

**Behaviour**

- `Data(byte*, size_t)` takes ownership.
- `~Data()` calls `delete[] data_`.
- Move transfers ownership; source becomes `{nullptr, 0}`.
- Copy is deleted.

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

**Default `batch_size_`**: `2048` bytes.

**`push(data, len)`**

- Appends `len` bytes to the internal buffer.
- Every time the buffer reaches `batch_size_`, emits a full `Data` and allocates a fresh buffer.
- Returns the emitted `Data` blocks (may be empty).

**`flush()`**

- If the internal buffer is non-empty, emits it as a `Data`.
- Returns `std::nullopt` if empty.

**`setBatchSize(size)`**

- Returns `false` if `size < MinBatchSize`, `size > MaxBatchSize`, or `size <= len_`.
- Otherwise reallocates, copies the existing `len_` bytes, updates `batch_size_`.

**Edge cases**

- `setBatchSize` cannot shrink below the currently-used bytes.
- `flush` does not reset `batch_size_`.

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

**Purpose**: wraps a `Sink` and handles write failures.

**Construction**

Takes ownership of the sink. The caller's `unique_ptr` becomes null.

**`from(datas)`**

1. If `fail_data` is non-empty, try writing it; on success, `erase` the written prefix.
2. Write `datas`:
   - Success → done.
   - Failure → call `sink_->open()`:
     - `open()` succeeds → retry writing the unwritten tail.
     - `open()` fails, or retry fails → `store_fail_data` the unwritten tail.

**`store_fail_data`** (private)

- Appends the data to `fail_data`.
- If `fail_data` would exceed `MaxFailDataSize`, drops the oldest bytes first.

**Edge cases**

- **Best-effort**: not all data is guaranteed to reach the sink. Under persistent failure, data is dropped.
- `fail_data` is not persisted across `SinkWrapper` destruction.
- `open()` is called at most once per failed `from`.

---

## 10. Edge-case summary

| Scenario | Behaviour | API involved |
|---|---|---|
| `Config::start()` called twice | Second call creates formatters/sinks, then discards them via `add_entity == false`. No leak, wasted work. | `Config::start` |
| `TransportConfig` without formatter | `formatter_->start()` dereferences null → segfault. | `TransportConfig::start` |
| `Log::operator()` with `g_level > L` | Returns without constructing a `Task`. | `Log::operator()` |
| `Task` empty, `operator()` called | `assert(vtable_)` in debug; UB in release. | `Task::operator()` |
| `Task` capturing > 128 bytes | Heap `ControlBlock` + refcount. Copy is cheap (refcount bump). | `Task` ctor / copy |
| `SyncTransport::flush()` | Only flushes the calling thread's cache. | `SyncTransport::flush` |
| `AsyncTransport::from` queue full 3 ms | Overflow path: inline format + mutex + insert. Latency jumps to µs. | `AsyncTransport::from` |
| `AsyncTransport::flush()` | Blocks until queue drains; does not fsync. | `AsyncTransport::flush` |
| `FileSink::write` with `len == 0` | Returns immediately; no rotation, no write. | `FileSink::write` |
| `FileSink::write` when `file_size + len == max_file_size` | Rotation triggers (`>=`, not `>`). | `FileSink::write` |
| `FileSink::write` when rotation `open` fails | Returns `{len = partial, err = io_error}`; `SinkWrapper` retries. | `FileSink::write` |
| `FileSink::open()` | Advances `rotate_file_` (not `config_.file`). Filenames are monotonic (`1_app.log`, `2_app.log`, ...). | `FileSink::open` |
| `FileSink::set_file` with absolute path and `change_dir = false` | First rotation goes to `config_.dir` instead of the file's parent. Pass `change_dir = true` or set `dir` explicitly. | `FileSink::set_file` |
| `FileSink::set_directory("")` | Falls back to `current_path()`. | `FileSink::set_directory` |
| `FileSink` concurrent `write` | Safe. Internal `std::mutex` serialises all mutating operations. | `FileSink::write` |
| `FileSink::is_valid()` / `size()` | Lock-free atomic reads; safe from any thread. | `FileSink::is_valid` / `size` |
| `DataBuffer::setBatchSize(size <= len_)` | Returns `false`; unchanged. | `DataBuffer::setBatchSize` |
| `SinkWrapper` persistent write failure | `fail_data` grows to 128 KiB, then oldest is dropped. | `SinkWrapper::from` |
| Empty `Task` in `AsyncTransport` | Flush sentinel. `&& task` guard keeps it out of the overflow path. | `AsyncTransport::from` |
| `InnerBackend::flush()` after `stop()` | `AsyncTransport::flush` returns immediately. | `InnerBackend::flush` |
| `ConsoleSink::close()` then `write` | Returns `EBADF`. | `ConsoleSink::write` |
| `TermFormatter` default construction | Removed; must pass `Config{}`. | `TermFormatter` ctor |
| `Level` out of range in `get_level_string` | UB (array out of bounds). | `get_level_string` |
| `Status` bits 4..7 set | Ignored by all built-in formatters. | `Status` |
| `LogHead::filename == nullptr` | Built-in formatters emit `"<unknown>"`. | `TextFormatter` / `TermFormatter` |
| `Log` object reused across calls | Reports the construction site's location, not the call site. | `Log` ctor |
