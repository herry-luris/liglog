#pragma once
#ifndef NLQUEUE_MPMC_HPP
#define NLQUEUE_MPMC_HPP
#include "utils.hpp"
#include <type_traits>
#include <optional>
#include <vector>
#include <span>

namespace nlqueue {

// this mpmc is implete according to Vyukov, and the core operator is try_emplace and try_pop,
// which insert a element in the back and delete a element on the head.
// using the slot index, the head pos to query the whether slot is empty or the tail pos to query
// the whether slot has value is a feature that I can not reach when impleting this mpmc by myself.
//
// the bulk condition may not work if there has exception throw in the push
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

#if false
// why this false version is not use, because the cross-core cached line will
// make try to let push and pop operator synchronize, i means, if you are push,
// then the pop mey be stop, this is even worse than a Mutexe Queue
//
// casuing such behavior is the following two line
// @code
// 		const size_t head = head_.load(std::memory_order_acquire);  // in try_emplace
// 		const size_t tail = tail_.load(std::memory_order_acquire);  // in try_pop
//
// 	I do this is want to kown the queue is empty or full 
template <typename... Args> auto nlqueue::Mpmc::try_emplace(Args &&...data) -> bool {
    while (true) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t size = tail - head;
        if (size >= N) {
            return false;
        }
		// the following logic is same as the below implement ... 
    }
}

auto nlqueue::Mpmc::try_pop() -> std::optional<T> {
    while (true) {
        size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t size = tail - head;
        if (size == 0) {
            return std::nullopt;
        }
		// the following logic is same as the below implement ... 
    }
}
#endif
