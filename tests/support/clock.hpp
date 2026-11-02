#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstdint>

namespace test_support {

class FakeClock final : public libchiaking::platform::ClockProvider {
 public:
  FakeClock() = default;
  explicit FakeClock(std::uint64_t start_us) noexcept : now_us(start_us) {}

  auto now_microseconds() const noexcept -> std::uint64_t override {
    return now_us;
  }

  void advance_ms(std::int64_t milliseconds) {
    now_us += static_cast<std::uint64_t>(milliseconds) * 1000U;
  }

  std::uint64_t now_us{1'000'000'000};
};

}  // namespace test_support
