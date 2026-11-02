#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/runtime/stop_token.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace test_support {

class FakeWaiter final : public libchiaking::net::http::PollWaiter {
 public:
  FakeWaiter() = default;
  explicit FakeWaiter(FakeClock* clock) noexcept : clock_(clock) {}

  auto wait_milliseconds(int milliseconds,
                         const libchiaking::platform::StopToken& stop)
      -> bool override {
    waits.push_back(milliseconds);
    if (clock_ != nullptr)
      clock_->now_us +=
          step_us.value_or(static_cast<std::uint64_t>(milliseconds) * 1000U);
    if (on_wait) on_wait();
    if (stop_state != nullptr && stop_on_wait != 0 && calls() >= stop_on_wait)
      stop_state->stopped = true;
    return answer.value_or(!stop.stop_requested());
  }

  auto calls() const noexcept -> int { return static_cast<int>(waits.size()); }

  auto factory() -> libchiaking::net::http::WaiterFactory {
    return [this]() -> std::unique_ptr<libchiaking::net::http::Waiter> {
      return std::make_unique<Forwarding>(*this);
    };
  }

  std::vector<int> waits;
  std::optional<bool> answer;
  std::optional<std::uint64_t> step_us;
  int stop_on_wait{};
  StopState* stop_state{};
  std::function<void()> on_wait;

 private:
  class Forwarding final : public libchiaking::net::http::Waiter {
   public:
    explicit Forwarding(FakeWaiter& target) noexcept : target_(target) {}

    auto wait_milliseconds(int milliseconds,
                           const libchiaking::platform::StopToken& stop)
        -> bool override {
      return target_.wait_milliseconds(milliseconds, stop);
    }

   private:
    FakeWaiter& target_;
  };

  FakeClock* clock_{};
};

}  // namespace test_support
