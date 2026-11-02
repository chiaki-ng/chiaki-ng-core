/// @file
/// @brief Declares the account side of a Remote Play internet start: the push
/// connection, the session lease, opening a started session, and the
/// SignallingService that reads the push channel in the background while the
/// holepunch runs.
///
/// StartedSession::open() opens the push channel, creates the session manager
/// session and waits until PSN reports it created. Once holepunching begins,
/// the SignallingService takes over the channel: the console resends its
/// holepunch OFFERs until answered, so the service acknowledges repeats with a
/// RESULT and delivers each other notification to the holepunch code through
/// its inbox.
#pragma once

#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/psn_signalling_limits.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/remote_play/notification_inbox.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/push_feed.hpp"
#include "src/core/psn/remote_play/session_client.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/utils/json/json.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::psn::remote_play {

/// @brief Why the push channel could not be opened: one of four causes. An
/// immutable value.
class PushOpenError {
 public:
  /// @brief Which cause; the index of the cause in Cause.
  enum class Kind : std::uint8_t {
    /// @brief The token is empty.
    invalid_token,
    /// @brief The stop token fired before anything was sent.
    cancelled,
    /// @brief The push server could not be found.
    get_fqdn_failed,
    /// @brief The WebSocket did not connect.
    connect_failed,
  };
  /// @brief The token is empty.
  struct InvalidToken {};
  /// @brief The stop token fired before anything was sent.
  struct Cancelled {};
  /// @brief The push server lookup failed.
  struct GetFqdnFailed {
    /// @brief The lookup's failure.
    SessionError session;
  };
  /// @brief The WebSocket did not connect.
  struct ConnectFailed {
    /// @brief The connect's failure.
    PushError push;
  };
  /// @brief The cause, in Kind order.
  using Cause =
      std::variant<InvalidToken, Cancelled, GetFqdnFailed, ConnectFailed>;

  /// @brief Wraps a cause.
  ///
  /// @tparam C One of the cause types.
  /// @param cause The cause.
  template <class C>
    requires std::constructible_from<Cause, C>
  PushOpenError(C cause) noexcept : cause_(std::move(cause)) {}

  /// @brief Tells which cause.
  ///
  /// @return The kind.
  Kind kind() const noexcept;
  /// @brief Reaches the cause as one type.
  ///
  /// @tparam C A cause type.
  /// @return The cause, or null when it is another.
  template <class C>
  const C* as() const noexcept {
    return std::get_if<C>(&cause_);
  }

 private:
  /// @brief The cause.
  Cause cause_;
};

/// @brief An open push channel and the PushClient that reads it. Move-only by
/// way of its socket; closes nothing on destruction beyond the socket's own
/// cleanup.
class PushConnection {
 public:
  /// @brief Wraps an open channel.
  ///
  /// @param client The client reading it; must outlive this object.
  /// @param socket The connection.
  PushConnection(
      PushClient& client,
      std::unique_ptr<platform::WebSocketConnection> socket) noexcept;

  /// @brief Finds the push server and connects to it.
  ///
  /// @param session_client Finds the push server.
  /// @param push_client Connects, and reads the channel afterwards.
  /// @param bearer_token The Remote Play token.
  /// @param stop Cancels both requests.
  /// @return The connection.
  /// @retval PushOpenError The cause.
  [[nodiscard]] static auto open(SessionClient& session_client,
                                 PushClient& push_client,
                                 const psn::auth::BearerToken& bearer_token,
                                 const platform::StopToken& stop)
      -> Expected<PushConnection, PushOpenError>;

  /// @brief Tells whether the channel is open.
  ///
  /// @return False once closed or moved from.
  explicit operator bool() const noexcept { return socket_ != nullptr; }
  /// @brief Reaches the connection; it must be open.
  ///
  /// @return The connection.
  platform::WebSocketConnection& operator*() const noexcept { return *socket_; }
  /// @brief Reaches the client reading the channel.
  ///
  /// @return The client.
  PushClient& client() const noexcept { return *client_; }

  /// @brief Closes the channel; does nothing when already closed.
  ///
  /// @return Nothing, when closed cleanly.
  /// @retval platform::WebSocketError The close failed; the channel is
  ///     dropped regardless.
  [[nodiscard]] auto close() -> Expected<void, platform::WebSocketError>;

