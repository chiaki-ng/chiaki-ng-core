/// @file
/// @brief Declares UnconfirmedSession, the rules for a home start whose
/// console never sent a session ID, so the link made one up.
#pragma once

#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"

#include <chrono>

namespace libchiaking::play {

/// @brief How a home start behaves when the link made up the session ID.
///
/// A console may accept a made-up ID and stream, so the start still tries,
/// but it waits less for the console to answer calibration and, if the
/// stream fails before it plays, ends the attempt as a control timeout so the
/// start policy opens a fresh session. A plain value; with a confirmed ID it
/// changes nothing.
class UnconfirmedSession {
 public:
  /// @brief The stream error type.
  using StreamError = net::protocol::remote_play::StreamError;
  /// @brief The stream state type.
  using StreamState = net::protocol::remote_play::StreamState;

  /// @brief The longest wait for the console to answer calibration with an
  /// unconfirmed session.
  static constexpr std::chrono::milliseconds connect_timeout{5'000};

  /// @brief Makes the rules for one start.
  ///
  /// @param unconfirmed Whether the link made up the session ID.
  explicit constexpr UnconfirmedSession(bool unconfirmed) noexcept
      : unconfirmed_(unconfirmed) {}

  /// @brief Shortens the calibration connect wait for an unconfirmed session.
  ///
  /// @param policy The calibration policy the start would use.
  /// @return The policy, with its connect timeout capped at connect_timeout
  ///     when the session is unconfirmed.
  [[nodiscard]] constexpr auto calibration(
      net::protocol::senkusha::Policy policy) const noexcept
      -> net::protocol::senkusha::Policy {
    if (unconfirmed_ && policy.connect_timeout > connect_timeout)
      policy.connect_timeout = connect_timeout;
    return policy;
  }

  /// @brief Reports whether a failure should end the attempt so a fresh
  /// session is opened.
  ///
  /// @param state The stream's state when it failed.
  /// @return `true` when the session is unconfirmed and the stream never
  ///     reached streaming.
  [[nodiscard]] constexpr auto starts_over(StreamState state) const noexcept
      -> bool {
    return unconfirmed_ && state != StreamState::streaming;
  }

  /// @brief Chooses the error a failed attempt reports.
  ///
  /// @param state The stream's state when it failed.
  /// @param error The error the stream reported.
  /// @return StreamError::ctrl_timeout when the attempt starts over, so the
  ///     start policy tries a fresh session; otherwise @p error.
  [[nodiscard]] constexpr auto failure(StreamState state,
                                       StreamError error) const noexcept
      -> StreamError {
    return starts_over(state) ? StreamError::ctrl_timeout : error;
  }

 private:
  /// @brief Whether the link made up the session ID.
  bool unconfirmed_{};
};

}  // namespace libchiaking::play
