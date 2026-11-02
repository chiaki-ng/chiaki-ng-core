/// @file
/// @brief Declares Accept, the ACCEPT half of PSN hole punching:
/// telling the console which candidate answered, waiting for its ACCEPT, and
/// handing the selected socket over as a route. Also the wait and drive that
/// order the exchange.
#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "src/core/psn/remote_play/holepunch_message.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::play::internet::holepunch {

/// @brief Effect: acknowledge the console's ACCEPT with a RESULT message.
struct AckPeerAccept {
  /// @brief The attempt.
  std::uint64_t attempt{};
  /// @brief The console's request id.
  std::uint16_t peer_req_id{};
};

/// @brief Where an accept exchange stands.
enum class AcceptWaitState : std::uint8_t {
  /// @brief Not begun.
  idle,
  /// @brief Waiting for the console's ACCEPT.
  waiting_accept,
  /// @brief The ACCEPT arrived; its acknowledgement is not sent yet.
  ack_pending,
  /// @brief Acknowledged; the route can be handed over.
  ready,
  /// @brief Timed out, stopped or terminated.
  failed,
};

/// @brief The state machine of one accept exchange, as Boost.SML.
///
/// Messages for another attempt are ignored. Not thread-safe; movable.
class AcceptWait {
 public:
  /// @brief Builds an idle wait.
  AcceptWait();
  /// @brief Releases the machine.
  ~AcceptWait();
  /// @brief Moves the wait.
  AcceptWait(AcceptWait&&) noexcept;
  /// @brief Moves the wait.
  ///
  /// @return This wait.
  AcceptWait& operator=(AcceptWait&&) noexcept;
  /// @brief Begins an exchange.
  ///
  /// @param attempt The attempt; not 0.
  /// @return `true` when it began; `false` when not idle.
  [[nodiscard]] bool begin(std::uint64_t attempt);
  /// @brief Takes a PSN hole-punch message while waiting: an ACCEPT asks for
  /// its acknowledgement, a TERMINATE fails the exchange; others are ignored.
  ///
  /// @param attempt The attempt.
  /// @param message The message.
  void on_message(std::uint64_t attempt,
                  const net::nat::holepunch::SessionMessage& message);
  /// @brief Takes the acknowledgement asked for.
  ///
  /// @return The acknowledgement; empty when none is asked for.
  [[nodiscard]] auto take_ack() -> std::optional<AckPeerAccept>;
  /// @brief Records that the acknowledgement was sent.
  ///
  /// @param attempt The attempt.
  /// @return `true` when the exchange is now ready.
  [[nodiscard]] bool ack_sent(std::uint64_t attempt);
  /// @brief Fails the exchange while it waits.
  ///
  /// @param attempt The attempt.
  /// @return `true` when it failed now.
  [[nodiscard]] bool timeout(std::uint64_t attempt);
  /// @brief Fails the exchange while it waits, as AcceptWait::timeout.
  ///
  /// @param attempt The attempt.
  /// @return `true` when it failed now.
  [[nodiscard]] bool cancel(std::uint64_t attempt);
  /// @brief Reads where the exchange stands.
  ///
  /// @return The state.
  auto state() const noexcept -> AcceptWaitState;

 private:
  /// @brief The Boost.SML machine; defined in the implementation.
  class Machine;
  /// @brief The machine.
  std::unique_ptr<Machine> machine_;
  /// @brief The attempt.
  std::uint64_t attempt_{};
  /// @brief The acknowledgement not yet taken.
  std::optional<AckPeerAccept> effect_{};
};

/// @brief Why acknowledging the console's ACCEPT failed.
enum class AcceptAckErrorKind : std::uint8_t {
  /// @brief The acknowledgement is for another attempt, or attempt 0.
  stale_attempt,
  /// @brief The request id is out of range; not produced by Accept::send_ack.
  invalid_req_id,
  /// @brief PSN refused the message.
  session_failure,
};

