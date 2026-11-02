/// @file
/// @brief Declares the failures of the PSN push channel and of waiting on it.
#pragma once

#include <libchiaking/platform/websocket/provider.hpp>
#include "src/core/psn/remote_play/session_client.hpp"
#include "src/core/psn/remote_play/signal_barriers.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::psn::remote_play {

/// @brief Why the push channel failed.
enum class PushErrorKind : std::uint8_t {
  /// @brief The Remote Play token is not usable; nothing was sent.
  invalid_token,
  /// @brief The push server host cannot be put in a URL.
  invalid_fqdn,
  /// @brief The stop token fired, or a deadline passed.
  cancelled,
  /// @brief The WebSocket could not connect or a receive failed.
  websocket_failure,
  /// @brief A notification or phase change broke the signalling protocol.
  invalid_notification,
  /// @brief Notifications held for later stages exceeded the queue.
  notification_queue_full,
  /// @brief A receive used another connection than the one being served.
  connection_mismatch,
  /// @brief The session manager deleted the session.
  session_deleted,
  /// @brief The session was terminated; not produced at present.
  session_terminated,
  /// @brief Acknowledging a message failed; not produced at present.
  acknowledgement_failed,
};

/// @brief A push channel failure. Plain data.
struct PushError {
  /// @brief Why.
  PushErrorKind kind{};
  /// @brief The WebSocket provider's error, for websocket_failure.
  platform::WebSocketError backend{platform::WebSocketError::backend_failure};
  /// @brief An acknowledgement's failure; read by callers, but nothing in
  /// the library sets it at present.
  std::optional<SessionError> acknowledgement{};
};

/// @brief Why a receive with a deadline ended without a notification.
enum class BoundedPushReceiveErrorKind : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The deadline passed.
  timed_out,
  /// @brief The channel failed.
  push_failure,
};

/// @brief A receive with a deadline that failed. Plain data.
struct BoundedPushReceiveError {
  /// @brief Why.
  BoundedPushReceiveErrorKind kind{};
  /// @brief The channel's failure, for push_failure; the cancellation
  /// otherwise.
  PushError push{};
};

/// @brief A wait for session notifications that failed. Plain data.
struct NotificationWaitFailure {
  /// @brief Why the wait ended.
  CreationWaitError kind;
  /// @brief The channel's failure, for receive_failed.
  std::optional<PushError> push;
};

}  // namespace libchiaking::psn::remote_play
