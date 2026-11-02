/// @file
/// @brief Declares PsnFailure, why a PSN step of an internet start failed:
/// a session request, the push connection, a notification wait or the
/// session start.
#pragma once

#include "src/core/psn/remote_play/session_client.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace libchiaking::play {

/// @brief A PSN failure of an internet start, with its text, cancellation and
/// HTTP status. A value.
class PsnFailure {
 public:
  /// @brief A wait for a PSN notification that failed.
  struct Wait {
    /// @brief Why the wait failed.
    psn::remote_play::CreationWaitError error{};
    /// @brief The push connection's error, when it caused the failure.
    std::optional<psn::remote_play::PushError> push{};
  };

  /// @brief A session request error, a push error, a wait failure, or a start
  /// failure.
  using Cause =
      std::variant<psn::remote_play::SessionError, psn::remote_play::PushError,
                   Wait, psn::remote_play::StartError>;

  /// @brief Builds the failure.
  ///
  /// @param cause The cause.
  explicit PsnFailure(Cause cause) noexcept : cause_(std::move(cause)) {}

  /// @brief Reads the cause.
  ///
  /// @return The cause.
  auto cause() const noexcept -> const Cause& { return cause_; }
  /// @brief Reads the cause as one alternative.
  ///
  /// @tparam T One of the Cause alternatives.
  /// @return The cause when it is a @p T; null otherwise.
  template <class T>
  auto as() const noexcept -> const T* {
    return std::get_if<T>(&cause_);
  }
  /// @brief Describes the failure.
  ///
  /// @return A fixed sentence for the innermost cause.
  auto describe() const noexcept -> std::string_view;
  /// @brief Reports whether the failure was a cancellation.
  ///
  /// @return `true` when the innermost cause was cancelled.
  auto cancelled() const noexcept -> bool;
  /// @brief Gives the HTTP status behind the failure.
  ///
  /// @return The status; 0 when none.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief Reports whether the push connection refused the account's token,
  /// so the account lacks access.
  ///
  /// @return `true` for a start whose push open failed with an invalid token.
  auto lacks_access() const noexcept -> bool;
  /// @brief Reports whether the failure was because this machine had no
  /// network.
  ///
  /// @return `true` for a session request, alone or inside a start, whose
  ///     HTTP transport could not connect or was disconnected; a timeout, an
  ///     HTTP status or any other failure is not.
  auto offline() const noexcept -> bool;

  /// @brief Describes a session request error.
  ///
  /// @param error The error.
  /// @return A fixed sentence for its kind.
  static auto describe(const psn::remote_play::SessionError& error) noexcept
      -> std::string_view;
  /// @brief Describes a push error, or its acknowledgement's error.
  ///
  /// @param error The error.
  /// @return A fixed sentence for its kind.
  static auto describe(const psn::remote_play::PushError& error) noexcept
      -> std::string_view;
  /// @brief Describes a wait failure, or its push error.
  ///
  /// @param wait The failure.
  /// @return A fixed sentence.
  static auto describe(const Wait& wait) noexcept -> std::string_view;
  /// @brief Describes a start failure, or the failure inside it.
  ///
  /// @param error The error.
  /// @return A fixed sentence.
  static auto describe(const psn::remote_play::StartError& error) noexcept
      -> std::string_view;
  /// @brief Reports whether a session request was cancelled.
  ///
  /// @param error The error.
  /// @return `true` for a cancellation.
  static auto cancelled(const psn::remote_play::SessionError& error) noexcept
      -> bool;
  /// @brief Reports whether a push error was a cancellation.
  ///
  /// @param error The error.
  /// @return `true` for a cancellation, or a cancelled acknowledgement.
  static auto cancelled(const psn::remote_play::PushError& error) noexcept
      -> bool;
  /// @brief Reports whether a wait was cancelled.
  ///
  /// @param wait The failure.
  /// @return `true` for a cancellation.
  static auto cancelled(const Wait& wait) noexcept -> bool;
  /// @brief Reports whether a start was cancelled.
  ///
  /// @param error The error.
  /// @return `true` for a cancellation, here or inside.
  static auto cancelled(const psn::remote_play::StartError& error) noexcept
      -> bool;
  /// @brief Gives a session request's HTTP status.
  ///
  /// @param error The error.
  /// @return The status.
  static auto http_status(const psn::remote_play::SessionError& error) noexcept
      -> std::uint16_t;
  /// @brief Gives a push error's HTTP status.
  ///
  /// @param error The error.
  /// @return The acknowledgement's status; 0 otherwise.
  static auto http_status(const psn::remote_play::PushError& error) noexcept
      -> std::uint16_t;
  /// @brief Gives a wait failure's HTTP status.
  ///
  /// @param wait The failure.
  /// @return The push error's status; 0 otherwise.
  static auto http_status(const Wait& wait) noexcept -> std::uint16_t;
  /// @brief Gives a start failure's HTTP status.
  ///
  /// @param error The error.
  /// @return The inner failure's status; 0 otherwise.
  static auto http_status(const psn::remote_play::StartError& error) noexcept
      -> std::uint16_t;

 private:
  /// @brief Finds the failure inside a start failure.
  ///
  /// @param error The start failure.
  /// @return The session request, wait or push error inside; empty when the
  ///     start failed on its own.
  static auto unwrapped(const psn::remote_play::StartError& error) noexcept
      -> std::optional<Cause>;

  /// @brief The cause.
  Cause cause_;
};

}  // namespace libchiaking::play