/// @brief Why acknowledging the console's ACCEPT failed: a refusal before sending, or PSN's error.
class AcceptAckError {
 public:
  /// @brief The acknowledgement is for another attempt, or attempt 0.
  /// @return The error.
  static constexpr auto stale_attempt() noexcept -> AcceptAckError {
    return AcceptAckError{AcceptAckErrorKind::stale_attempt};
  }
  /// @brief The request id is out of range; not produced by Accept::send_ack.
  /// @return The error.
  static constexpr auto invalid_req_id() noexcept -> AcceptAckError {
    return AcceptAckError{AcceptAckErrorKind::invalid_req_id};
  }
  /// @brief PSN refused the message.
  /// @param session PSN's error.
  /// @return The error.
  static constexpr auto refused(psn::remote_play::SessionError session) noexcept
      -> AcceptAckError {
    return AcceptAckError{session};
  }

  /// @brief What failed.
  /// @return The kind.
  constexpr auto kind() const noexcept -> AcceptAckErrorKind {
    return session_ ? AcceptAckErrorKind::session_failure : kind_;
  }
  /// @brief PSN's error, for AcceptAckErrorKind::session_failure.
  /// @return The error, or nothing for a refusal before sending.
  constexpr auto session() const noexcept
      -> std::optional<psn::remote_play::SessionError> {
    return session_;
  }

 private:
  /// @brief Makes a refusal before sending.
  /// @param kind What failed.
  constexpr explicit AcceptAckError(AcceptAckErrorKind kind) noexcept
      : kind_(kind) {}
  /// @brief Makes PSN's refusal.
  /// @param session PSN's error.
  constexpr explicit AcceptAckError(
      psn::remote_play::SessionError session) noexcept
      : kind_(AcceptAckErrorKind::session_failure), session_(session) {}

  /// @brief The refusal before sending.
  AcceptAckErrorKind kind_;
  /// @brief PSN's error.
  std::optional<psn::remote_play::SessionError> session_;
};

/// @brief Why sending our ACCEPT failed.
enum class AcceptSendErrorKind : std::uint8_t {
  /// @brief The send is for another attempt, or attempt 0.
  stale_attempt,
  /// @brief Our request id is 0 or above 65535.
  invalid_req_id,
  /// @brief PSN refused the message.
  session_failure,
};

/// @brief Why sending our ACCEPT failed: a refusal before sending, or PSN's error.
class AcceptSendError {
 public:
  /// @brief The send is for another attempt, or attempt 0.
  /// @return The error.
  static constexpr auto stale_attempt() noexcept -> AcceptSendError {
    return AcceptSendError{AcceptSendErrorKind::stale_attempt};
  }
  /// @brief Our request id is 0 or above 65535.
  /// @return The error.
  static constexpr auto invalid_req_id() noexcept -> AcceptSendError {
    return AcceptSendError{AcceptSendErrorKind::invalid_req_id};
  }
  /// @brief PSN refused the message.
  /// @param session PSN's error.
  /// @return The error.
  static constexpr auto refused(psn::remote_play::SessionError session) noexcept
      -> AcceptSendError {
    return AcceptSendError{session};
  }

  /// @brief What failed.
  /// @return The kind.
  constexpr auto kind() const noexcept -> AcceptSendErrorKind {
    return session_ ? AcceptSendErrorKind::session_failure : kind_;
  }
  /// @brief PSN's error, for AcceptSendErrorKind::session_failure.
  /// @return The error, or nothing for a refusal before sending.
  constexpr auto session() const noexcept
      -> std::optional<psn::remote_play::SessionError> {
    return session_;
  }

 private:
  /// @brief Makes a refusal before sending.
  /// @param kind What failed.
  constexpr explicit AcceptSendError(AcceptSendErrorKind kind) noexcept
      : kind_(kind) {}
  /// @brief Makes PSN's refusal.
  /// @param session PSN's error.
  constexpr explicit AcceptSendError(
      psn::remote_play::SessionError session) noexcept
      : kind_(AcceptSendErrorKind::session_failure), session_(session) {}

  /// @brief The refusal before sending.
  AcceptSendErrorKind kind_;
  /// @brief PSN's error.
  std::optional<psn::remote_play::SessionError> session_;
};

