/// @file
/// @brief Declares PushFeed, the source of push notifications for one Remote
/// Play start: notifications held back earlier, then the WebSocket.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "src/core/psn/remote_play/notification_inbox.hpp"
#include "src/core/psn/remote_play/push_error.hpp"
#include "src/core/psn/rest/push/push.hpp"

#include <string_view>

namespace libchiaking::psn::remote_play {

/// @brief Reads push notifications, first from those held for later stages,
/// then from the connection, and logs what becomes of each.
///
/// Owned by a PushClient. Not thread-safe; the signalling service's worker
/// reads it only while the client does not.
class PushFeed {
 public:
  /// @brief Makes a feed with an empty queue of net::psn_push_pending_limit
  /// notifications or net::psn_push_pending_bytes.
  ///
  /// @param diagnostics Where notifications are logged; may be null.
  explicit PushFeed(platform::DiagnosticsProvider* diagnostics) noexcept;

  /// @brief Waits for the next message on the connection.
  ///
  /// Blocks until a message arrives, the connection fails or `stop` fires.
  ///
  /// @param connection The push channel.
  /// @param stop Ends the wait.
  /// @return The message as a notification; a body that is not JSON gives an
  ///     unknown notification, not a failure.
  /// @retval PushErrorKind::cancelled The stop token fired.
  /// @retval PushErrorKind::websocket_failure The receive failed.
  [[nodiscard]] static auto receive(platform::WebSocketConnection& connection,
                                    const platform::StopToken& stop)
      -> Expected<psn::rest::Notification, PushError>;
  /// @brief Gives the oldest held notification, or waits for the next one.
  ///
  /// @param connection The push channel.
  /// @param stop Ends the wait.
  /// @return The notification.
  /// @retval PushError The failures of receive().
  [[nodiscard]] auto next(platform::WebSocketConnection& connection,
                          const platform::StopToken& stop)
      -> Expected<psn::rest::Notification, PushError>;

  /// @brief Logs what became of a notification, with the queue's size.
  ///
  /// @param type The notification's type.
  /// @param disposition What became of it, such as `delivered`.
  void log(psn::rest::NotificationType type,
           std::string_view disposition) const noexcept;
  /// @brief Logs a line at info level, when there is a log.
  ///
  /// @param message The line; must hold no secret.
  void note(std::string_view message) const noexcept;

  /// @brief Reaches the notifications held for later stages.
  ///
  /// @return The queue.
  auto pending() noexcept -> NotificationInbox& { return pending_; }
  /// @brief Reaches the log.
  ///
  /// @return It, or null.
  auto diagnostics() const noexcept -> platform::DiagnosticsProvider* {
    return diagnostics_;
  }

 private:
  /// @brief Names a notification type for the log.
  ///
  /// @param type The type.
  /// @return Its name, such as `session_message`.
  static auto type_name(psn::rest::NotificationType type) noexcept
      -> std::string_view;

  /// @brief Where notifications are logged; may be null.
  platform::DiagnosticsProvider* diagnostics_;
  /// @brief The notifications held for later stages.
  NotificationInbox pending_;
};

}  // namespace libchiaking::psn::remote_play
