#pragma once
#ifndef NLQUEUE_SPSC_HPP
#define NLQUEUE_SPSC_HPP

#include "utils.hpp"
#include <type_traits>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>
#include <atomic>
#include <cassert>

namespace nlqueue {
template <typename T, size_t N, Strategy S = Strategy::Spin> class Spsc {
    static_assert(N != 0 && isNValid(N), "the N must be pow of 2 and not be 0");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "SPSC requires nothrow move constructible T");

  public:
    Spsc() : strategy_{pop_notifier_, push_notifier_, head_, tail_, is_close_} {
        datas_ = static_cast<T *>(::operator new(sizeof(T) * N));
    }
    ~Spsc() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            const size_t h = head_.load(std::memory_order_relaxed);
            const size_t t = tail_.load(std::memory_order_relaxed);
            for (size_t i = h; i < t; ++i) {
                std::destroy_at(&datas_[i & (N - 1)]);
            }
        }
        ::operator delete(datas_);
    }
    Spsc(Spsc &&) = delete;
    Spsc(const Spsc &) = delete;
    auto operator=(Spsc &&) -> Spsc & = delete;
    auto operator=(const Spsc &) -> Spsc & = delete;

    template <typename... Args> auto try_emplace(Args &&...args) -> bool {
        const size_t t = local_tail_;
        if (t - cache_head_ >= N) {
            cache_head_ = head_.load(std::memory_order_acquire);
            if (t - cache_head_ >= N) {
                return false;
            }
        }
        std::construct_at(&datas_[t & (N - 1)], std::forward<Args>(args)...);
        local_tail_ = t + 1;
        tail_.store(local_tail_, std::memory_order_release);
        strategy_.template notify<true>();
        return true;
    }
    auto try_pop() -> std::optional<T> {
        const size_t h = local_head_;
        if (cache_tail_ - h == 0) {
            cache_tail_ = tail_.load(std::memory_order_acquire);
            if (cache_tail_ - h == 0) {
                return std::nullopt;
            }
        }
        T data = std::move(datas_[h & (N - 1)]);
        std::destroy_at(&datas_[h & (N - 1)]);
        local_head_ = h + 1;
        head_.store(local_head_, std::memory_order_release);
        strategy_.template notify<false>();
        return std::move(data);
    }
    template <typename... Args> auto try_push_bulk(const std::span<T> &datas) -> bool {
        if (datas.size() == 0 || datas.size() > N) {
            return false;
        }
        const size_t t = local_tail_;
        if (t - cache_head_ > N - datas.size()) {
            cache_head_ = head_.load(std::memory_order_acquire);
            if (t - cache_head_ > N - datas.size()) {
                return false;
            }
        }
        for (size_t i = 0; i < datas.size(); ++i) {
            std::construct_at(&datas_[(t + i) & (N - 1)], std::move(datas[i]));
        }
        local_tail_ = t + datas.size();
        tail_.store(local_tail_, std::memory_order_release);
        strategy_.template notify<true>();
        return true;
    }
    auto try_pop_bulk(size_t n) -> std::vector<T> {
        if (n == 0 || n > N) {
            return {};
        }
        const size_t h = local_head_;
        if (cache_tail_ - h < n) {
            cache_tail_ = tail_.load(std::memory_order_acquire);
            if (cache_tail_ - h < n) {
                return {};
            }
        }
        std::vector<T> datas{};
        datas.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            datas.emplace_back(std::move(datas_[(h + i) & (N - 1)]));
            std::destroy_at(&datas_[(h + i) & (N - 1)]);
        }
        local_head_ = h + n;
        head_.store(local_head_, std::memory_order_release);
        strategy_.template notify<false>();
        return datas;
    }
    auto try_pop_till_empty() -> std::vector<T> {
        const size_t h = local_head_;
        size_t left = cache_tail_ - h;
        if (left == 0) {
            cache_tail_ = tail_.load(std::memory_order_acquire);
            left = cache_tail_ - h;
            if (left == 0) {
                return {};
            }
        }
        std::vector<T> datas{};
        datas.reserve(left);
        for (size_t i = 0; i < left; ++i) {
            datas.emplace_back(std::move(datas_[(h + i) & (N - 1)]));
            std::destroy_at(&datas_[(h + i) & (N - 1)]);
        }
        local_head_ = h + left;
        head_.store(cache_tail_, std::memory_order_release);
        strategy_.template notify<false>();
        return datas;
    }

    template <typename... Args> auto emplace(Args &&...args) -> bool {
        size_t spin = 0;
        while (!try_emplace(std::forward<Args>(args)...)) {
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
            if (res.has_value()) {
                return res;
            }
            if (strategy_.template wait<false>(spin)) {
                return std::nullopt;
            }
        }
    }
    auto push(const T &data) -> bool { return emplace(data); }
    auto push(T &&data) -> bool { return emplace(std::move(data)); }
    auto push_bulk(const std::span<T> &datas) -> bool {
        if (datas.size() == 0 || datas.size() > N) {
            return false;
        }
        size_t spin = 0;
        while (!try_push_bulk(datas)) {
            if (strategy_.template wait<true>(spin)) {
                return false;
            }
        }
        return true;
    }
    auto pop_bulk(size_t n) -> std::vector<T> {
        if (n == 0 || n > N) {
            return {};
        }
        size_t spin = 0;
        while (true) {
            auto res = try_pop_bulk(n);
            if (!res.empty()) {
                return res;
            }
            if (strategy_.template wait<false>(spin)) {
                return {};
            }
        }
    }
    auto pop_till_empty() -> std::vector<T> {
        if (is_empty()) {
            return {};
        }
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

    [[nodiscard]] auto is_empty() const noexcept -> bool {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }
    [[nodiscard]] auto is_full() const noexcept -> bool {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire) >= N;
    }
    [[nodiscard]] auto size() const noexcept -> size_t {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    void close() noexcept {
        is_close_.store(true, std::memory_order_relaxed);
        strategy_.template notify<false>();
        strategy_.template notify<true>();
    }

  private:
    alignas(64) std::atomic_size_t head_{0};
    size_t local_head_{};
    size_t cache_tail_{};
    alignas(64) std::atomic_size_t tail_{0};
    size_t local_tail_{};
    size_t cache_head_{};
    std::atomic_bool is_close_{false};
    std::atomic_size_t pop_notifier_{0};
    std::atomic_size_t push_notifier_{0};
    T *datas_{};
    StrategyMethod<N, S> strategy_;
};

} // namespace nlqueue

#endif
