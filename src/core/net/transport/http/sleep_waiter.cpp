#include "src/core/net/transport/http/sleep_waiter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace libchiaking::net::http {

SleepWaiter::SleepWaiter(platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock)
    : clock_(&clock), sleeper_(std::make_unique<platform::Sleeper>(threading, clock)) {}

SleepWaiter::SleepWaiter(platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock, platform::NetworkProvider& network)
    : SleepWaiter(threading, clock) {
  try {
    auto poller = network.pollers().create_poller();
    if (poller) poller_ = std::move(poller.value());
  } catch (...) {
    poller_.reset();
  }
}

auto SleepWaiter::wait_milliseconds(int milliseconds,
                                    const platform::StopToken& stop) -> bool {
  if (milliseconds < 0) return false;
  return wait_for(std::chrono::milliseconds{milliseconds}, stop);
}

auto SleepWaiter::wait_readable(
    std::span<const platform::Pollable* const> sources,
    int timeout_milliseconds, const platform::StopToken& stop) -> bool {
  if (timeout_milliseconds < 0) return false;
  if (!poller_ || sources.empty())
    return wait_milliseconds(timeout_milliseconds, stop);
  if (stop.stop_requested()) return false;
  try {
    std::vector<platform::PollRequest> requests;
    requests.reserve(sources.size());
    for (const auto* source : sources)
      requests.push_back({source, 0, platform::PollEvent::readable});
    const auto waited = poller_->wait(
        requests, std::chrono::milliseconds{timeout_milliseconds}, stop);
    if (!waited) return wait_milliseconds(timeout_milliseconds, stop);
  } catch (...) {
    return wait_milliseconds(timeout_milliseconds, stop);
  }
  return !stop.stop_requested();
}

void SleepWaiter::wait(const platform::Pollable* pollable,
                       platform::PollEvent interest,
                       std::chrono::nanoseconds timeout) {
  if (timeout.count() <= 0) return;
  if (!poller_) {
    static_cast<void>(sleeper_->sleep_for(
        std::min(timeout, std::chrono::nanoseconds{idle_slice})));
    return;
  }
  const std::array<platform::PollRequest, 1> requests{{{pollable, 0, interest}}};
  const auto watched = pollable != nullptr && interest != platform::PollEvent::none
                           ? std::span<const platform::PollRequest>{requests}
                           : std::span<const platform::PollRequest>{};
  (void)poller_->wait(watched, timeout, platform::StopToken{});
}

void SleepWaiter::wake() noexcept {
  if (poller_) (void)poller_->wake();
}

auto SleepWaiter::wait_for(std::chrono::nanoseconds duration,
                           const platform::StopToken& stop) -> bool {
  if (stop.stop_requested()) return false;
  const auto started = clock_->now_microseconds();
  for (;;) {
    if (stop.stop_requested()) return false;
    const auto spent = std::chrono::microseconds{
        static_cast<std::int64_t>(clock_->now_microseconds() - started)};
    if (spent >= duration) return !stop.stop_requested();
    static_cast<void>(sleeper_->sleep_for(
        std::min(duration - spent, std::chrono::nanoseconds{sleep_slice})));
  }
}

}  // namespace libchiaking::net::http
