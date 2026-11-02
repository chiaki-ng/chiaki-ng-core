#include "src/core/users/request_pacer.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace libchiaking::users {

RequestPacer::RequestPacer(platform::ThreadingProvider& threading,
                           const platform::ClockProvider& clock,
                           net::http::WaiterFactory waiters, Limits limits)
    : clock_(clock),
      waiters_(std::move(waiters)),
      limits_(limits),
      mutex_(threading) {}

auto RequestPacer::window_cost() const noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(limits_.window.count());
}

auto RequestPacer::lane_of(AccountId account, std::uint64_t now_us) -> Lane& {
  auto found = lanes_.find(account.value());
  if (found != lanes_.end()) return found->second;
  const auto budget = std::uint64_t{limits_.window_requests} * window_cost();
  return lanes_
      .emplace(
          account.value(),
          Lane{timing::TokenBucket{limits_.burst, limits_.burst, now_us},
               timing::TokenBucket{limits_.window_requests, budget, now_us},
               std::nullopt})
      .first->second;
}

auto RequestPacer::turn(AccountId account)
    -> Expected<std::uint64_t, users::Unanswered> {
  const platform::Lock lock{mutex_};
  const auto now = clock_.now_microseconds();
  auto& lane = lane_of(account, now);
  if (lane.cooling_until_us && now < *lane.cooling_until_us)
    return unexpected(
        users::Unanswered{users::Unanswered::Reason::rate_limited});
  lane.cooling_until_us.reset();
  lane.burst.refill(now);
  lane.window.refill(now);
  if (!lane.window.holds(window_cost()))
    return unexpected(
        users::Unanswered{users::Unanswered::Reason::rate_limited});
  if (lane.burst.take(1)) {
    static_cast<void>(lane.window.take(window_cost()));
    return std::uint64_t{0};
  }
  return std::max<std::uint64_t>(
      timing::Deadline::elapsed_us(now, lane.burst.ready_at_us(1)), 1);
}

auto RequestPacer::admit(AccountId account, const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  constexpr std::uint64_t microseconds_per_millisecond = 1000;
  std::unique_ptr<net::http::Waiter> waiter;
  for (;;) {
    if (stop.stop_requested())
      return unexpected(users::Unanswered::cancelled());
    auto wait = turn(account);
    if (!wait) return unexpected(wait.error());
    if (*wait == 0) return {};
    const auto milliseconds =
        std::min<std::uint64_t>((*wait + microseconds_per_millisecond - 1) /
                                    microseconds_per_millisecond,
                                std::numeric_limits<int>::max());
    if (!waiter) waiter = waiters_();
    if (!waiter->wait_milliseconds(static_cast<int>(milliseconds), stop))
      return unexpected(users::Unanswered::cancelled());
  }
}

void RequestPacer::throttled(AccountId account) {
  const platform::Lock lock{mutex_};
  const auto now = clock_.now_microseconds();
  const auto cooldown_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(limits_.cooldown)
          .count());
  lane_of(account, now).cooling_until_us =
      timing::Deadline::after_us(now, cooldown_us);
}

void RequestPacer::forget(AccountId account) {
  const platform::Lock lock{mutex_};
  lanes_.erase(account.value());
}

}  // namespace libchiaking::users
