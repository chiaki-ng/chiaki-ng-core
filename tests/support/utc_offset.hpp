#pragma once

#include <libchiaking/platform/runtime/utc_offset.hpp>

#include <chrono>
#include <optional>

namespace test_support {

class FixedUtcOffset final : public libchiaking::platform::UtcOffsetProvider {
 public:
  explicit FixedUtcOffset(std::optional<std::chrono::minutes> offset) noexcept
      : offset_(offset) {}

  auto utc_offset() const noexcept
      -> std::optional<std::chrono::minutes> override {
    return offset_;
  }

 private:
  std::optional<std::chrono::minutes> offset_;
};

}  // namespace test_support