/// @brief What failed while waiting for the console's ACCEPT through PSN.
enum class AcceptPsnErrorKind : std::uint8_t {
  /// @brief The attempt was 0, or the exchange could not begin.
  invalid_attempt,
  /// @brief The wait was stopped.
  cancelled,
  /// @brief The deadline passed.
  timed_out,
  /// @brief The push connection failed.
  push_failure,
  /// @brief A session message could not be decoded.
  invalid_message,
  /// @brief Sending the acknowledgement failed.
  send_failure,
  /// @brief The console terminated the exchange, or it asked for no
  /// acknowledgement.
  terminated,
};

/// @brief Why waiting for the console's ACCEPT failed, with the receive's or
/// acknowledgement's error that caused it.
class AcceptPsnError {
 public:
  /// @brief Makes an error with no receive or send behind it.
  /// @param kind What failed.
  constexpr explicit AcceptPsnError(AcceptPsnErrorKind kind) noexcept
      : kind_(kind) {}
  /// @brief Makes the error of a failed receive: cancelled, timed out or a
  /// push failure, as the receive says.
  /// @param push The receive's error.
  /// @return The error.
  static constexpr auto received(
      psn::remote_play::BoundedPushReceiveError push) noexcept
      -> AcceptPsnError {
    using Push = psn::remote_play::BoundedPushReceiveErrorKind;
    const auto kind =
        push.kind == Push::cancelled   ? AcceptPsnErrorKind::cancelled
        : push.kind == Push::timed_out ? AcceptPsnErrorKind::timed_out
                                       : AcceptPsnErrorKind::push_failure;
    return AcceptPsnError{kind, Cause{push}};
  }
  /// @brief Makes the error of a failed acknowledgement.
  /// @param kind AcceptPsnErrorKind::cancelled or timed_out when the
  ///     send was stopped, otherwise send_failure.
  /// @param send The acknowledgement's error.
  /// @return The error.
  static constexpr auto sent(AcceptPsnErrorKind kind,
                             AcceptAckError send) noexcept -> AcceptPsnError {
    return AcceptPsnError{kind, Cause{send}};
  }

  /// @brief What failed.
  /// @return The kind.
  constexpr auto kind() const noexcept -> AcceptPsnErrorKind { return kind_; }
  /// @brief The push receive's error, when receiving failed.
  /// @return The error, or nothing.
  constexpr auto push() const noexcept
      -> std::optional<psn::remote_play::BoundedPushReceiveError> {
    if (const auto* push =
            std::get_if<psn::remote_play::BoundedPushReceiveError>(&cause_))
      return *push;
    return std::nullopt;
  }
  /// @brief The acknowledgement's error, when sending it failed.
  /// @return The error, or nothing.
  constexpr auto send() const noexcept -> std::optional<AcceptAckError> {
    if (const auto* send = std::get_if<AcceptAckError>(&cause_)) return *send;
    return std::nullopt;
  }

 private:
  /// @brief The receive's or acknowledgement's error behind the failure, if
  /// any.
  using Cause =
      std::variant<std::monostate, psn::remote_play::BoundedPushReceiveError,
                   AcceptAckError>;

  /// @brief Makes an error with its cause.
  /// @param kind What failed.
  /// @param cause What caused it.
  constexpr AcceptPsnError(AcceptPsnErrorKind kind, Cause cause) noexcept
      : kind_(kind), cause_(cause) {}

  /// @brief What failed.
  AcceptPsnErrorKind kind_;
  /// @brief What caused it.
  Cause cause_;
};

/// @brief Drives an accept exchange: receives messages until the console's
/// ACCEPT, then sends its acknowledgement.
///
/// Blocks in the receive and send functions. Use on one thread; run once.
class AcceptDrive {
 public:
  /// @brief Receives the next hole-punch message; blocks.
  using Receive = std::function<
      Expected<net::nat::holepunch::SessionMessage, AcceptPsnError>()>;
  /// @brief Sends the acknowledgement; blocks.
  using SendAck =
      std::function<Expected<void, AcceptPsnError>(const AckPeerAccept&)>;

  /// @brief Builds the drive.
  ///
  /// @param receive Receives messages.
  /// @param send_ack Sends the acknowledgement.
  /// @param trace Records the exchange's states; may be null.
  AcceptDrive(Receive receive, SendAck send_ack,
              const diagnostics::Trace* trace = nullptr);

