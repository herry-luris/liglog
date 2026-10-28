#pragma once
#ifndef NLQUEUE_MPMC_HPP
#define NLQUEUE_MPMC_HPP
#include <type_traits>
#include <optional>
#include <vector>
#include <span>

#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <cstdint>
#include <thread>
#include <atomic>
namespace nlqueue {

inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

enum struct Strategy : uint8_t {
    Spin,
    SpinFastFail,
    SpinSleep,
    Wait,
};

static constexpr auto isNValid(const int N) -> bool {
    size_t n = N;
    while (n % 2 == 0) {
        n /= 2;
    }
    return n == 1;
}

template <size_t N, Strategy S> class StrategyMethod {
  public:
    StrategyMethod(std::atomic_size_t &pop_notifier, std::atomic_size_t &push_notifier,
                   std::atomic_size_t &head, std::atomic_size_t &tail, std::atomic_bool &is_close)
        : pop_notifier_{pop_notifier}, push_notifier_{push_notifier}, head_{head}, tail_{tail},
          is_close_(is_close) {}

    template <bool onPush> auto wait(size_t &spin) -> bool {
        if constexpr (S == Strategy::Spin) {
            if (spin < 128) {
                cpuRelax();
            } else {
                std::this_thread::yield();
            }
        } else if constexpr (S == Strategy::SpinFastFail) {
            if (spin < 128) {
                cpuRelax();
            } else {
                return true;
            }
        } else if constexpr (S == Strategy::SpinSleep) {
            if (spin < 64) {
                cpuRelax();
            } else if (spin < 128) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
                spin = 0;
            }
        } else if constexpr (S == Strategy::Wait) {
            if (spin < 36) {
                cpuRelax();
            } else if (spin < 64) {
                std::this_thread::yield();
            } else {
                if constexpr (onPush) {
                    const size_t cur = pop_notifier_.load(std::memory_order_acquire);
                    if ((tail_.load(std::memory_order_relaxed) -
                             head_.load(std::memory_order_relaxed) >=
                         N) &&
                        !is_close_.load(std::memory_order_acquire)) {
                        pop_notifier_.wait(cur);
                    }
                } else {
                    const size_t cur = push_notifier_.load(std::memory_order_acquire);
                    if (tail_.load(std::memory_order_relaxed) -
                                head_.load(std::memory_order_relaxed) ==
                            0 &&
                        !is_close_.load(std::memory_order_acquire)) {
                        push_notifier_.wait(cur);
                    }
                }
                spin = 0;
            }
        }
        spin++;
        return is_close_.load(std::memory_order_acquire);
    }

    template <bool isPushSuccess> void notify() {
        if constexpr (S == Strategy::Wait) {
            if constexpr (isPushSuccess) {
                push_notifier_.fetch_add(1, std::memory_order_release);
                push_notifier_.notify_one();
            } else {
                pop_notifier_.fetch_add(1, std::memory_order_release);
                pop_notifier_.notify_one();
            }
        }
    }

  private:
    std::atomic_size_t &push_notifier_;
    std::atomic_size_t &pop_notifier_;
    std::atomic_size_t &head_;
    std::atomic_size_t &tail_;
    std::atomic_bool &is_close_;
};
} // namespace nlqueue
namespace nlqueue {

template <typename T, size_t N, Strategy S = Strategy::Spin> class VyukovMpmc {
    static_assert(N != 0 && isNValid(N), "the N must be pow of 2");
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                  "the mpmc element type must be movable");
    struct Cell {
        alignas(64) std::atomic_size_t slot;
        std::optional<T> data;
    };

  public:
    VyukovMpmc() : strategy_{pop_notifier_, push_notifier_, head_, tail_, is_close_} {
        cells_ = new Cell[N];
        for (int i = 0; i < N; ++i) {
            cells_[i].slot.store(i, std::memory_order_relaxed);
            cells_[i].data = std::nullopt;
        }
    }
    ~VyukovMpmc() {
        close();
        delete[] cells_;
    }

    VyukovMpmc(const VyukovMpmc &) = delete;
    auto operator=(const VyukovMpmc &) -> VyukovMpmc & = delete;
    VyukovMpmc(VyukovMpmc &&) = delete;
    auto operator=(VyukovMpmc &&) -> VyukovMpmc & = delete;

