/// @file
/// @brief Declares CloudRelaunch, the user's memory of when its last cloud
/// stream ended.
///
/// PS keeps a cloud server held for a short while after its stream ends and
/// refuses to lock the account to a new session until it lets go. A cloud
/// launch therefore waits out the rest of a settling time before it asks.
#pragma once

#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::users {

/// @brief When the user's last cloud stream released its server, measured on
/// the device's monotonic clock.
///
/// Each PsUser::Core owns one, so it lives as long as the user and every
/// launch for that user reaches the same one. Safe to share between threads.
class CloudRelaunch {
 public:
  /// @brief How long PS needs after a cloud stream ends before it will lock
  /// the account to another.
  static constexpr std::chrono::milliseconds settle_time{
      std::chrono::seconds{20}};

  /// @brief Builds a memory of no stream at all.
  ///
  /// @param threading Makes the lock guarding the time.
  /// @param clock The device's monotonic clock. Must outlive this object.
  CloudRelaunch(platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock);

  /// @brief Finds the memory of a user's last cloud stream.
  ///
  /// Never blocks; safe from any thread.
  ///
  /// @param user A user handed out by SignedInUsers.
  /// @return The memory, which lives as long as the user.
  static auto of(PsUser& user) noexcept -> CloudRelaunch&;

  /// @brief Records that a cloud stream released its server just now.
  void ended();

  /// @brief Reads how much of the settling time is left.
  ///
  /// @return The time left, or zero when no stream has ended or it ended at
  ///     least ``settle_time`` ago.
  [[nodiscard]] auto remaining() const -> std::chrono::milliseconds;

 private:
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Guards ended_us_.
  mutable platform::Mutex mutex_;
  /// @brief When the last stream ended, in microseconds; empty until one
  /// has.
  std::optional<std::uint64_t> ended_us_;
};

}  // namespace libchiaking::users