 private:
  /// @brief The client reading the channel; never null.
  PushClient* client_;
  /// @brief The connection; null once closed.
  std::unique_ptr<platform::WebSocketConnection> socket_;
};

/// @brief A session manager session this client created, and the right to
/// leave it once.
///
/// Move-only; moving marks the source as released so only one object leaves
/// the session. It does not leave the session on destruction: the owner calls
/// release(). Holds a reference to the session client, which must outlive it.
class SessionLease {
 public:
  /// @brief Not copyable.
  SessionLease(const SessionLease&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never.
  SessionLease& operator=(const SessionLease&) = delete;
  /// @brief Takes the lease over.
  ///
  /// @param other The lease; left marked as released.
  SessionLease(SessionLease&& other) noexcept;
  /// @brief Not assignable.
  ///
  /// @return Never.
  SessionLease& operator=(SessionLease&&) = delete;

  /// @brief Creates a session.
  ///
  /// @param client The session client; must outlive the lease.
  /// @param bearer_token The Remote Play token; kept for release().
  /// @param stop Cancels the request.
  /// @return The lease.
  /// @retval SessionError The failures of SessionClient::create_session().
  [[nodiscard]] static auto create(SessionClient& client,
                                   psn::auth::BearerToken&& bearer_token,
                                   const platform::StopToken& stop)
      -> Expected<SessionLease, SessionError>;

  /// @brief Reads the session.
  ///
  /// @return The session as created.
  const psn::rest::CreatedSession& created() const noexcept { return created_; }
  /// @brief Reads the Remote Play token.
  ///
  /// @return The token; secret; valid while this object lives.
  auto bearer_token() const noexcept -> const psn::auth::BearerToken& {
    return token_;
  }
  /// @brief Tells whether release() was called, or the lease moved away.
  ///
  /// @return True once it was.
  bool release_attempted() const noexcept { return release_attempted_; }

  /// @brief Leaves the session, once.
  ///
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request Release was already attempted.
  /// @retval SessionError The failures of SessionClient::release_session().
  [[nodiscard]] auto release(const platform::StopToken& stop)
      -> Expected<void, SessionError>;

 private:
  /// @brief Wraps a created session.
  ///
  /// @param client The session client.
  /// @param token The Remote Play token.
  /// @param created The session.
  SessionLease(SessionClient& client, psn::auth::BearerToken&& token,
               psn::rest::CreatedSession&& created) noexcept;

  /// @brief The session client.
  SessionClient& client_;
  /// @brief The Remote Play token.
  psn::auth::BearerToken token_;
  /// @brief The session.
  psn::rest::CreatedSession created_;
  /// @brief Whether release was attempted or the lease moved away.
  bool release_attempted_{};
};

/// @brief Why a session could not be started: one of six causes. An immutable
/// value.
class StartError {
 public:
  /// @brief Which cause; the index of the cause in Cause.
  enum class Kind : std::uint8_t {
    /// @brief The attempt is 0 or not the barrier's.
    invalid_attempt,
    /// @brief The deadline passed before the session was created.
    timed_out,
    /// @brief The stop token fired before anything was sent.
    cancelled,
    /// @brief The push channel could not be opened.
    push_open_failed,
    /// @brief The session could not be created.
    create_failed,
    /// @brief The session was created but its creation was not confirmed.
    wait_failed,
  };
  /// @brief The attempt is 0 or not the barrier's.
  struct InvalidAttempt {};
  /// @brief The deadline had passed.
  struct TimedOut {};
  /// @brief The stop token fired.
  struct Cancelled {};
  /// @brief The push channel could not be opened.
  struct PushOpenFailed {
    /// @brief Why.
    PushOpenError push_open;
  };
  /// @brief The session could not be created; the push channel was closed.
  struct CreateFailed {
    /// @brief The create request's failure.
    SessionError create;
    /// @brief The channel's close failure, if it failed.
    std::optional<platform::WebSocketError> close_failure;
  };
  /// @brief The wait for creation failed; the session was left (unless that
  /// was pointless) and the channel closed.
  struct WaitFailed {
    /// @brief Why the wait ended.
    CreationWaitError wait{};
    /// @brief The channel's failure, if it failed.
    std::optional<PushError> push;
    /// @brief Whether leaving the session was skipped because the channel
    /// had failed, the session was deleted, or it was already released.
    bool release_skipped{};
    /// @brief Whether a members-deleted or session-deleted notification
    /// confirmed the leave within net::psn_session_deletion_timeout_us.
    bool deletion_confirmed{};
    /// @brief The leave request's failure, if it failed.
    std::optional<SessionError> release_failure;
    /// @brief The channel's close failure, if it failed.
    std::optional<platform::WebSocketError> close_failure;
  };
  /// @brief The cause, in Kind order.
  using Cause = std::variant<InvalidAttempt, TimedOut, Cancelled,
                             PushOpenFailed, CreateFailed, WaitFailed>;

