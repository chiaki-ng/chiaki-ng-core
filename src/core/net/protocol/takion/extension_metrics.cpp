#include "src/core/net/protocol/takion/extension_metrics.hpp"

namespace libchiaking::net::protocol::takion {

void ExtensionReceiveMetrics::observe(std::uint32_t remote_us,
                                      std::uint32_t counter,
                                      std::uint64_t now_us) noexcept {
  const auto local_us = static_cast<std::uint32_t>(now_us);
  last_delta_us_ = transit_delta(local_us, remote_us, previous_local_us_,
                                 previous_remote_us_);
  if (timed_) {
    const auto magnitude = last_delta_us_ < 0
                               ? static_cast<std::uint64_t>(
                                     -static_cast<std::int64_t>(last_delta_us_))
                               : static_cast<std::uint64_t>(last_delta_us_);
    jitter_scaled_ = jitter_scaled_ + magnitude - ((jitter_scaled_ + 8U) >> 4U);
  }
  previous_local_us_ = local_us;
  previous_remote_us_ = remote_us;
  timed_ = true;
  count(counter, now_us);
}

void ExtensionReceiveMetrics::observe_untimed(std::uint32_t counter,
                                              std::uint64_t now_us) noexcept {
  timed_ = false;
  count(counter, now_us);
}

void ExtensionReceiveMetrics::count(std::uint32_t counter,
                                    std::uint64_t now_us) noexcept {
  if (received_ == 0) {
    base_ = counter;
    highest_ = counter;
    interval_start_us_ = now_us;
  } else {
    const auto step = static_cast<std::int32_t>(
        counter - static_cast<std::uint32_t>(highest_));
    if (step > 0) highest_ += static_cast<std::uint64_t>(step);
  }
  ++received_;
  if (now_us - interval_start_us_ < extension_loss_interval_us) return;
  const auto wanted = expected();
  const auto interval_expected = wanted - interval_expected_;
  const auto interval_received = received_ - interval_received_;
  loss_ratio_ =
      interval_expected > interval_received
          ? static_cast<double>(interval_expected - interval_received) /
                static_cast<double>(interval_expected)
          : 0.0;
  interval_expected_ = wanted;
  interval_received_ = received_;
  interval_start_us_ = now_us;
}

}  // namespace libchiaking::net::protocol::takion
