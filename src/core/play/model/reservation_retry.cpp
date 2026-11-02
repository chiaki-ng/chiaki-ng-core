#include "src/core/play/model/reservation_retry.hpp"

#include <algorithm>
#include <cstdint>

namespace libchiaking::play {

auto ReservationLockRetry::on_reply(bool acquired, int poll_seconds) noexcept
    -> Decision {
  network_errors_ = 0;
  if (acquired) return {Step::acquired};
  if (attempt_ == retry_limit) return {Step::failed};
  ++attempt_;
  return {Step::wait_poll, poll_seconds};
}

auto ReservationLockRetry::on_failure(ReservationPollFailure failure) noexcept
    -> Decision {
  if (failure == ReservationPollFailure::cancelled) return {Step::cancelled};
  if (failure == ReservationPollFailure::network &&
      ++network_errors_ <= network_error_limit)
    return {Step::wait_network, network_retry_seconds};
  return {Step::failed};
}

auto ReservationRestart::on_refused_lock() noexcept -> std::optional<int> {
  if (restarts_ == restart_limit) return std::nullopt;
  ++restarts_;
  return restart_seconds;
}

auto ReservationAllocateRetry::on_progress(bool waiting, int poll_seconds,
                                           int wait_estimate) noexcept
    -> Decision {
  if (!waiting) return {Step::failed};
  network_errors_ = 0;
  ++attempt_;
  if (!wait_limit_) wait_limit_ = wait_limit_for(wait_estimate);
  if (elapsed_ >= wait_limit()) return {Step::timed_out};
  const int poll = poll_seconds > 0 ? poll_seconds : default_poll_seconds;
  const int delay = std::min(poll, wait_limit() - elapsed_);
  elapsed_ += delay;
  return {Step::wait_poll, delay};
}

auto ReservationAllocateRetry::on_failure(
    ReservationPollFailure failure) noexcept -> Decision {
  if (failure == ReservationPollFailure::cancelled) return {Step::cancelled};
  if (failure != ReservationPollFailure::network) return {Step::failed};
  ++network_errors_;
  if (network_errors_ > network_error_limit) return {Step::network_failed};
  if (elapsed_ >= wait_limit()) return {Step::timed_out};
  elapsed_ += network_retry_seconds;
  return {Step::wait_network, network_retry_seconds};
}

auto ReservationAllocateRetry::wait_limit_for(int wait_estimate) noexcept
    -> int {
  if (wait_estimate <= 0) return default_wait_seconds;
  const auto doubled = static_cast<std::int64_t>(wait_estimate) * 2;
  return static_cast<int>(std::min<std::int64_t>(doubled, wait_limit_seconds));
}

auto ReservationAllocateRetry::wait_limit() const noexcept -> int {
  return wait_limit_.value_or(default_wait_seconds);
}

}  // namespace libchiaking::play
