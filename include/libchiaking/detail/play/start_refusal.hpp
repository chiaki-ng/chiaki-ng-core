/// @file
/// @brief Declares StartRefusal, the reason Play declined to start a session.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/play/launch_refused.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace libchiaking::play {

/// @brief Says why a session was not started.
///
/// A StartRefusal is the error of every Play start function. It means no
/// session exists: nothing was sent to SessionEvents and there is no Outcome.
/// Failures that happen after a session has been returned are reported through
/// Outcome instead.
class StartRefusal {
 public:
  /// @brief Names the reason for the refusal.
  enum class Kind : std::uint8_t {
    /// @brief Another session is live, or a console is being paired over
    /// the internet.
    busy,
    /// @brief The player is not paired with the console, or the console is
    /// not on their PSN account.
    not_paired,
    /// @brief The console cannot be addressed; StartRefusal::unreachable
    /// says why.
    console_unreachable,
    /// @brief The PSN account lists the console with remote play turned
    /// off.
    remote_play_disabled,
    /// @brief The player's PSN sign-in has expired; they need to sign in
    /// again.
    sign_in_expired,
    /// @brief The start needs a PSN account and the player is not signed in
    /// to one.
    needs_psn_sign_in,
    /// @brief An argument was missing or the preferences cannot form a
    /// valid request.
    invalid_preferences,
    /// @brief A cloud launch could not be prepared; StartRefusal::launch
    /// says why.
    launch_refused,
    /// @brief The caller's stop token was triggered before the session was
    /// created.
    cancelled,
    /// @brief The host could not start the session's thread.
    host_refused,
  };

  /// @brief Builds a Kind::busy refusal.
  ///
  /// @return The Kind::busy refusal.
  static auto busy() noexcept -> StartRefusal {
    return StartRefusal{Kind::busy};
  }
  /// @brief Builds a Kind::not_paired refusal.
  ///
  /// @return The Kind::not_paired refusal.
  static auto not_paired() noexcept -> StartRefusal {
    return StartRefusal{Kind::not_paired};
  }
  /// @brief Builds a Kind::console_unreachable refusal.
  ///
  /// @param why Why the console cannot be addressed.
  /// @return The Kind::console_unreachable refusal.
  static auto console_unreachable(consoles::Unreachable why) noexcept
      -> StartRefusal {
    StartRefusal refusal{Kind::console_unreachable};
    refusal.unreachable_ = why;
    return refusal;
  }
  /// @brief Builds a Kind::remote_play_disabled refusal.
  ///
  /// @return The Kind::remote_play_disabled refusal.
  static auto remote_play_disabled() noexcept -> StartRefusal {
    return StartRefusal{Kind::remote_play_disabled};
  }
  /// @brief Builds a Kind::sign_in_expired refusal.
  ///
  /// @return The Kind::sign_in_expired refusal.
  static auto sign_in_expired() noexcept -> StartRefusal {
    return StartRefusal{Kind::sign_in_expired};
  }
  /// @brief Builds a Kind::needs_psn_sign_in refusal.
  ///
  /// @return The Kind::needs_psn_sign_in refusal.
  static auto needs_psn_sign_in() noexcept -> StartRefusal {
    return StartRefusal{Kind::needs_psn_sign_in};
  }
  /// @brief Builds a Kind::invalid_preferences refusal.
  ///
  /// @return The Kind::invalid_preferences refusal.
  static auto invalid_preferences() noexcept -> StartRefusal {
    return StartRefusal{Kind::invalid_preferences};
  }
  /// @brief Builds a Kind::cancelled refusal.
  ///
  /// @return The Kind::cancelled refusal.
  static auto cancelled() noexcept -> StartRefusal {
    return StartRefusal{Kind::cancelled};
  }
  /// @brief Builds a Kind::host_refused refusal.
  ///
  /// @return The Kind::host_refused refusal.
  static auto host_refused() noexcept -> StartRefusal {
    return StartRefusal{Kind::host_refused};
  }
  /// @brief Builds a Kind::launch_refused refusal.
  ///
  /// @param refused Why the cloud launch could not be prepared.
  /// @return The Kind::launch_refused refusal.
  static auto launch_refused(LaunchRefused refused) -> StartRefusal {
    StartRefusal refusal{Kind::launch_refused};
    refusal.launch_ = std::move(refused);
    return refusal;
  }

  /// @brief Returns the reason for the refusal.
  ///
  /// @return The reason for the refusal.
  auto kind() const noexcept -> Kind { return kind_; }
  /// @brief Returns why the console cannot be addressed; set only for
  /// Kind::console_unreachable.
  ///
  /// @return Why the console cannot be addressed; set only for
  ///     Kind::console_unreachable.
  auto unreachable() const noexcept -> std::optional<consoles::Unreachable> {
    return unreachable_;
  }
  /// @brief Returns why the cloud launch could not be prepared; set only for
  /// Kind::launch_refused.
  ///
  /// @return Why the cloud launch could not be prepared; set only for
  ///     Kind::launch_refused.
  auto launch() const noexcept -> const std::optional<LaunchRefused>& {
    return launch_;
  }

 private:
  explicit StartRefusal(Kind kind) noexcept : kind_(kind) {}

  Kind kind_;
  std::optional<consoles::Unreachable> unreachable_{};
  std::optional<LaunchRefused> launch_{};
};

}  // namespace libchiaking::play