  /// @brief Runs the exchange; blocks until it finishes.
  ///
  /// @param attempt The attempt; not 0.
  /// @return The ready wait, moved out of the drive.
  /// @retval AcceptPsnErrorKind::invalid_attempt The exchange could
  ///     not begin.
  /// @retval AcceptPsnErrorKind::terminated The console terminated
  ///     the exchange, or asked for no acknowledgement.
  /// @retval AcceptPsnErrorKind::send_failure The acknowledgement
  ///     was sent for a stale attempt.
  /// @retval AcceptPsnError The receive's or send's error, unchanged.
  [[nodiscard]] auto run(std::uint64_t attempt)
      -> Expected<AcceptWait, AcceptPsnError>;

 private:
  /// @brief Records the exchange's state.
  void trace_state() const noexcept;
  /// @brief The exchange.
  AcceptWait wait_{};
  /// @brief Receives messages.
  Receive receive_;
  /// @brief Sends the acknowledgement.
  SendAck send_ack_;
  /// @brief Records the states; may be null.
  const diagnostics::Trace* trace_;
};

/// @brief Why our ACCEPT for the selected candidate could not be built.
enum class SelectedAcceptError : std::uint8_t {
  /// @brief The selected index is not one of the console's candidates.
  candidate_index_out_of_range,
  /// @brief Our offer does not have a local candidate, a static candidate and
  /// a trailing local candidate.
  malformed_local_offer,
  /// @brief A session id is above 65535.
  sid_out_of_range,
};

/// @brief Which step of the selected ACCEPT exchange failed.
enum class SelectedAcceptExchangeErrorKind : std::uint8_t {
  /// @brief Building our ACCEPT.
  build_failed,
  /// @brief Sending our ACCEPT.
  send_failed,
  /// @brief Waiting for the console's ACCEPT.
  wait_failed,
  /// @brief Answering the console's late probes on the selected socket.
  late_probe_failed,
};

/// @brief Why the selected ACCEPT exchange failed: the error of the step that
/// failed.
class SelectedAcceptExchangeError {
 public:
  /// @brief Building our ACCEPT failed.
  /// @param build The build error.
  constexpr explicit SelectedAcceptExchangeError(
      SelectedAcceptError build) noexcept
      : cause_(build) {}
  /// @brief Sending our ACCEPT failed.
  /// @param send The send error.
  constexpr explicit SelectedAcceptExchangeError(AcceptSendError send) noexcept
      : cause_(send) {}
  /// @brief Waiting for the console's ACCEPT failed.
  /// @param wait The wait error.
  constexpr explicit SelectedAcceptExchangeError(AcceptPsnError wait) noexcept
      : cause_(wait) {}
  /// @brief Answering the console's late probes failed.
  /// @param probe The late probe error.
  constexpr explicit SelectedAcceptExchangeError(
      net::nat::holepunch::ProbeWaitError probe) noexcept
      : cause_(probe) {}

  /// @brief The failed step.
  /// @return The step.
  constexpr auto kind() const noexcept -> SelectedAcceptExchangeErrorKind {
    return static_cast<SelectedAcceptExchangeErrorKind>(cause_.index());
  }
  /// @brief The build error.
  /// @return The error, or nothing when another step failed.
  constexpr auto build() const noexcept -> std::optional<SelectedAcceptError> {
    return of<SelectedAcceptError>();
  }
  /// @brief The send error.
  /// @return The error, or nothing when another step failed.
  constexpr auto send() const noexcept -> std::optional<AcceptSendError> {
    return of<AcceptSendError>();
  }
  /// @brief The wait error.
  /// @return The error, or nothing when another step failed.
  constexpr auto wait() const noexcept -> std::optional<AcceptPsnError> {
    return of<AcceptPsnError>();
  }
  /// @brief The late probe error.
  /// @return The error, or nothing when another step failed.
  constexpr auto probe() const noexcept
      -> std::optional<net::nat::holepunch::ProbeWaitError> {
    return of<net::nat::holepunch::ProbeWaitError>();
  }

 private:
  /// @brief The failed step's error, in the order of
  /// SelectedAcceptExchangeErrorKind.
  using Cause =
      std::variant<SelectedAcceptError, AcceptSendError, AcceptPsnError,
                   net::nat::holepunch::ProbeWaitError>;

