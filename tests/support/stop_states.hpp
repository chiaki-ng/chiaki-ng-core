#pragma once

#include <libchiaking/platform/runtime/stop_token.hpp>

#include <atomic>
#include <memory>

namespace test_support {

class StopState final : public libchiaking::platform::StopState {
 public:
  auto stop_requested() const noexcept -> bool override { return stopped; }

  bool stopped{};
};

class AtomicStop final : public libchiaking::platform::StopState {
 public:
  void request_stop() noexcept {
    stopped.store(true, std::memory_order_release);
  }

  auto stop_requested() const noexcept -> bool override {
    return stopped.load(std::memory_order_acquire);
  }

  std::atomic<bool> stopped{};
};

class Stopped final : public libchiaking::platform::StopState {
 public:
  static auto token() -> libchiaking::platform::StopToken {
    return libchiaking::platform::StopToken{std::make_shared<Stopped>()};
  }

  auto stop_requested() const noexcept -> bool override { return true; }
};

class WatchedStop final : public libchiaking::platform::StopState {
 public:
  WatchedStop() = default;
  explicit WatchedStop(bool stops) noexcept : stops_(stops) {}

  auto stop_requested() const noexcept -> bool override {
    ++asked;
    return stops_;
  }

  mutable std::atomic<int> asked{};

 private:
  bool stops_{};
};

}  // namespace test_support