  /// @brief Wraps a cause.
  ///
  /// @tparam C One of the cause types.
  /// @param cause The cause.
  template <class C>
    requires std::constructible_from<Cause, C>
  StartError(C cause) noexcept : cause_(std::move(cause)) {}

  /// @brief Tells which cause.
  ///
  /// @return The kind.
  Kind kind() const noexcept;
  /// @brief Reaches the cause as one type.
  ///
  /// @tparam C A cause type.
  /// @return The cause, or null when it is another.
  template <class C>
  const C* as() const noexcept {
    return std::get_if<C>(&cause_);
  }

 private:
  /// @brief The cause.
  Cause cause_;
};

/// @brief A Remote Play session PSN confirmed created, with its push channel
/// and the online id it was created for. Move-only through its parts.
class StartedSession {
 public:
  /// @brief Assembles a started session.
  ///
  /// @param lease The session.
  /// @param connection Its push channel.
  /// @param online_id The online id PSN created it for.
  /// @param attempt The start attempt.
  StartedSession(SessionLease&& lease, PushConnection&& connection,
                 std::string online_id, std::uint64_t attempt) noexcept;

  /// @brief Opens the push channel, creates the session and waits until PSN
  /// confirms it (CreationBarrier), asking the session manager about it after
  /// each notification.
  ///
  /// On failure after creation the session is left and the channel closed
  /// before returning. Blocks for the whole start. The deadline is armed
  /// once on entry (net::RemoteWaitDeadline::arm), and opening the push
  /// channel and creating the session share that one armed instant, so a
  /// per-wait deadline bounds the two requests together as one wait. The
  /// wait for creation then arms `deadline_microseconds` afresh for each
  /// notification, and the session queries made during it are bounded by
  /// its absolute instant alone.
  ///
  /// @param session_client Makes the session requests.
  /// @param push_client Reads the channel.
  /// @param barrier The barrier for `attempt`.
  /// @param bearer_token The Remote Play token.
  /// @param attempt The start attempt.
  /// @param clock The monotonic clock.
  /// @param parent_stop The caller's stop token.
  /// @param deadline_microseconds The deadline.
  /// @return The started session.
  /// @retval StartError::TimedOut The armed deadline had passed on entry, or
  ///     passed while the push channel was opened or the session created; a
  ///     channel already open is closed first.
  /// @retval StartError Any other cause.
  [[nodiscard]] static auto open(SessionClient& session_client,
                                 PushClient& push_client,
                                 CreationBarrier& barrier,
                                 psn::auth::BearerToken&& bearer_token,
                                 std::uint64_t attempt,
                                 const platform::ClockProvider& clock,
                                 platform::StopToken parent_stop,
                                 net::RemoteWaitDeadline deadline_microseconds)
      -> Expected<StartedSession, StartError>;

  /// @brief Reaches the session.
  ///
  /// @return The lease.
  SessionLease& lease() noexcept { return lease_; }
  /// @brief Reads the session.
  ///
  /// @return The lease.
  const SessionLease& lease() const noexcept { return lease_; }
  /// @brief Reaches the push channel.
  ///
  /// @return The connection.
  PushConnection& connection() noexcept { return connection_; }
  /// @brief Reads the push channel.
  ///
  /// @return The connection.
  const PushConnection& connection() const noexcept { return connection_; }
  /// @brief Reads the online id PSN created the session for.
  ///
  /// @return The online id.
  const std::string& online_id() const noexcept { return online_id_; }
  /// @brief Reads the start attempt.
  ///
  /// @return The attempt number.
  std::uint64_t attempt() const noexcept { return attempt_; }