    template <typename... Args> auto try_emplace(Args &&...data) -> bool {
        size_t pos = tail_.load(std::memory_order_relaxed);
        while (true) {
            Cell &cell = cells_[pos & (N - 1)];
            size_t slot = cell.slot.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)slot - (intptr_t)pos;
            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
                        cell.data.emplace(std::forward<Args>(data)...);
                    } else {
                        try {
                            cell.data.emplace(std::forward<Args>(data)...);
                        } catch (...) {
                            cell.data.reset();
                            cell.slot.store(pos + 1, std::memory_order_release);
                            strategy_.template notify<true>();
                            throw;
                        }
                    }
                    cell.slot.store(pos + 1, std::memory_order_release);
                    if constexpr (S == Strategy::Wait) {
                        strategy_.template notify<true>();
                    }
                    return true;
                }
                cpuRelax();
                continue;
            } else if (dif < 0) {
                return false;
            }
            pos = tail_.load(std::memory_order_relaxed);
        }
    }
    auto try_pop() -> std::optional<T> {
        size_t pos = head_.load(std::memory_order_relaxed);
        while (true) {
            Cell &cell = cells_[pos & (N - 1)];
            size_t slot = cell.slot.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)slot - (intptr_t)(pos + 1);
            if (dif == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    if (!cell.data) {
                        cell.slot.store(pos + N, std::memory_order_release);
                        strategy_.template notify<false>();
                        continue;
                    }
                    T data = std::move(*cell.data);
                    cell.data.reset();
                    cell.slot.store(pos + N, std::memory_order_release);
                    strategy_.template notify<false>();
                    return data;
                }
                cpuRelax();
                continue;
            } else if (dif < 0) {
                return std::nullopt;
            }
            pos = head_.load(std::memory_order_relaxed);
        }
    }
    // thie just support the movable data arguments
    // unwork condition: has exception on push(may be would trigger)
    auto try_push_bulk(std::span<T> data) -> bool {
        const size_t len = data.size();
        if (len >= N || len == 0) {
            return false;
        }
        size_t pos = tail_.load(std::memory_order_relaxed);
        while (true) {
            bool ready = true;
            for (size_t i = 0; i < len; ++i) {
                Cell &cell = cells_[(pos + i) & (N - 1)];
                intptr_t dif =
                    (intptr_t)(cell.slot.load(std::memory_order_acquire)) - (intptr_t)(pos + i);
                if (dif < 0) {
                    return false;
                } else if (dif > 0) {
                    ready = false;
                    cpuRelax();
                    pos = tail_.load(std::memory_order_relaxed);
                    break;
                }
            }
            if (ready && tail_.compare_exchange_strong(pos, pos + len, std::memory_order_relaxed)) {
                for (size_t i = 0; i < len; ++i) {
                    Cell &cell = cells_[(pos + i) & (N - 1)];
                    cell.data = std::move(data[i]);
                    cell.slot.store(pos + i + 1, std::memory_order_release);
                }
                strategy_.template notify<true>();
                return true;
            }
        }
    }
    auto try_pop_bulk(size_t len) -> std::vector<T> {
        if (len >= N || len == 0) {
            return {};
        }
        std::vector<T> bulk{};
        bulk.reserve(len);
        size_t pos = head_.load(std::memory_order_relaxed);
        while (true) {
            bool ready = true;
            for (size_t i = 0; i < len; ++i) {
                Cell &cell = cells_[(pos + i) & (N - 1)];
                intptr_t dif =
                    (intptr_t)(cell.slot.load(std::memory_order_acquire)) - (intptr_t)(pos + 1 + i);
                if (dif < 0) {
                    return {};
                } else if (dif > 0) {
                    ready = false;
                    cpuRelax();
                    pos = head_.load(std::memory_order_relaxed);
                    break;
                }
            }

            if (ready && head_.compare_exchange_strong(pos, pos + len, std::memory_order_relaxed)) {
                for (size_t i = 0; i < len; ++i) {
                    Cell &cell = cells_[(pos + i) & (N - 1)];
                    if (!cell.data) [[unlikely]] {
                        cell.slot.store(pos + i + N, std::memory_order_release);
                        continue;
                    }
                    bulk.emplace_back(std::move(*cell.data));
                    cell.data.reset();
                    cell.slot.store(pos + i + N, std::memory_order_release);
                }
                strategy_.template notify<false>();
                return bulk;
            }
            continue;
        }
    }
    auto try_pop_till_empty() -> std::vector<T> {
        std::vector<T> res{};
        while (true) {
            size_t pos = head_.load(std::memory_order_relaxed);
            size_t len{0};
            bool continue_flags = false;
            while (len <= N) {
                const intptr_t dif =
                    (intptr_t)(cells_[(pos + len) & (N - 1)].slot.load(std::memory_order_acquire)) -
                    (intptr_t)(pos + len + 1);
                if (dif == 0) {
                    len++;
                } else if (dif < 0) {
                    break;
                } else {
                    continue_flags = true;
                    break;
                }
            }
            if (continue_flags) {
                continue;
            }
            if (len == 0 || len > N) {
                return {};
            }
            if (res.capacity() < len) {
                res.reserve(len);
            }
            if (head_.compare_exchange_strong(pos, pos + len, std::memory_order_relaxed)) {
                for (size_t i = 0; i < len; ++i) {
                    Cell &cell = cells_[(pos + i) & (N - 1)];
                    if (!cell.data) [[unlikely]] {
                        cell.slot.store(pos + i + N, std::memory_order_release);
                        strategy_.template notify<false>();
                        continue;
                    }
                    res.emplace_back(std::move(cell.data.value()));
                    cell.data.reset();
                    cell.slot.store(pos + i + N, std::memory_order_release);
                }
                strategy_.template notify<false>();
                return res;
            }
        }
    }

    template <typename... Args> auto emplace(Args &&...data) -> bool {
        size_t spin = 0;
        while (!try_emplace(std::forward<Args>(data)...)) {
            if (strategy_.template wait<true>(spin)) {
                return false;
            }
        }
        return true;
    }

    // isEmty() isFull() size() is a snapshoot, may not be correct when high-concurrency
    [[nodiscard]] auto isEmpty() const -> bool { return size() == 0; }
    [[nodiscard]] auto isFull() const -> bool { return size() == N; }
    [[nodiscard]] auto size() const -> intptr_t {
        return (intptr_t)tail_.load(std::memory_order_relaxed) -
               (intptr_t)head_.load(std::memory_order_relaxed);
    }
    // after close, the queue won't close immediately. it may continue a few time.
    void close() {
        is_close_.store(true, std::memory_order_release);
        if constexpr (S == Strategy::Wait) {
            strategy_.template notify<false>();
            strategy_.template notify<true>();
        }
    }

    auto try_push(T &&data) -> bool { return try_emplace(std::move(data)); }
    auto try_push(const T &data) -> bool { return try_emplace(data); }
    auto push(T &&data) -> bool {
        size_t spin = 0;
        while (!try_emplace(std::move(data))) {
            if (strategy_.template wait<true>(spin)) {
                return false;
            }
        }
        return true;
    }
    auto push(const T &data) -> bool {
        size_t spin = 0;
        while (!try_emplace(data)) {
            if (strategy_.template wait<true>(spin)) {
                return false;
            }
        }
        return true;
    }

    auto push_bulk(std::span<T> data) {
        if (data.size() >= N) {
            return false;
        }
        size_t spin = 0;
        while (!try_push_bulk(data)) {
            if (strategy_.template wait<true>(spin)) {
                return false;
            }
        }
        return true;
    }

    auto pop() -> std::optional<T> {
        size_t spin = 0;
        while (true) {
            auto res = try_pop();
            if (res) {
                return res.value();
            }

            if (strategy_.template wait<false>(spin)) {
                return std::nullopt;
            }
        }
    }
    auto pop_bulk(size_t len) -> std::vector<T> {
        if (len >= N) {
            return {};
        }
        size_t spin = 0;
        while (true) {
            auto res = try_pop_bulk(len);
            if (!res.empty()) {
                return res;
            }

            if (strategy_.template wait<false>(spin)) {
                return {};
            }
        }
    }
    auto pop_till_empty() -> std::vector<T> {
        size_t spin = 0;
        while (true) {
            auto res = try_pop_till_empty();
            if (!res.empty()) {
                return res;
            }
            if (strategy_.template wait<false>(spin)) {
                return {};
            }
        }
    }

    void wakeup_consumer() { strategy_.template notify<true>(); }
    void wakeup_productor() { strategy_.template notify<false>(); }

  private:
    Cell *cells_{};
    alignas(64) std::atomic_bool is_close_{false};
    alignas(64) std::atomic_size_t head_{0};
    alignas(64) std::atomic_size_t tail_{0};
    alignas(64) std::atomic_size_t push_notifier_{0};
    alignas(64) std::atomic_size_t pop_notifier_{0};
    StrategyMethod<N, S> strategy_;
};
}; // namespace nlqueue
#endif
