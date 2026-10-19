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
