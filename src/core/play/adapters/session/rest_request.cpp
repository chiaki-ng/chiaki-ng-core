#include "src/core/play/adapters/session/rest_request.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/session/locked_session_lifecycle.hpp"

#include <algorithm>

namespace libchiaking::play {

RestRequest::RestRequest(platform::ThreadingProvider& threading,
                         const CommandRoute& route,
                         const LockedSessionLifecycle& lifecycle,
                         platform::ClockProvider& clock,
                         std::chrono::milliseconds flush)
    : route_(route),
      lifecycle_(lifecycle),
      clock_(clock),
      flush_(flush),
      mutex_(threading) {}

auto RestRequest::request(Reason reason) -> bool {
  bool accepted = false;
  {
    const platform::Lock lock(mutex_);
    accepted =
        route_.console() && (phase_ != Phase::idle || !lifecycle_.sleeping());
    if (accepted && phase_ == Phase::idle) {
      phase_ = Phase::queued;
      reason_ = reason;
    } else if (accepted && reason == Reason::host_sleep) {
      reason_ = Reason::host_sleep;
    } else if (!accepted && phase_ == Phase::idle) {
      phase_ = Phase::refused;
    }
  }
  if (accepted) route_.wake();
  return accepted;
}

auto RestRequest::outcome() const noexcept -> Outcome {
  const platform::Lock lock(mutex_);
  switch (phase_) {
    case Phase::idle:
      return Outcome::none;
    case Phase::flushing:
      return Outcome::sent;
    case Phase::queued:
    case Phase::refused:
      break;
  }
  return Outcome::refused;
}

auto RestRequest::idle() const noexcept -> bool {
  const platform::Lock lock(mutex_);
  return phase_ == Phase::idle;
}

auto RestRequest::queued() const noexcept -> bool {
  const platform::Lock lock(mutex_);
  return phase_ == Phase::queued;
}

auto RestRequest::flushing() const noexcept -> bool {
  const platform::Lock lock(mutex_);
  return phase_ == Phase::flushing;
}

auto RestRequest::for_sleep() const noexcept -> bool {
  const platform::Lock lock(mutex_);
  return reason_ == Reason::host_sleep;
}

auto RestRequest::advance(std::chrono::milliseconds wait) -> Step {
  if (idle()) return {false, wait};
  const auto now = clock_.now_microseconds();
  if (queued()) {
    const auto result =
        route_.driver().execute(net::protocol::remote_play::command::GotoBed{});
    route_.report("Console rest mode request", result);
    const platform::Lock lock(mutex_);
    if (phase_ == Phase::queued) {
      phase_ = result ? Phase::flushing : Phase::refused;
      flushing_since_us_ = result ? now : 0;
    }
  }
  const platform::Lock lock(mutex_);
  const auto flush_us =
      static_cast<std::uint64_t>(flush_.count()) * microseconds_per_millisecond;
  const auto elapsed = now - flushing_since_us_;
  if (phase_ != Phase::flushing || elapsed >= flush_us) return {true, wait};
  const auto remaining =
      (flush_us - elapsed + microseconds_per_millisecond - 1) /
      microseconds_per_millisecond;
  return {false, std::min(wait, std::chrono::milliseconds{
                                    static_cast<std::int64_t>(remaining)})};
}

}  // namespace libchiaking::play
