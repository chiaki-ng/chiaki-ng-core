#include "src/core/psn/remote_play/push_feed.hpp"

#include "src/core/net/psn_signalling_limits.hpp"

#include <format>
#include <string>

namespace libchiaking::psn::remote_play {

PushFeed::PushFeed(platform::DiagnosticsProvider* diagnostics) noexcept
    : diagnostics_(diagnostics),
      pending_(net::psn_push_pending_limit, net::psn_push_pending_bytes) {}

auto PushFeed::receive(platform::WebSocketConnection& connection,
                       const platform::StopToken& stop)
    -> Expected<psn::rest::Notification, PushError> {
  if (stop.stop_requested())
    return unexpected(PushError{PushErrorKind::cancelled});
  auto message = connection.receive(stop);
  if (!message) {
    if (message.error() == platform::WebSocketError::cancelled)
      return unexpected(PushError{PushErrorKind::cancelled});
    return unexpected(
        PushError{PushErrorKind::websocket_failure, message.error()});
  }
  return psn::rest::Notification::parse(message.value().payload.bytes());
}

auto PushFeed::next(platform::WebSocketConnection& connection,
                    const platform::StopToken& stop)
    -> Expected<psn::rest::Notification, PushError> {
  if (auto queued = pending_.take_front()) return std::move(*queued);
  return receive(connection, stop);
}

auto PushFeed::type_name(psn::rest::NotificationType type) noexcept
    -> std::string_view {
  switch (type) {
    case psn::rest::NotificationType::remote_play_session_created:
      return "session_created";
    case psn::rest::NotificationType::members_created:
      return "member_created";
    case psn::rest::NotificationType::custom_data1_updated:
      return "session_data_updated";
    case psn::rest::NotificationType::session_message_created:
      return "session_message";
    case psn::rest::NotificationType::members_deleted:
      return "member_deleted";
    case psn::rest::NotificationType::remote_play_session_deleted:
      return "session_deleted";
    case psn::rest::NotificationType::invitation_created:
      return "invitation_created";
    case psn::rest::NotificationType::lounge_group_changed:
      return "lounge_group_changed";
    case psn::rest::NotificationType::unknown:
      break;
  }
  return "unknown";
}

void PushFeed::log(psn::rest::NotificationType type,
                   std::string_view disposition) const noexcept {
  if (!diagnostics_) return;
  try {
    note(std::format("PSN notification: type={} disposition={} pending={}",
                     type_name(type), disposition, pending_.size()));
  } catch (...) {
  }
}

void PushFeed::note(std::string_view message) const noexcept {
  if (diagnostics_) diagnostics_->write({platform::LogSeverity::info, message});
}

}  // namespace libchiaking::psn::remote_play
