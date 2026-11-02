#pragma once

#include <atomic>
#include <cstddef>

namespace play_test {

class CountedAllocations {
 public:
  static void count() noexcept {
    everywhere_.fetch_add(1, std::memory_order_relaxed);
    ++on_this_thread_;
  }

  static auto everywhere() noexcept -> std::size_t {
    return everywhere_.load(std::memory_order_relaxed);
  }

  static auto on_this_thread() noexcept -> std::size_t {
    return on_this_thread_;
  }

 private:
  static inline std::atomic<std::size_t> everywhere_{0};
  static inline thread_local std::size_t on_this_thread_{0};
};

}  // namespace play_test
