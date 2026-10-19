#ifndef LIGLOG_INCLUDE_LOGGER_HPP
#define LIGLOG_INCLUDE_LOGGER_HPP
#include <chrono>
#include <cstddef>
#include <vector>
#pragma once

#include "interface.hpp"
#include <atomic>
#include <source_location>
#include <string>
#include <utility>
#if defined(_WIN32)
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif

namespace lig::logger {

// LIG_LOGGER_LEVEL == 0 support dynamic level
// LIG_LOGGER_LEVEL == 1 support dynamic level(bigger than Info)
// LIG_LOGGER_LEVEL == 2 support dynamic level(bigger than Warn)
#ifndef LIG_LOGGER_LEVEL
#define LIG_LOGGER_LEVEL 0
#endif

inline std::atomic<::lig::Level> g_level{};
inline auto getThreadId() -> uint64_t {
#if defined(_WIN32)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<uint64_t>(::pthread_self());
#endif
}

template <typename BE> inline auto getBackend() -> BE & {
    static BE backend{};
    return backend;
}

struct Task {
    static constexpr size_t InlineSize = 128;

    Task() = default;

    Task(Task &&other) noexcept { move_from(std::move(other)); }
    auto operator=(Task &&other) noexcept -> Task & {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }
    Task(const Task &other) { copy_from(other); }
    auto operator=(const Task &other) -> Task & {
        if (this != &other) {
            reset();
            copy_from(other);
        }
        return *this;
    }
    ~Task() { reset(); }

    template <
        typename F,
        typename = std::enable_if_t<
            !std::is_same_v<std::decay_t<F>, Task> &&
            std::is_invocable_v<std::decay_t<F>, const interface::Formatter *,
                                std::vector<std::byte> &> &&
            std::is_same_v<void, std::invoke_result_t<std::decay_t<F>, const interface::Formatter *,
                                                      std::vector<std::byte> &>>>>
    Task(F &&func) {
        using T = std::decay_t<F>;
        if constexpr (sizeof(T) <= InlineSize && alignof(T) <= alignof(std::max_align_t)) {
            new (static_cast<void *>(inline_)) T(std::forward<F>(func));
            block_ = nullptr;
        } else {
            block_ = new ControlBlock();
            block_->heap_ = new T(std::forward<F>(func));
            block_->ref_.store(1, std::memory_order_relaxed);
        }
        vtable_ = &VirtualTable<T>;
    }

    explicit operator bool() const noexcept { return vtable_; }
    void operator()(const interface::Formatter *formatter, std::vector<std::byte> &data) const {
        vtable_->invoke(ptr(), formatter, data);
    }

  private:
    struct VTable {
        void (*invoke)(const void *, const interface::Formatter *, std::vector<std::byte> &);
        void (*destroy)(void *) noexcept;
        void (*move)(void *dst, void *src) noexcept;
        void (*copy)(void *dst, const void *src);
    };

    struct ControlBlock {
        void *heap_{nullptr};
        std::atomic_size_t ref_{0};
    };

    template <typename T> static const VTable VirtualTable;

    void reset() noexcept {
        if (!vtable_) {
            return;
        }
        if (block_) {
            if (block_ && block_->ref_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                vtable_->destroy(ptr());
                ::operator delete(block_->heap_);
                ::operator delete(block_);
            }
        } else {
            vtable_->destroy(ptr());
        }
        block_ = nullptr;
        vtable_ = nullptr;
    }

    void move_from(Task &&other) noexcept {
        vtable_ = other.vtable_;
        if (!vtable_) {
            return;
        }
        if (other.block_) {
            block_ = other.block_;
            other.block_ = nullptr;
        } else {
            block_ = nullptr;
            vtable_->move(inline_, other.inline_);
        }
        other.vtable_ = nullptr;
    }

    void copy_from(const Task &other) {
        vtable_ = other.vtable_;
        if (!vtable_) {
            return;
        }
        if (other.block_) {
            block_ = other.block_;
            block_->ref_.fetch_add(1, std::memory_order_relaxed);
        } else {
            block_ = nullptr;
            vtable_->copy(inline_, other.inline_);
        }
    }

    auto ptr() const noexcept -> void * {
        return block_ ? block_->heap_ : const_cast<std::byte *>(inline_);
    }

    ControlBlock *block_{};
    const VTable *vtable_{nullptr};
    alignas(std::max_align_t) std::byte inline_[InlineSize]{};
};

template <typename T>
const Task::VTable Task::VirtualTable{
    [](const void *p, const interface::Formatter *f, std::vector<std::byte> &data) -> void {
        (*static_cast<const T *>(p))(f, data);
    },
    [](void *p) noexcept -> void { static_cast<T *>(p)->~T(); },
    [](void *dst, void *src) noexcept -> void {
        auto *s = static_cast<T *>(src);
        new (dst) T(std::move(*s));
        s->~T();
    },
    [](void *dst, const void *src) -> void {
        const auto *s = static_cast<const T *>(src);
        new (dst) T(*s);
    },
};

template <Level L, typename BE> class Log {
  public:
    template <typename... Args>
    void operator()(const std::format_string<Args...> &format, Args &&...args) {
        if constexpr ((LIG_LOGGER_LEVEL == 1 &&
                       static_cast<uint8_t>(L) < static_cast<uint8_t>(Level::Info)) ||
                      (LIG_LOGGER_LEVEL == 2 &&
                       static_cast<uint8_t>(L) < static_cast<uint8_t>(Level::Warn))) {
            return;
        }
        if (static_cast<uint8_t>(g_level.load(std::memory_order_relaxed)) >
            static_cast<uint8_t>(L)) {
            return;
        }
        thread_local const auto thread_id = getThreadId();
        LogHead info{
            .level = L,
            .line = loc_.line(),
            .thread_id = thread_id,
            .filename = loc_.file_name(),
            .funcname = loc_.function_name(),
            .now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())};
        Task task = [info = info, fmt = std::string_view(format.get()),
                     ... args =
                         std::forward<Args>(args)](const ::lig::interface::Formatter *formatter,
                                                   std::vector<std::byte> &data) -> void {
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
        };
        getBackend<BE>().submit(std::move(task));
    }

    Log(std::source_location &&loc = std::source_location::current()) : loc_{loc} {}
    Log(const Log &) = delete;
    Log(Log &&) = delete;
    auto operator=(const Log &) -> Log & = delete;
    auto operator=(Log &&) -> Log & = delete;

  private:
    std::source_location loc_{};
};
template <typename BE> using Trace = Log<Level::Trace, BE>;
template <typename BE> using Debug = Log<Level::Debug, BE>;
template <typename BE> using Info = Log<Level::Info, BE>;
template <typename BE> using Warn = Log<Level::Warn, BE>;
template <typename BE> using Error = Log<Level::Error, BE>;
template <typename BE> using Fatal = Log<Level::Fatal, BE>;

} // namespace lig::logger

#endif
