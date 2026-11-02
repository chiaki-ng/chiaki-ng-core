/// @file
/// @brief Declares NtpClock, the stock TrustedClock that judges release dates
/// against plain NTP time.
///
/// Trusted time is never PS's HTTP Date. It comes from time.cloudflare.com,
/// then time1..time4.google.com, through net::ntp::TimeSync, and is advanced on
/// the monotonic clock until it expires after net::ntp::TrustedTime::max_age
/// (one hour).
#pragma once

#include "libchiaking/detail/catalog/trusted_clock.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ntp/time_sync.hpp"
#include "src/core/net/ntp/trusted_time.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::catalog {

/// @brief A trusted clock fed by NTP, synced on demand and advanced on the
/// monotonic clock.
///
/// The clock holds at most one sync. now never blocks on the network; only
/// refresh does, and one refresh syncs at a time while the others wait for
/// it. After a failed sync no new one is attempted for retry_after_failure.
/// Every member is safe to call from any thread. The providers given to the
/// constructor must outlive the clock. Not copyable.
class NtpClock final : public TrustedClock {
 public:
  /// @brief How long to wait after a failed sync before trying again,
  /// measured on the monotonic clock.
  static constexpr std::chrono::seconds retry_after_failure{30};

  /// @brief Builds a clock that has not synced yet.
  ///
  /// @param network Sends the NTP queries. Must outlive the clock.
  /// @param threading Makes the clock's locks. Must outlive the clock.
  /// @param monotonic Measures time since a sync, in microseconds. Must
  ///     outlive the clock.
  /// @param random Draws the origin timestamps that tie replies to
  ///     queries. Must outlive the clock.
  NtpClock(platform::NetworkProvider& network,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& monotonic,
           platform::SecureRandomProvider& random);
  /// @brief Not copyable: the clock owns its sync and its locks.
  NtpClock(const NtpClock&) = delete;
  /// @brief Not copyable: the clock owns its sync and its locks.
  auto operator=(const NtpClock&) -> NtpClock& = delete;

  /// @brief The trusted time now, from the held sync.
  ///
  /// Takes a short lock; never touches the network.
  ///
  /// @return Trusted wall-clock time in milliseconds, or std::nullopt when
  ///     there has been no sync, the held sync is older than an hour, or
  ///     the monotonic clock reads earlier than the sync.
  [[nodiscard]] auto now() const noexcept -> std::optional<Time> override;
  /// @brief The trusted time now, syncing with NTP first when none is held.
  ///
  /// Returns at once when a sync is held. Otherwise it waits for any other
  /// refresh in progress, then syncs unless the last attempt failed less
  /// than retry_after_failure ago. A sync blocks on the network: each
  /// query waits at most net::ntp::TimeSync::query_timeout (one second),
  /// for each address of each provider in turn. A sync abandoned because
  /// the stop token fired is not counted as a failure.
  ///
  /// @param stop Abandons the sync when requested.
  /// @param asking Told time.cloudflare.com, then time1..time4.google.com
  ///     as the sync falls back to them; told nothing when no sync runs.
  ///     May be empty.
  /// @return Trusted wall-clock time in milliseconds, or std::nullopt when
  ///     the sync failed, was stopped or is still in its back-off.
  [[nodiscard]] auto refresh(const platform::StopToken& stop,
                             const ServerObserver& asking)
      -> std::optional<Time> override;
  /// @brief The held sync, while it is still usable.
  ///
  /// @return The sync with its provider and time, or std::nullopt when
  ///     none is held or it has expired.
  auto source() const -> std::optional<net::ntp::TrustedTime>;
  /// @brief Which provider gave the held sync, and when.
  ///
  /// @return "cloudflare" or "google" with the synced time in
  ///     milliseconds, or std::nullopt when no usable sync is held.
  [[nodiscard]] auto last_sync() const -> std::optional<Sync> override;

 private:
  /// @brief The held sync advanced to a monotonic reading. Caller holds
  /// mutex_.
  ///
  /// @param now_us The monotonic clock in microseconds.
  /// @return The trusted time, or std::nullopt when none is held or it has
  ///     expired.
  auto held_at(std::uint64_t now_us) const -> std::optional<Time>;
  /// @brief Whether the back-off after a failure has passed. Caller holds
  /// mutex_.
  ///
  /// @param now_us The monotonic clock in microseconds.
  /// @return True when there was no failure, the clock went backwards, or
  ///     retry_after_failure has passed since it.
  auto may_sync_at(std::uint64_t now_us) const -> bool;
  /// @brief The name a provider is reported under.
  ///
  /// @param source The provider.
  /// @return "cloudflare" or "google".
  static auto provider_of(net::ntp::TimeSource source) -> std::string;

  /// @brief Runs the NTP queries; used only under syncing_.
  net::ntp::TimeSync sync_;
  /// @brief The monotonic clock the sync is advanced on.
  const platform::ClockProvider& monotonic_;
  /// @brief Serialises refreshes so that only one sync runs at a time.
  platform::Mutex syncing_;
  /// @brief Guards held_ and failed_at_us_.
  mutable platform::Mutex mutex_;
  /// @brief The latest successful sync; replaced by each new one.
  std::optional<net::ntp::TrustedTime> held_;
  /// @brief Monotonic microseconds of the last failed sync; cleared by a
  /// success.
  std::optional<std::uint64_t> failed_at_us_;
};

}  // namespace libchiaking::catalog