 private:
  /// @brief The lease and channel while creation is awaited, which leaves the
  /// session and closes the channel unless completed.
  class Startup;

  /// @brief The session.
  SessionLease lease_;
  /// @brief The push channel.
  PushConnection connection_;
  /// @brief The online id PSN created the session for.
  std::string online_id_;
  /// @brief The start attempt.
  std::uint64_t attempt_;
};

/// @brief Reads the push channel on a worker thread during the holepunch.
///
/// In the control phase it waits for the console's CTRL OFFER, then delivers
/// the ACCEPT; repeats of an OFFER it has already delivered are answered with
/// a RESULT sent through the session client, and OFFERs that arrive before
/// the CTRL ACCEPT are held and delivered when the data phase starts. In the
/// data phase the same happens for the DATA route. In the active phase
/// repeated OFFERs are still acknowledged but nothing more is delivered, and
/// the inbox is emptied. Notifications of other sessions are ignored; the
/// session's deletion fails the service.
///
/// Shared by its PushClient; it must not outlive the feed, connection, session
/// client, token and session id it was made with. phase(), console_uid(),
/// stop(), failure() and receive() may be called from any thread; receive()
/// sleeps until the inbox or the failure changes, or until its stop token or
/// the service ends.
class SignallingService {
 public:
  /// @brief Prepares the service; nothing runs until start().
  ///
  /// @param feed The channel's feed, also read for held notifications.
  /// @param connection The push channel.
  /// @param session Sends the acknowledgements.
  /// @param threading Makes the service's lock and condition variable; must
  ///     outlive the service.
  /// @param clock The monotonic clock the service's waits measure on; must
  ///     outlive the service.
  /// @param attempt The start attempt.
  /// @param token The Remote Play token; cloned into the service.
  /// @param session_id The session; viewed.
  /// @param account_id The account id acknowledgements are addressed to.
  /// @param uid The console's device unique id.
  /// @param family The console's generation.
  /// @param parent The caller's stop token.
  SignallingService(PushFeed& feed, platform::WebSocketConnection& connection,
                    SessionClient& session,
                    platform::ThreadingProvider& threading,
                    const platform::ClockProvider& clock, std::uint64_t attempt,
                    const psn::auth::BearerToken& token,
                    std::string_view session_id, std::int64_t account_id,
                    std::array<std::byte, psn_device_uid_size> uid,
                    psn::rest::ConsoleFamily family,
                    platform::StopToken parent);
  /// @brief Stops the worker and joins it.
  ~SignallingService();
  /// @brief Starts the worker thread, named `chiaki-psn`, in the control
  /// role.
  ///
  /// @param threading Starts the thread.
  /// @return Nothing, once it runs.
  /// @retval platform::PlatformError The thread could not be started.
  auto start(platform::ThreadingProvider& threading)
      -> Expected<void, platform::PlatformError>;
  /// @brief Moves the worker on to a later phase; the worker's pending
  /// receive is interrupted so it applies it at once.
  ///
  /// @param attempt The start attempt; a stale one is ignored and logged.
  /// @param phase The phase; one not after the current is ignored and logged.
  void phase(std::uint64_t attempt, PsnServicePhase phase);
  /// @brief Sets the console acknowledgements are addressed to.
  ///
  /// @param uid The console's device unique id.
  void console_uid(std::array<std::byte, psn_device_uid_size> uid);
  /// @brief Ends the worker, interrupting its receive, and joins it.
  void stop();
  /// @brief Reads how the worker failed.
  ///
  /// @return Its first failure, or nothing.
  auto failure() const -> std::optional<PushError>;
  /// @brief Takes the oldest delivered notification of an accepted type.
  ///
  /// Blocks until one is delivered, the worker fails, or `stop` or the
  /// service ends.
  ///
  /// @param connection The channel; must be the service's.
  /// @param stop Ends the wait.
  /// @param accepted The types wanted; empty accepts any.
  /// @return The notification.
  /// @retval PushErrorKind::connection_mismatch Another connection was given.
  /// @retval PushErrorKind::cancelled `stop` fired or the service ended.
  /// @retval PushError The worker's failure.
  auto receive(platform::WebSocketConnection& connection,
               const platform::StopToken& stop,
               std::span<const psn::rest::NotificationType> accepted)
      -> Expected<psn::rest::Notification, PushError>;

