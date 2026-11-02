/// @file
/// @brief Declares how a cloud server reservation keeps polling: when a lock
/// poll is retried, how long the allocation queue is waited out, and how many
/// network errors are borne.
#pragma once

#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief Why one reservation poll got no answer.
enum class ReservationPollFailure : std::uint8_t {
  /// @brief The caller cancelled.
  cancelled,
  /// @brief The request did not reach PS or its reply did not arrive.
  network,
  /// @brief Every other failure: neither a cancellation nor a transport
  /// error, such as a refusal or an unreadable reply.
  refused,
};

/// @brief Decides what follows each poll that locks a cloud session.
///
/// A lock is polled until PS grants it, at most retry_limit times after the
/// first, and up to network_error_limit network errors in a row are waited
/// out network_retry_seconds apart. A plain value owned by one launch.
class ReservationLockRetry {
 public:
  /// @brief What the launch does next.
  enum class Step : std::uint8_t {
    /// @brief The lock is held; go on.
    acquired,
    /// @brief Wait, then poll again.
    wait_poll,
    /// @brief Wait after a network error, then poll again.
    wait_network,
    /// @brief Give up.
    failed,
    /// @brief The caller cancelled.
    cancelled,
  };

  /// @brief The next step and how long to wait first.
  struct Decision {
    /// @brief The step.
    Step step;
    /// @brief The wait before the next poll, in seconds; zero when none.
    int delay_seconds{};
  };

  /// @brief The most polls after the first.
  static constexpr int retry_limit = 12;
  /// @brief The most network errors in a row.
  static constexpr int network_error_limit = 5;
  /// @brief The wait after a network error, in seconds.
  static constexpr int network_retry_seconds = 5;

  /// @brief How many polls have been retried.
  ///
  /// @return The count, from 0.
  auto attempt() const noexcept -> int { return attempt_; }
  /// @brief Decides after PS answered a poll.
  ///
  /// @param acquired Whether PS granted the lock.
  /// @param poll_seconds The wait PS asked for before the next poll.
  /// @return acquired, wait_poll with that wait, or failed once retry_limit
  ///     polls have been retried.
  auto on_reply(bool acquired, int poll_seconds) noexcept -> Decision;
  /// @brief Decides after a poll got no answer.
  ///
  /// @param failure Why.
  /// @return cancelled, wait_network while network errors in a row stay
  ///     within network_error_limit, or failed.
  auto on_failure(ReservationPollFailure failure) noexcept -> Decision;

 private:
  /// @brief How many polls have been retried.
  int attempt_{};
  /// @brief Network errors in a row.
  int network_errors_{};
};

/// @brief Decides whether a launch starts a fresh cloud session after PS
/// refused to lock its session.
///
/// PS can refuse one session's lock for good while granting a fresh
/// session's, so the launch starts again up to restart_limit times,
/// restart_seconds apart. A plain value owned by one launch.
class ReservationRestart {
 public:
  /// @brief The most fresh sessions after the first.
  static constexpr int restart_limit = 4;
  /// @brief The wait before each fresh session, in seconds.
  static constexpr int restart_seconds = 5;

  /// @brief How many fresh sessions have been started.
  ///
  /// @return The count, from 0.
  auto restarts() const noexcept -> int { return restarts_; }
  /// @brief Decides after PS refused to lock the session.
  ///
  /// @return The wait before the next fresh session, in seconds; empty once
  ///     restart_limit sessions have been started.
  auto on_refused_lock() noexcept -> std::optional<int>;

 private:
  /// @brief How many fresh sessions have been started.
  int restarts_{};
};

/// @brief Decides what follows each poll of a queued cloud server
/// allocation.
///
/// The queue is waited out for twice PS's first estimate, at most
/// wait_limit_seconds and default_wait_seconds when PS gives none, polling as
/// often as PS asks or every default_poll_seconds. Up to network_error_limit
/// network errors in a row are waited out network_retry_seconds apart. A
/// plain value owned by one launch.
class ReservationAllocateRetry {
 public:
  /// @brief What the launch does next.
  enum class Step : std::uint8_t {
    /// @brief Wait, then poll again.
    wait_poll,
    /// @brief Wait after a network error, then poll again.
    wait_network,
    /// @brief Give up after too many network errors.
    network_failed,
    /// @brief Give up; the queue took too long.
    timed_out,
    /// @brief Give up.
    failed,
    /// @brief The caller cancelled.
    cancelled,
  };

  /// @brief The next step and how long to wait first.
  struct Decision {
    /// @brief The step.
    Step step;
    /// @brief The wait before the next poll, in seconds; zero when none.
    int delay_seconds{};
  };

  /// @brief The longest wait, in seconds.
  static constexpr int wait_limit_seconds = 900;
  /// @brief The wait when PS gives no estimate, in seconds.
  static constexpr int default_wait_seconds = 300;
  /// @brief The poll interval when PS gives none, in seconds.
  static constexpr int default_poll_seconds = 15;
  /// @brief The most network errors in a row.
  static constexpr int network_error_limit = 5;
  /// @brief The wait after a network error, in seconds.
  static constexpr int network_retry_seconds = 5;

  /// @brief How many polls have reported progress.
  ///
  /// @return The count, from 0.
  auto attempt() const noexcept -> int { return attempt_; }
  /// @brief Decides after PS reported the allocation's progress.
  ///
  /// @param waiting Whether PS says the allocation is queued or migrating.
  /// @param poll_seconds The poll interval PS asked for; zero or less when
  ///     it gave none.
  /// @param wait_estimate PS's estimate of the wait, in seconds; zero or less
  ///     when it gave none. Only the first poll's estimate counts.
  /// @return failed when PS says neither, timed_out once the wait is used up,
  ///     wait_poll otherwise.
  auto on_progress(bool waiting, int poll_seconds, int wait_estimate) noexcept
      -> Decision;
  /// @brief Decides after a poll got no answer.
  ///
  /// @param failure Why.
  /// @return cancelled, failed for a refusal, network_failed past
  ///     network_error_limit, timed_out once the wait is used up, or
  ///     wait_network.
  auto on_failure(ReservationPollFailure failure) noexcept -> Decision;

 private:
  /// @brief The wait limit a first estimate sets.
  ///
  /// @param wait_estimate The estimate, in seconds.
  /// @return Twice the estimate, at most wait_limit_seconds;
  ///     default_wait_seconds when there is none.
  static auto wait_limit_for(int wait_estimate) noexcept -> int;
  /// @brief The wait limit in force.
  ///
  /// @return The limit, in seconds.
  auto wait_limit() const noexcept -> int;

  /// @brief How many polls have reported progress.
  int attempt_{};
  /// @brief Network errors in a row.
  int network_errors_{};
  /// @brief The seconds waited so far.
  int elapsed_{};
  /// @brief The wait limit, once the first progress set it.
  std::optional<int> wait_limit_;
};

}  // namespace libchiaking::play
