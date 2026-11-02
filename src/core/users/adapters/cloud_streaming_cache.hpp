/// @file
/// @brief Declares CloudStreamingCache, the per-user memory of the last cloud
/// streaming verdict.
#pragma once

#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::users {

/// @brief Holds the last CloudStreaming verdict read for one user, so that
/// listing games does not ask PS every time.
///
/// A verdict PS answered (allowed or refused) is reused for lifetime; an
/// unknown one, for unknown_lifetime only, so a passing failure is retried
/// soon. A cancelled read is never kept, and an unknown verdict never
/// replaces a fresh answered one. Each clear() starts a new round, and a read
/// that began in an earlier round is dropped when it lands, so a verdict read
/// before a sign-out cannot reappear after it.
///
/// Owned by PsUser::Core and cleared when the user leaves the registry. Ages
/// are measured on the monotonic clock. Every member takes the cache's lock
/// briefly and is safe to call from several threads.
class CloudStreamingCache {
 public:
  /// @brief Whether a caller will accept the held verdict.
  enum class Freshness : std::uint8_t {
    /// @brief A held verdict that is still fresh is returned without asking
    /// PS; used when listing games.
    held_will_do,
    /// @brief PS is always asked; used when checking a launch.
    read_afresh,
  };

  /// @brief How long an answered verdict is reused: one hour.
  static constexpr std::chrono::microseconds lifetime{std::chrono::hours{1}};
  /// @brief How long an unknown verdict is reused: thirty seconds.
  static constexpr std::chrono::microseconds unknown_lifetime{
      std::chrono::seconds{30}};

  /// @brief Builds an empty cache in round zero.
  ///
  /// @param threading Provider of the cache's lock. Must outlive the cache.
  /// @param clock Monotonic clock that ages the held verdict. Must outlive
  ///     the cache.
  CloudStreamingCache(platform::ThreadingProvider& threading,
                      const platform::ClockProvider& clock)
      : clock_(clock), mutex_(threading) {}

  /// @brief Returns the held verdict while it is fresh.
  ///
  /// @return A copy of the verdict, or nothing when none is held or it has
  ///     outlived its lifetime.
  [[nodiscard]] auto held() const -> std::optional<CloudStreaming> {
    platform::Lock lock{mutex_};
    if (!is_fresh(held_)) return std::nullopt;
    return held_->verdict;
  }

  /// @brief The current round, to pass to keep() once a read lands.
  ///
  /// @return How many times the cache has been cleared.
  auto round() const -> std::uint64_t {
    platform::Lock lock{mutex_};
    return round_;
  }

  /// @brief Holds a verdict that was just read.
  ///
  /// The verdict is dropped when it is unknown because the read was
  /// cancelled, when the cache was cleared after the read began, or when it
  /// is unknown and a fresh answered verdict is already held. Otherwise it
  /// replaces whatever was held and its age starts now.
  ///
  /// @param verdict The verdict read.
  /// @param asked_in_round The round() taken before the read began.
  void keep(const CloudStreaming& verdict, std::uint64_t asked_in_round) {
    const auto cause = verdict.cause();
    if (cause && cause->reason == Unanswered::Reason::cancelled) return;
    platform::Lock lock{mutex_};
    if (asked_in_round != round_) return;
    if (cause && is_fresh(held_) && !held_->verdict.cause()) return;
    held_.emplace(Held{verdict, clock_.now_microseconds()});
  }

  /// @brief Forgets the held verdict and starts a new round, so reads still
  /// in flight are not kept.
  void clear() {
    platform::Lock lock{mutex_};
    ++round_;
    held_.reset();
  }

 private:
  /// @brief A kept verdict and when it was kept.
  struct Held {
    /// @brief The verdict.
    CloudStreaming verdict;
    /// @brief When it was kept, in microseconds on the monotonic clock.
    std::uint64_t kept_us{};
  };

  /// @brief Whether a kept verdict is still within its lifetime. The caller
  /// holds the lock.
  ///
  /// @param held The kept verdict, if any.
  /// @return False when nothing is kept; otherwise whether less than
  ///     lifetime (answered) or unknown_lifetime (unknown) has passed since
  ///     it was kept.
  auto is_fresh(const std::optional<Held>& held) const noexcept -> bool {
    if (!held) return false;
    const auto kept_for = held->verdict.cause() ? unknown_lifetime : lifetime;
    return clock_.now_microseconds() <
           held->kept_us + static_cast<std::uint64_t>(kept_for.count());
  }

  /// @brief Monotonic clock that ages the held verdict.
  const platform::ClockProvider& clock_;
  /// @brief Guards held_ and round_.
  mutable platform::Mutex mutex_;
  /// @brief The last verdict kept; empty at first and after clear().
  std::optional<Held> held_;
  /// @brief How many times clear() has run; starts at zero.
  std::uint64_t round_{};
};

}  // namespace libchiaking::users
