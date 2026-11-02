#include "src/core/catalog/ntp_clock.hpp"

namespace libchiaking::catalog {

NtpClock::NtpClock(platform::NetworkProvider& network,
                   platform::ThreadingProvider& threading,
                   const platform::ClockProvider& monotonic,
                   platform::SecureRandomProvider& random)
    : sync_(network, monotonic, random),
      monotonic_(monotonic),
      syncing_(threading),
      mutex_(threading) {}

auto NtpClock::now() const noexcept -> std::optional<Time> {
  const platform::Lock lock{mutex_};
  return held_at(monotonic_.now_microseconds());
}

auto NtpClock::refresh(const platform::StopToken& stop,
                       const ServerObserver& asking) -> std::optional<Time> {
  if (auto held = now()) return held;
  const platform::Lock syncing{syncing_};
  {
    const platform::Lock lock{mutex_};
    const auto now_us = monotonic_.now_microseconds();
    if (auto held = held_at(now_us)) return held;
    if (!may_sync_at(now_us)) return std::nullopt;
  }
  auto synced = sync_.sync(stop, asking);
  const platform::Lock lock{mutex_};
  const auto now_us = monotonic_.now_microseconds();
  if (synced) {
    held_ = *synced;
    failed_at_us_.reset();
  } else if (!stop.stop_requested()) {
    failed_at_us_ = now_us;
  }
  return held_at(now_us);
}

auto NtpClock::source() const -> std::optional<net::ntp::TrustedTime> {
  const platform::Lock lock{mutex_};
  if (!held_ || !held_->at(monotonic_.now_microseconds())) return std::nullopt;
  return held_;
}

auto NtpClock::last_sync() const -> std::optional<Sync> {
  const auto held = source();
  if (!held) return std::nullopt;
  return Sync{provider_of(held->source()),
              std::chrono::floor<std::chrono::milliseconds>(held->synced_at())};
}

auto NtpClock::provider_of(net::ntp::TimeSource source) -> std::string {
  switch (source) {
    case net::ntp::TimeSource::cloudflare:
      return "cloudflare";
    case net::ntp::TimeSource::google:
      return "google";
  }
  return {};
}

auto NtpClock::held_at(std::uint64_t now_us) const -> std::optional<Time> {
  if (!held_) return std::nullopt;
  const auto at = held_->at(now_us);
  if (!at) return std::nullopt;
  return std::chrono::floor<std::chrono::milliseconds>(*at);
}

auto NtpClock::may_sync_at(std::uint64_t now_us) const -> bool {
  constexpr auto wait_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(retry_after_failure)
          .count());
  return !failed_at_us_ || now_us < *failed_at_us_ ||
         now_us - *failed_at_us_ >= wait_us;
}

}  // namespace libchiaking::catalog