  /// @brief One step's error.
  /// @tparam T The step's error type.
  /// @return The error, or nothing when another step failed.
  template <class T>
  constexpr auto of() const noexcept -> std::optional<T> {
    if (const auto* cause = std::get_if<T>(&cause_)) return *cause;
    return std::nullopt;
  }

  /// @brief The failed step's error.
  Cause cause_;
};

/// @brief A completed ACCEPT exchange. Plain data.
struct SelectedAcceptExchangeResult {
  /// @brief The selected candidate and its socket.
  net::nat::holepunch::SelectedCandidate selected;
  /// @brief The ready wait.
  AcceptWait ready;
};

/// @brief Why the selected socket could not be handed over.
enum class CtrlRudpHandoffError : std::uint8_t {
  /// @brief The accept exchange is not ready.
  accept_not_ready,
  /// @brief The selection holds no socket.
  null_socket,
  /// @brief The socket belongs to another network provider.
  foreign_provider,
  /// @brief The console's endpoint is unspecified, has port 0 or is of
  /// another family than the socket.
  invalid_peer,
  /// @brief The socket has no bound local port.
  unbound_socket,
  /// @brief The socket could not be adopted as a connected transport.
  adoption_failed,
};

/// @brief A hole-punched socket handed over as a connected route. Plain data.
struct CtrlRudpHandoff {
  /// @brief The connected transport.
  std::unique_ptr<net::transport::datagram::ProviderUdpTransport> transport;
  /// @brief Our endpoint.
  platform::SocketEndpoint local_endpoint;
  /// @brief The console's endpoint.
  platform::SocketEndpoint peer_endpoint;
  /// @brief The local address the registration sends; set by
  /// Accept::transport_ctrl.
  std::string registration_local_address{};
  /// @brief The console's session id; 0 until the caller sets it.
  std::uint32_t console_sid{};
};

/// @brief Which half of opening the control route failed.
enum class CtrlAcceptTransportErrorKind : std::uint8_t {
  /// @brief The ACCEPT exchange.
  exchange,
  /// @brief The handover.
  handoff,
};

/// @brief Why opening the control route failed: the error of the half that
/// failed.
class CtrlAcceptTransportError {
 public:
  /// @brief The ACCEPT exchange failed.
  /// @param exchange The exchange's error.
  constexpr explicit CtrlAcceptTransportError(
      SelectedAcceptExchangeError exchange) noexcept
      : cause_(exchange) {}
  /// @brief The handover failed.
  /// @param handoff The handover's error.
  constexpr explicit CtrlAcceptTransportError(
      CtrlRudpHandoffError handoff) noexcept
      : cause_(handoff) {}

  /// @brief The failed half.
  /// @return The half.
  constexpr auto kind() const noexcept -> CtrlAcceptTransportErrorKind {
    return static_cast<CtrlAcceptTransportErrorKind>(cause_.index());
  }
  /// @brief The exchange's error.
  /// @return The error, or nothing when the handover failed.
  constexpr auto exchange() const noexcept
      -> std::optional<SelectedAcceptExchangeError> {
    if (const auto* exchange =
            std::get_if<SelectedAcceptExchangeError>(&cause_))
      return *exchange;
    return std::nullopt;
  }
  /// @brief The handover's error.
  /// @return The error, or nothing when the exchange failed.
  constexpr auto handoff() const noexcept
      -> std::optional<CtrlRudpHandoffError> {
    if (const auto* handoff = std::get_if<CtrlRudpHandoffError>(&cause_))
      return *handoff;
    return std::nullopt;
  }

 private:
  /// @brief The failed half's error, in the order of
  /// CtrlAcceptTransportErrorKind.
  std::variant<SelectedAcceptExchangeError, CtrlRudpHandoffError> cause_;
};

/// @brief The ACCEPT half of PSN hole punching. Static functions only; the
/// PSN and network calls block.
class Accept {
 public:
  /// @brief Acknowledges the console's ACCEPT with a RESULT; blocks on PSN.
  ///
  /// @param ack The acknowledgement.
  /// @param current_attempt The attempt now running.
  /// @param client Sends PSN requests.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param stop Stops the request.
  /// @return Nothing; the reason it was not sent.
  [[nodiscard]] static auto send_ack(
      const AckPeerAccept& ack, std::uint64_t current_attempt,
      psn::remote_play::SessionClient& client,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, const platform::StopToken& stop)
      -> Expected<void, AcceptAckError>;

