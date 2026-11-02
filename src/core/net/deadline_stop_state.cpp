#include "src/core/net/deadline_stop_state.hpp"

namespace libchiaking::net {

auto RemoteWaitDeadline::arm(
    const platform::ClockProvider& clock) const noexcept -> std::uint64_t {
  if (wait_timeout == 0) return absolute;
  return std::min(absolute, timing::Deadline::after_us(clock.now_microseconds(),
                                                       wait_timeout));
}

DeadlineStopState::DeadlineStopState(
    platform::StopToken parent, const platform::ClockProvider& clock,
    std::uint64_t deadline_microseconds) noexcept
    : parent_(std::move(parent)),
      clock_(&clock),
      deadline_(timing::Deadline::instant_of(deadline_microseconds)) {}

bool DeadlineStopState::stop_requested() const noexcept {
  return parent_.stop_requested() || timed_out();
}

bool DeadlineStopState::timed_out() const noexcept {
  return parent_.deadline_expired() ||
         (!parent_.stop_requested() && deadline_.expired(*clock_));
}

}  // namespace libchiaking::net
