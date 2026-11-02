#pragma once

#include "tests/support/process_threading.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

namespace test_support {

class LiveProviders {
 public:
  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return process_threading();
  }

  static auto steady() -> libchiaking::platform::SteadyClock& {
    static libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto wall() -> libchiaking::platform::SystemClock& {
    static libchiaking::platform::SystemClock clock;
    return clock;
  }

  static auto steady_us() -> std::uint64_t {
    return steady().now_microseconds();
  }

  static auto now() -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{
        std::chrono::microseconds{static_cast<std::int64_t>(steady_us())}};
  }

  static void nap(std::chrono::nanoseconds duration) {
    static_cast<void>(libchiaking::platform::Sleeper{threading(), steady()}
                          .sleep_for(duration));
  }

  static void spin_pause() { nap(std::chrono::microseconds{100}); }

  template <class Work>
  static auto spawn(std::string_view name, Work work)
      -> libchiaking::platform::Thread {
    return libchiaking::platform::Thread::start(
               threading(), name,
               libchiaking::platform::ThreadRole::unspecified, std::move(work))
        .value();
  }
};

}  // namespace test_support