  /// @brief Reports whether an address is one of the private-range prefixes,
  /// compared whole.
  ///
  /// @param address The address.
  /// @return `true` when it equals one of the prefixes.
  static bool legacy_private_stub(std::string_view address) noexcept;

  /// @brief Fills a candidate's mapped address: our local candidate's for a
  /// local candidate or a derived one at a private stub, else our public one.
  ///
  /// @param selected The candidate.
  /// @param local Our local candidate.
  /// @param remote Our public candidate.
  /// @return The candidate with its mapped address.
  static auto prepare_candidate(const net::nat::holepunch::Candidate& selected,
                                const net::nat::holepunch::Candidate& local,
                                const net::nat::holepunch::Candidate& remote)
      -> net::nat::holepunch::Candidate;

  /// @brief Builds our ACCEPT naming one candidate.
  ///
  /// @param sid Our session id.
  /// @param peer_sid The console's session id.
  /// @param selected The candidate.
  /// @param local Our local candidate.
  /// @param remote Our public candidate.
  /// @return The connection request.
  static auto build_request(std::uint32_t sid, std::uint32_t peer_sid,
                            const net::nat::holepunch::Candidate& selected,
                            const net::nat::holepunch::Candidate& local,
                            const net::nat::holepunch::Candidate& remote)
      -> net::nat::holepunch::ConnRequest;

  /// @brief Sends our ACCEPT through PSN; blocks on PSN.
  ///
  /// @param attempt The send's attempt.
  /// @param current_attempt The attempt now running.
  /// @param local_req_id Our request id; 1 to 65535.
  /// @param request Our ACCEPT.
  /// @param client Sends PSN requests.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param stop Stops the request.
  /// @return Nothing; the reason it was not sent.
  [[nodiscard]] static auto send_request(
      std::uint64_t attempt, std::uint64_t current_attempt,
      std::uint64_t local_req_id,
      const net::nat::holepunch::ConnRequest& request,
      psn::remote_play::SessionClient& client,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, const platform::StopToken& stop)
      -> Expected<void, AcceptSendError>;

  /// @brief Builds and sends our ACCEPT; blocks on PSN.
  ///
  /// @param attempt The send's attempt.
  /// @param current_attempt The attempt now running.
  /// @param local_req_id Our request id; 1 to 65535.
  /// @param sid Our session id.
  /// @param peer_sid The console's session id.
  /// @param selected The candidate.
  /// @param local Our local candidate.
  /// @param remote Our public candidate.
  /// @param client Sends PSN requests.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param stop Stops the request.
  /// @return Nothing; the reason it was not sent.
  [[nodiscard]] static auto send(
      std::uint64_t attempt, std::uint64_t current_attempt,
      std::uint64_t local_req_id, std::uint32_t sid, std::uint32_t peer_sid,
      const net::nat::holepunch::Candidate& selected,
      const net::nat::holepunch::Candidate& local,
      const net::nat::holepunch::Candidate& remote,
      psn::remote_play::SessionClient& client,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, const platform::StopToken& stop)
      -> Expected<void, AcceptSendError>;

