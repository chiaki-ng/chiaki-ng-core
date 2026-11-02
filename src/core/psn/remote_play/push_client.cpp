#include "src/core/psn/remote_play/push_client.hpp"

#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

namespace libchiaking::psn::remote_play {

PushClient::PushClient(platform::WebSocketProvider& websocket,
                       platform::DiagnosticsProvider* diagnostics) noexcept
    : websocket_(websocket), feed_(diagnostics) {}

void PushClient::scope_session(std::string_view session_id) {
  session_id_ = session_id;
}

auto PushClient::connect(const psn::auth::BearerToken& bearer_token,
                         std::string_view push_fqdn,
                         const platform::StopToken& stop)
    -> Expected<std::unique_ptr<platform::WebSocketConnection>, PushError> {
  stop_service();
  session_id_.clear();
  feed_.pending().clear();
  inbox_connection_ = nullptr;
  if (!SessionClient::valid_token(bearer_token))
    return unexpected(PushError{PushErrorKind::invalid_token});
  auto channel = psn::rest::PushChannelRequest::create(
      SessionClient::bearer(bearer_token), push_fqdn);
  if (!channel) return unexpected(PushError{PushErrorKind::invalid_fqdn});
  if (stop.stop_requested())
    return unexpected(PushError{PushErrorKind::cancelled});

  auto connection = websocket_.connect(channel.value().request(), stop);
  if (!connection) {
    if (connection.error() == platform::WebSocketError::cancelled)
      return unexpected(PushError{PushErrorKind::cancelled});
    return unexpected(
        PushError{PushErrorKind::websocket_failure, connection.error()});
  }
  return std::move(connection.value());
}

void PushClient::log_console_readiness(bool member_received,
                                       bool data_received) const noexcept {
  const std::string_view status =
      member_received
          ? (data_received ? "ready: console joined and session data received"
                           : "waiting for session data; console joined")
          : (data_received
                 ? "waiting for console membership; session data received"
                 : "waiting for console membership and session data");
  try {
    feed_.note(std::format("PSN readiness: {}", status));
  } catch (...) {
  }
}

auto PushClient::receive_notification(
    platform::WebSocketConnection& connection, const platform::StopToken& stop,
    std::span<const psn::rest::NotificationType> accepted)
    -> Expected<psn::rest::Notification, PushError> {
  if (service_) return service_->receive(connection, stop, accepted);
  if (stop.stop_requested())
    return unexpected(PushError{PushErrorKind::cancelled});
  auto& pending = feed_.pending();
  if (inbox_connection_ != &connection) {
    if (!pending.empty())
      return unexpected(PushError{PushErrorKind::connection_mismatch});
    inbox_connection_ = &connection;
  }
  const auto matches = [&](psn::rest::NotificationType type) {
    return accepted.empty() ||
           std::ranges::find(accepted, type) != accepted.end();
  };
  const auto deleted = [&](const psn::rest::Notification& notification) {
    return notification.type() ==
               psn::rest::NotificationType::remote_play_session_deleted &&
           !session_id_.empty() &&
           notification_in_session(notification, session_id_, true) &&
           !accepted.empty();
  };
  bool session_deleted = false;
  using Verdict = NotificationInbox::Verdict;
  auto queued = pending.scan([&](const psn::rest::Notification& item) {
    if (!session_id_.empty() && !notification_in_session(item, session_id_))
      return Verdict::drop;
    if (deleted(item)) {
      session_deleted = true;
      return Verdict::stop;
    }
    return matches(item.type()) ? Verdict::take : Verdict::keep;
  });
  if (session_deleted)
    return unexpected(PushError{PushErrorKind::session_deleted});
  if (queued) {
    feed_.log(queued->type(), "delivered_from_queue");
    return std::move(*queued);
  }
  while (!stop.stop_requested()) {
    auto received = PushFeed::receive(connection, stop);
    if (!received) return unexpected(received.error());
    if (!session_id_.empty() &&
        !notification_in_session(received.value(), session_id_)) {
      feed_.log(received.value().type(), "ignored_unrelated_session");
      continue;
    }
    if (deleted(received.value()))
      return unexpected(PushError{PushErrorKind::session_deleted});
    if (matches(received.value().type())) {
      feed_.log(received.value().type(), "delivered");
      return std::move(received.value());
    }
    if (received.value().type() == psn::rest::NotificationType::unknown ||
        received.value().type() ==
            psn::rest::NotificationType::invitation_created ||
        received.value().type() ==
            psn::rest::NotificationType::lounge_group_changed) {
      feed_.log(received.value().type(), "ignored");
      continue;
    }
    feed_.log(received.value().type(), "queued_for_another_stage");
    if (!pending.push_back(std::move(received.value())))
      return unexpected(PushError{PushErrorKind::notification_queue_full});
  }
  return unexpected(PushError{PushErrorKind::cancelled});
}

bool PushClient::notification_in_session(
    const psn::rest::Notification& notification, std::string_view session_id,
    bool require_id) {
  return notification.belongs_to_session(session_id, require_id);
}

auto PushClient::receive_bounded(
    platform::WebSocketConnection& connection, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds,
    std::span<const psn::rest::NotificationType> accepted)
    -> Expected<psn::rest::Notification, BoundedPushReceiveError> {
  if (parent_stop.stop_requested())
    return unexpected(BoundedPushReceiveError{
        parent_stop.deadline_expired() ? BoundedPushReceiveErrorKind::timed_out
                                       : BoundedPushReceiveErrorKind::cancelled,
        {}});
  const auto deadline = deadline_microseconds.arm(clock);
  if (clock.now_microseconds() >= deadline)
    return unexpected(
        BoundedPushReceiveError{BoundedPushReceiveErrorKind::timed_out, {}});

  auto deadline_state = std::make_shared<net::DeadlineStopState>(
      std::move(parent_stop), clock, deadline);
  auto received = receive_notification(
      connection, platform::StopToken{deadline_state}, accepted);
  if (received) return std::move(received.value());
  auto error = received.error();
  if (error.kind == PushErrorKind::cancelled) {
    const auto kind = deadline_state->timed_out()
                          ? BoundedPushReceiveErrorKind::timed_out
                          : BoundedPushReceiveErrorKind::cancelled;
    return unexpected(BoundedPushReceiveError{kind, error});
  }
  return unexpected(BoundedPushReceiveError{
      BoundedPushReceiveErrorKind::push_failure, error});
}

auto PushClient::start_service(
    platform::WebSocketConnection& connection, SessionClient& session,
    platform::ThreadingProvider& threading,
    const platform::ClockProvider& clock, std::uint64_t attempt,
    const psn::auth::BearerToken& token, std::string_view session_id,
    std::int64_t account_id, std::array<std::byte, psn_device_uid_size> uid,
    psn::rest::ConsoleFamily family, platform::StopToken stop)
    -> Expected<void, platform::PlatformError> {
  if (service_ || attempt == 0)
    return unexpected(platform::PlatformError::thread_failure);
  service_ = std::make_shared<SignallingService>(
      feed_, connection, session, threading, clock, attempt, token, session_id,
      account_id, uid, family, std::move(stop));
  auto started = service_->start(threading);
  if (!started) service_.reset();
  return started;
}

PushClient::~PushClient() { stop_service(); }

void PushClient::service_phase(std::uint64_t attempt, PsnServicePhase phase) {
  if (service_) service_->phase(attempt, phase);
}
void PushClient::service_console_uid(
    std::array<std::byte, psn_device_uid_size> uid) {
  if (service_) service_->console_uid(uid);
}
void PushClient::quiesce_service() {
  if (service_) service_->stop();
}
void PushClient::stop_service() {
  if (service_) service_->stop();
  service_.reset();
}
auto PushClient::service_failure() const -> std::optional<PushError> {
  return service_ ? service_->failure() : std::nullopt;
}

}  // namespace libchiaking::psn::remote_play
