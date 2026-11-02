/// @file
/// @brief Declares PushClient, a Remote Play start's PSN push channel: it
/// connects the WebSocket, hands each stage the notifications it waits for,
/// holding the others back, and can hand the channel to a background
/// SignallingService once holepunching begins.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/psn/remote_play/push_error.hpp"
#include "src/core/psn/remote_play/push_feed.hpp"
#include "src/core/psn/remote_play/session_client.hpp"
#include "src/core/psn/remote_play/signal_barriers.hpp"
#include "src/core/psn/rest/push/push.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::remote_play {

/// @brief Which route the holepunch is negotiating, which tells the
/// signalling service how to treat repeated OFFERs.
enum class PsnServicePhase : std::uint8_t {
  /// @brief Negotiating the control (CTRL) route.
  control,
  /// @brief Negotiating the data route.
  data,
  /// @brief Both routes are up; the session is streaming.
  active
};
/// @brief The background reader of the push channel.
class SignallingService;

/// @brief The push channel of one Remote Play start.
///
/// Before start_service() the caller's thread reads the channel through
/// receive_notification(): notifications of other sessions are dropped, ones
/// another stage will want are queued, and the session's deletion ends the
/// wait. After start_service() a worker thread reads it and
/// receive_notification() takes from the worker's inbox instead.
///
/// Holds a reference to the WebSocket provider, which must outlive it.
/// Not copyable; stops its service when destroyed. Its members other than the
/// service's are for one thread.
class PushClient {
 public:
  /// @brief Prepares the channel.
  ///
  /// @param websocket The host's WebSocket provider; must outlive this
  ///     object.
  /// @param diagnostics Where notifications are logged; may be null.
  explicit PushClient(
      platform::WebSocketProvider& websocket,
      platform::DiagnosticsProvider* diagnostics = nullptr) noexcept;

  /// @brief Hands the channel to a background SignallingService for the
  /// holepunch.
  ///
  /// @param connection The push channel; must outlive the service.
  /// @param session The client that sends acknowledgements; must outlive the
  ///     service.
  /// @param threading Starts the worker thread and makes the service's lock
  ///     and condition variable.
  /// @param clock The monotonic clock the service's waits measure on; must
  ///     outlive the service.
  /// @param attempt The start attempt; not 0.
  /// @param token The Remote Play token; the service keeps its own copy.
  /// @param session_id The session; viewed, so it must outlive the service.
  /// @param account_id The account id acknowledgements are addressed to.
  /// @param uid The console's device unique id, if known; see
  ///     service_console_uid().
  /// @param family The console's generation.
  /// @param stop Stops the service.
  /// @return Nothing, once the worker is running.
  /// @retval platform::PlatformError::thread_failure A service is already
  ///     running, `attempt` is 0, or no thread could be started.
  /// @retval platform::PlatformError The threading provider's failure.
  [[nodiscard]] auto start_service(
      platform::WebSocketConnection& connection, SessionClient& session,
      platform::ThreadingProvider& threading,
      const platform::ClockProvider& clock, std::uint64_t attempt,
      const psn::auth::BearerToken& token, std::string_view session_id,
      std::int64_t account_id, std::array<std::byte, psn_device_uid_size> uid,
      psn::rest::ConsoleFamily family, platform::StopToken stop)
      -> Expected<void, platform::PlatformError>;
  /// @brief Moves the service on to a later phase; ignored without a service.
  ///
  /// @param attempt The start attempt; a stale one is ignored.
  /// @param phase The phase; one not after the current is ignored.
  void service_phase(std::uint64_t attempt, PsnServicePhase phase);
  /// @brief Tells the service which console acknowledgements are addressed
  /// to.
  ///
  /// @param uid The console's device unique id.
  void service_console_uid(std::array<std::byte, psn_device_uid_size> uid);
  /// @brief Stops and drops the service, joining its thread.
  void stop_service();
  /// @brief Stops the service's worker, keeping it so its failure can still
  /// be read.
  void quiesce_service();
  /// @brief Reads how the service failed.
  ///
  /// @return Its first failure, or nothing without one or without a service.
  auto service_failure() const -> std::optional<PushError>;
  /// @brief Limits receive_notification() to one session's notifications.
  ///
  /// @param session_id The session; empty receives every session's.
  void scope_session(std::string_view session_id);