  /// @brief Waits through PSN for the console's ACCEPT and acknowledges it;
  /// blocks up to the deadline. Only session-message notifications count.
  ///
  /// @param attempt The attempt; not 0.
  /// @param session Sends PSN requests.
  /// @param push Receives PSN notifications.
  /// @param connection The push connection.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param parent_stop The caller's stop.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param trace Records the exchange's states; may be null.
  /// @return The ready wait; the failure.
  [[nodiscard]] static auto run_psn(
      std::uint64_t attempt, psn::remote_play::SessionClient& session,
      psn::remote_play::PushClient& push,
      platform::WebSocketConnection& connection,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
      const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<AcceptWait, AcceptPsnError>;

  /// @brief Decodes a PSN notification into the wait.
  ///
  /// @param wait The wait.
  /// @param attempt The attempt.
  /// @param notification The notification.
  /// @return Nothing; the decode error.
  [[nodiscard]] static auto feed_wait(
      AcceptWait& wait, std::uint64_t attempt,
      const psn::rest::Notification& notification)
      -> Expected<void, psn::remote_play::HolepunchMessage::Error>;

  /// @brief Builds our ACCEPT for the candidate the probe selected: the
  /// console's candidate, or a derived one at the endpoint that answered.
  ///
  /// @param ours Our offer.
  /// @param console The console's offer.
  /// @param selected The selected candidate.
  /// @return The connection request; the reason it could not be built.
  [[nodiscard]] static auto build_selected(
      const PreparedOffer& ours,
      const net::nat::holepunch::ConnRequest& console,
      const net::nat::holepunch::SelectedCandidate& selected)
      -> Expected<net::nat::holepunch::ConnRequest, SelectedAcceptError>;

  /// @brief Sends our ACCEPT, waits for the console's and acknowledges it,
  /// then answers the console's late probes on the selected socket. Blocks up
  /// to the deadline.
  ///
  /// @param network Creates the poller for the late probes.
  /// @param ours Our offer.
  /// @param console The console's offer.
  /// @param selected The selected candidate and its socket.
  /// @param attempt The attempt.
  /// @param local_req_id Our request id; 1 to 65535.
  /// @param session Sends PSN requests.
  /// @param push Receives PSN notifications.
  /// @param connection The push connection.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param parent_stop The caller's stop.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param trace Records the steps; may be null.
  /// @return The selection and the ready wait; the failed step.
  [[nodiscard]] static auto run_selected_exchange(
      platform::NetworkProvider& network, const PreparedOffer& ours,
      const net::nat::holepunch::ConnRequest& console,
      net::nat::holepunch::SelectedCandidate selected, std::uint64_t attempt,
      std::uint64_t local_req_id, psn::remote_play::SessionClient& session,
      psn::remote_play::PushClient& push,
      platform::WebSocketConnection& connection,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
      const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<SelectedAcceptExchangeResult, SelectedAcceptExchangeError>;

  /// @brief Hands the selected socket over as a connected transport.
  ///
  /// @param network The network provider that must own the socket.
  /// @param selected The selected candidate and its socket; consumed.
  /// @param accept_wait The exchange; must be ready.
  /// @return The handover, without a registration address or console session
  ///     id; the reason it failed.
  [[nodiscard]] static auto handoff_ctrl_rudp(
      platform::NetworkProvider& network,
      net::nat::holepunch::SelectedCandidate selected,
      const AcceptWait& accept_wait)
      -> Expected<CtrlRudpHandoff, CtrlRudpHandoffError>;

  /// @brief Opens the control route: runs the selected ACCEPT exchange and
  /// hands the socket over, with our local address for the registration.
  /// Blocks up to the deadline.
  ///
  /// @param network The network provider.
  /// @param ours Our offer.
  /// @param console The console's offer.
  /// @param selected The selected candidate and its socket.
  /// @param attempt The attempt.
  /// @param local_req_id Our request id; 1 to 65535.
  /// @param session Sends PSN requests.
  /// @param push Receives PSN notifications.
  /// @param connection The push connection.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param parent_stop The caller's stop.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param trace Records the steps; may be null.
  /// @return The handover; the failed half.
  [[nodiscard]] static auto transport_ctrl(
      platform::NetworkProvider& network, const PreparedOffer& ours,
      const net::nat::holepunch::ConnRequest& console,
      net::nat::holepunch::SelectedCandidate selected, std::uint64_t attempt,
      std::uint64_t local_req_id, psn::remote_play::SessionClient& session,
      psn::remote_play::PushClient& push,
      platform::WebSocketConnection& connection,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
      const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<CtrlRudpHandoff, CtrlAcceptTransportError>;

 private:
  /// @brief Names a failed acknowledgement: cancelled when the caller stopped
  /// it, timed out when the exchange's deadline passed, otherwise a send
  /// failure.
  ///
  /// @param error The acknowledgement's error.
  /// @param parent The caller's stop token.
  /// @param send_stop The stop state the send ran under.
  /// @return The kind.
  static auto send_failure_kind(const AcceptAckError& error,
                                const platform::StopToken& parent,
                                const net::DeadlineStopState& send_stop)
      -> AcceptPsnErrorKind;
};

}  // namespace libchiaking::play::internet::holepunch
