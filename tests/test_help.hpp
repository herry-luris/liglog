#pragma once
#include <atomic>
#include <chrono>
#include <iostream>

struct Timer {
  public:
    Timer() = default;
    void start() { now_ = std::chrono::steady_clock::now(); }
    size_t stop() const noexcept { return (std::chrono::steady_clock::now() - now_).count(); }

  private:
    std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::now()};
};

inline std::atomic_size_t success = 0;
inline std::atomic_size_t fail = 0;

#define ASSERT(cond)                                                                               \
    if (cond) {                                                                                    \
        success.fetch_add(1);                                                                      \
    } else {                                                                                       \
        fail.fetch_add(1);                                                                         \
        std::cerr << "{" << __FILE__ << ":" << __LINE__ << "} ASSERT FAIL\n"                       \
                  << "(" #cond ") FAIL!!!";                                                        \
    }

#define ASSERT_EQUAL(value, desired)                                                               \
    if (value == desired) {                                                                        \
        success.fetch_add(1);                                                                      \
    } else {                                                                                       \
        fail.fetch_add(1);                                                                         \
        std::cerr << "{" << __FILE__ << ":" << __LINE__ << "} ASSERT FAIL\n"                       \
                  << "CUREENT (" #value "): " << value << "\t" << "DESIRED: " << desired << '\n';

#define TEST(func, ...)                                                                            \
    {                                                                                              \
        std::cout << "[" #func " START]...\n";                                                     \
        Timer timer{};                                                                             \
        func(__VA_ARGS__);                                                                         \
        size_t delta = timer.stop();                                                               \
        std::cout << "[" #func " END] with " << delta / 1e6 << "ms\n";                             \
    }

inline int report() noexcept {
    if (fail == 0) {
        std::cout << "ALL TESTS HAS PASS, Total " << success.load() << "\n";
        return 0;
    }
    std::cout << "PASS " << success.load() << ", FAIL " << fail.load() << "\n";
    return 1;
}