  /// @brief Stops the service.
  ~PushClient();
  /// @brief Not copyable.
  PushClient(const PushClient&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never.
  PushClient& operator=(const PushClient&) = delete;
  /// @brief Logs which of the console-ready notifications have arrived.
  ///
  /// @param member_received Whether the console joined.
  /// @param data_received Whether customData1 arrived.
  void log_console_readiness(bool member_received,
                             bool data_received) const noexcept;

  /// @brief Opens the push channel (rest::PushChannelRequest), first stopping
  /// any service and forgetting the session scope and queued notifications.
  ///
  /// Blocks until connected, within the request's 30 s connect timeout.
  ///
  /// @param bearer_token The Remote Play token.
  /// @param push_fqdn The push server, from SessionClient::get_push_fqdn().
  /// @param stop Cancels the connect.
  /// @return The connection.
  /// @retval PushErrorKind::invalid_token The token is not usable.
  /// @retval PushErrorKind::invalid_fqdn The host cannot be put in a URL.
  /// @retval PushErrorKind::cancelled The stop token fired.
  /// @retval PushErrorKind::websocket_failure The connection failed.
  [[nodiscard]] auto connect(const psn::auth::BearerToken& bearer_token,
                             std::string_view push_fqdn,
                             const platform::StopToken& stop)
      -> Expected<std::unique_ptr<platform::WebSocketConnection>, PushError>;

  /// @brief Waits for the next notification of one of the accepted types.
  ///
  /// Without a service: queued notifications are looked at first; then the
  /// channel is read, notifications of other sessions ignored, unknown ones
  /// ignored and others queued for a later stage. With a service: taken from
  /// its inbox. Blocks until one arrives, the channel fails, or `stop` fires.
  ///
  /// @param connection The push channel; it must be the one first read.
  /// @param stop Ends the wait.
  /// @param accepted The types wanted; empty accepts any.
  /// @return The notification.
  /// @retval PushErrorKind::cancelled The stop token fired.
  /// @retval PushErrorKind::connection_mismatch Notifications are queued from
  ///     another connection.
  /// @retval PushErrorKind::session_deleted The scoped session was deleted
  ///     while specific types were awaited.
  /// @retval PushErrorKind::notification_queue_full The queue overflowed.
  /// @retval PushError The channel's failure.
  [[nodiscard]] auto receive_notification(
      platform::WebSocketConnection& connection,
      const platform::StopToken& stop,
      std::span<const psn::rest::NotificationType> accepted = {})
      -> Expected<psn::rest::Notification, PushError>;

  /// @brief Tells whether a notification concerns a session.
  ///
  /// @param notification The notification.
  /// @param session_id The session.
  /// @param require_id Whether one naming no session counts as not concerning
  ///     it.
  /// @return As rest::Notification::belongs_to_session().
  static bool notification_in_session(
      const psn::rest::Notification& notification, std::string_view session_id,
      bool require_id = false);

  /// @brief Waits for a notification until a deadline.
  ///
  /// @param connection The push channel.
  /// @param parent_stop The caller's stop token; a deadline on it counts as a
  ///     timeout.
  /// @param clock The monotonic clock the deadline is measured on.
  /// @param deadline_microseconds The deadline, absolute or per wait.
  /// @param accepted The types wanted; empty accepts any.
  /// @return The notification.
  /// @retval BoundedPushReceiveErrorKind::timed_out The deadline passed.
  /// @retval BoundedPushReceiveErrorKind::cancelled The caller's stop fired.
  /// @retval BoundedPushReceiveErrorKind::push_failure The channel failed.
  [[nodiscard]] auto receive_bounded(
      platform::WebSocketConnection& connection,
      platform::StopToken parent_stop, const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      std::span<const psn::rest::NotificationType> accepted = {})
      -> Expected<psn::rest::Notification, BoundedPushReceiveError>;

  /// @brief Waits at a CreationBarrier for the session-created and
  /// members-created notifications.
  ///
  /// @tparam Observe Called after each notification, as SignalBarrier::wait()
  ///     describes.
  /// @param barrier The barrier.
  /// @param attempt The start attempt.
  /// @param connection The push channel.
  /// @param parent_stop The caller's stop token.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param observe The observer.
  /// @return The online id the session was created for.
  /// @retval NotificationWaitFailure The barrier's failure, with the
  ///     channel's when it failed.
  template <class Observe = NoNotificationObservation>
  [[nodiscard]] auto await_creation(
      CreationBarrier& barrier, std::uint64_t attempt,
      platform::WebSocketConnection& connection,
      platform::StopToken parent_stop, const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds, Observe observe = {})
      -> Expected<std::string, NotificationWaitFailure>;

  /// @brief Waits at a ConsoleReadyBarrier for the console's members-created
  /// and customData1-updated notifications, logging progress before each
  /// receive and at the end.
  ///
  /// @tparam Observe Called after each notification.
  /// @param barrier The barrier.
  /// @param attempt The start attempt.
  /// @param connection The push channel.
  /// @param parent_stop The caller's stop token.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param observe The observer.
  /// @return The console's device id and customData1.
  /// @retval NotificationWaitFailure The barrier's failure, with the
  ///     channel's when it failed.
  template <class Observe = NoNotificationObservation>
  [[nodiscard]] auto await_console_ready(
      ConsoleReadyBarrier& barrier, std::uint64_t attempt,
      platform::WebSocketConnection& connection,
      platform::StopToken parent_stop, const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds, Observe observe = {})
      -> Expected<ConsoleReady, NotificationWaitFailure>;

 private:
  /// @brief Waits at a barrier, receiving its notifications with a deadline.
  ///
  /// @tparam Barrier A SignalBarrier.
  /// @tparam BeforeEach Callable as `before_each()`, run before each receive.
  /// @tparam Observe Called after each notification.
  /// @param barrier The barrier.
  /// @param attempt The start attempt.
  /// @param connection The push channel.
  /// @param parent_stop The caller's stop token.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param accepted The types the barrier needs.
  /// @param before_each Run before each receive.
  /// @param observe The observer.
  /// @return The barrier's value.
  /// @retval NotificationWaitFailure The barrier's failure, with the
  ///     channel's when it failed.
  template <class Barrier, class BeforeEach, class Observe>
  [[nodiscard]] auto await_barrier(
      Barrier& barrier, std::uint64_t attempt,
      platform::WebSocketConnection& connection,
      platform::StopToken parent_stop, const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      std::span<const psn::rest::NotificationType> accepted,
      BeforeEach before_each, Observe observe)
      -> Expected<decltype(barrier.value()), NotificationWaitFailure>;

  /// @brief The background reader, once started.
  std::shared_ptr<SignallingService> service_;
  /// @brief The session notifications are limited to; empty for any.
  std::string session_id_;
  /// @brief The host's WebSocket provider.
  platform::WebSocketProvider& websocket_;
  /// @brief The channel's notifications and the queue held for later stages.
  PushFeed feed_;
  /// @brief The connection the queue was filled from; null before the first
  /// receive.
  platform::WebSocketConnection* inbox_connection_{};
};

template <class Barrier, class BeforeEach, class Observe>
auto PushClient::await_barrier(
    Barrier& barrier, std::uint64_t attempt,
    platform::WebSocketConnection& connection, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds,
    std::span<const psn::rest::NotificationType> accepted,
    BeforeEach before_each, Observe observe)
    -> Expected<decltype(barrier.value()), NotificationWaitFailure> {
  std::optional<PushError> push_failure;
  auto result = barrier.wait(
      attempt,
      [&]() -> Expected<psn::rest::Notification, CreationWaitError> {
        before_each();
        auto received = receive_bounded(connection, parent_stop, clock,
                                        deadline_microseconds, accepted);
        if (received) return std::move(received.value());
        switch (received.error().kind) {
          case BoundedPushReceiveErrorKind::cancelled:
            return unexpected(CreationWaitError::cancelled);
          case BoundedPushReceiveErrorKind::timed_out:
            return unexpected(CreationWaitError::timed_out);
          case BoundedPushReceiveErrorKind::push_failure:
            push_failure = received.error().push;
            return unexpected(CreationWaitError::receive_failed);
        }
        return unexpected(CreationWaitError::receive_failed);
      },
      std::move(observe));
  if (!result)
    return unexpected(NotificationWaitFailure{result.error(), push_failure});
  return std::move(result.value());
}

template <class Observe>
auto PushClient::await_creation(CreationBarrier& barrier, std::uint64_t attempt,
                                platform::WebSocketConnection& connection,
                                platform::StopToken parent_stop,
                                const platform::ClockProvider& clock,
                                net::RemoteWaitDeadline deadline_microseconds,
                                Observe observe)
    -> Expected<std::string, NotificationWaitFailure> {
  constexpr std::array accepted{
      psn::rest::NotificationType::remote_play_session_created,
      psn::rest::NotificationType::members_created};
  return await_barrier(
      barrier, attempt, connection, std::move(parent_stop), clock,
      deadline_microseconds, accepted, [] {}, std::move(observe));
}

template <class Observe>
auto PushClient::await_console_ready(
    ConsoleReadyBarrier& barrier, std::uint64_t attempt,
    platform::WebSocketConnection& connection, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds, Observe observe)
    -> Expected<ConsoleReady, NotificationWaitFailure> {
  constexpr std::array accepted{
      psn::rest::NotificationType::members_created,
      psn::rest::NotificationType::custom_data1_updated};
  const auto report = [&] {
    log_console_readiness(barrier.device_uid().has_value(),
                          barrier.custom_data1().has_value());
  };
  auto result = await_barrier(
      barrier, attempt, connection, std::move(parent_stop), clock,
      deadline_microseconds, accepted, report, std::move(observe));
  report();
  return result;
}

}  // namespace libchiaking::psn::remote_play