 private:
  /// @brief The most notifications the inbox holds, and the most early OFFERs
  /// held.
  static constexpr std::size_t max_inbox_notifications = 4096;
  /// @brief The most body bytes the inbox holds (16 MiB), and the most body
  /// bytes the early OFFERs hold together.
  static constexpr std::size_t max_inbox_bytes = 16U * 1024U * 1024U;
  /// @brief State: awaiting the CTRL OFFER.
  struct ControlOffer;
  /// @brief State: the CTRL OFFER was delivered.
  struct ControlSelected;
  /// @brief State: the CTRL route was accepted.
  struct ControlEstablished;
  /// @brief State: awaiting the DATA OFFER.
  struct DataOffer;
  /// @brief State: the DATA OFFER was delivered.
  struct DataSelected;
  /// @brief State: both routes are up.
  struct Active;
  /// @brief Event: the data phase began.
  struct BeginData;
  /// @brief Event: the active phase began.
  struct BeginActive;
  /// @brief Event: an OFFER arrived.
  struct Offer;
  /// @brief Event: an ACCEPT arrived.
  struct Accept;
  /// @brief What to do with an OFFER.
  enum class Disposition {
    /// @brief Deliver it to the holepunch code.
    deliver,
    /// @brief A repeat: answer it with a RESULT.
    acknowledge,
    /// @brief Arrived after the CTRL route was accepted; a repeat of the
    /// CTRL OFFER is ignored, any other delivered.
    after_control
  };
  /// @brief The disposition the state machine chose for the last OFFER.
  struct Decision;
  /// @brief The signalling state machine's transition table.
  struct SignallingFsm;

  /// @brief The worker's stop state: stopped, the parent stopped, or a phase
  /// change waiting to be applied.
  class StopState;
  /// @brief A stop token for acknowledgements that only the end of the
  /// service fires.
  class AckStop;
  /// @brief A stop token for receive() that fires when the receiver's own
  /// token does or the service ends.
  class ReceiveStop;
  /// @brief The worker thread's loop and state machine.
  class Worker;
  /// @brief The worker thread's entry; an exception fails the service as a
  /// WebSocket failure.
  ///
  /// @param context The service.
  static void entry(void* context) noexcept;
  /// @brief Runs the worker loop until the service ends or fails.
  void run();
  /// @brief Records the service's first failure and wakes receivers.
  ///
  /// @param error The failure.
  void fail(PushError error);
  /// @brief Logs a line through the feed.
  ///
  /// @param message The line; must hold no secret.
  void log(std::string_view message) const;
  /// @brief The channel's feed.
  PushFeed& feed_;
  /// @brief The push channel.
  platform::WebSocketConnection& connection_;
  /// @brief Sends the acknowledgements.
  SessionClient& session_;
  /// @brief The start attempt.
  std::uint64_t attempt_;
  /// @brief The service's own copy of the Remote Play token.
  psn::auth::BearerToken token_;
  /// @brief The session, viewed.
  std::string_view session_id_;
  /// @brief The account id acknowledgements are addressed to.
  std::int64_t account_id_;
  /// @brief The console acknowledgements are addressed to; guarded by mutex_.
  std::array<std::byte, psn_device_uid_size> uid_;
  /// @brief The console's generation.
  psn::rest::ConsoleFamily family_;
  /// @brief The worker's stop state; never null.
  std::shared_ptr<StopState> stop_;
  /// @brief The worker thread, while running.
  std::unique_ptr<platform::native::Thread> thread_;
  /// @brief Guards the inbox, the failure and the console id.
  mutable platform::Mutex mutex_;
  /// @brief Signalled when the inbox or the failure changes, or the service
  /// ends.
  platform::ConditionVariable changed_;
  /// @brief The notifications delivered to the holepunch code.
  NotificationInbox inbox_{max_inbox_notifications, max_inbox_bytes};
  /// @brief The service's first failure.
  std::optional<PushError> failure_;
};

}  // namespace libchiaking::psn::remote_play
