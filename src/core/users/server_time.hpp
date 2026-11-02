/// @file
/// @brief Declares ServerTime, the registry-wide reckoning of what time PSN's
/// servers think it is.
///
/// Timestamps sent back to PS, such as datacenter measurements during a cloud
/// launch, must agree with PS's clock rather than the device's, which the
/// user can set to anything. SignedInUsers owns one ServerTime and every PsUser
/// it hands out reaches the same one.
#pragma once

#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include "src/core/psn/server_clock.hpp"

#include <chrono>
#include <optional>

namespace libchiaking::users {

/// @brief PSN's own clock, learned from the `Date` header of PS's replies and
/// carried forward on the device's monotonic clock.
///
/// It wraps one psn::ServerClock. The clock knows nothing until a reply from a
/// PS host has been recorded into it through server_clock(). Today only the
/// cloud launch does that, by sending its requests through
/// psn::ServerClockHttp; other requests the library makes do not feed it. Once
/// a reply is recorded the clock answers the offset it learned plus the
/// monotonic time elapsed since, so changing the device's wall clock does not
/// move it. The `Date` header has one-second precision, so the answer is no
/// finer than that.
///
/// One ServerTime is owned by SignedInUsers::Core and lives as long as the
/// registry; each PsUser::Core holds a reference to it. Reading and recording
/// are lock-free atomics, so it is safe to share between threads.
class ServerTime {
 public:
  /// @brief A moment on PSN's clock, in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief Builds a clock that has not yet seen a reply from PS.
  ///
  /// @param monotonic The device's monotonic clock, used to carry the learned
  ///     offset forward. Must outlive this object.
  explicit ServerTime(const platform::ClockProvider& monotonic) noexcept;
  /// @brief Not copyable; every user of a registry shares the one clock.
  ServerTime(const ServerTime&) = delete;
  /// @brief Not copy-assignable; every user of a registry shares the one
  /// clock.
  auto operator=(const ServerTime&) -> ServerTime& = delete;

  /// @brief Finds the PS clock of the registry a user belongs to.
  ///
  /// Never blocks; safe from any thread.
  ///
  /// @param user A user handed out by SignedInUsers.
  /// @return The registry's clock. It is the same object for every user of
  ///     one registry and lives as long as that registry.
  static auto of(PsUser& user) noexcept -> ServerTime&;

  /// @brief Reads PSN's current time.
  ///
  /// Never blocks; safe from any thread.
  ///
  /// @return PS's time, or nothing until a reply carrying a readable `Date`
  ///     header from a PS host has been recorded, and again after the
  ///     underlying clock is reset.
  auto now() const noexcept -> std::optional<Time>;
  /// @brief Reaches the underlying clock, so that replies can be recorded
  /// into it and launch code can read it in raw milliseconds.
  ///
  /// Never blocks; the clock itself is safe to use from any thread.
  ///
  /// @return The clock, which lives as long as this object.
  auto server_clock() noexcept -> psn::ServerClock& { return clock_; }

 private:
  /// @brief The offset between PS's clock and the device's monotonic
  /// clock, in milliseconds; unset until the first PS reply is recorded,
  /// replaced by each later one.
  psn::ServerClock clock_;
};

}  // namespace libchiaking::users
