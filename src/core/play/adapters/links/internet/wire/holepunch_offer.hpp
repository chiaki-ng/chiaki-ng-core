/// @file
/// @brief Declares Offer, the OFFER half of PSN hole punching:
/// trading connection requests with the console through PSN session
/// messages, then probing the console's candidates. Also the wait, inbox and
/// drive that order the trade.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/psn/remote_play/holepunch_message.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play::internet::holepunch {

/// @brief Effect: acknowledge the console's offer with a RESULT message.
struct AckPeerOffer {
  /// @brief The attempt.
  std::uint64_t attempt;
  /// @brief The console's request id; at most 65535.
  std::uint64_t peer_req_id;
  /// @brief The error sent back; 0 for none, at most 65535.
  std::uint32_t error{0};
};

/// @brief Effect: send our own OFFER.
struct SendOwnOffer {
  /// @brief The attempt.
  std::uint64_t attempt;
  /// @brief Our request id; 1 to 65535.
  std::uint64_t own_req_id;
};

/// @brief A message the offer trade asks to send.
using OfferEffect = std::variant<AckPeerOffer, SendOwnOffer>;

/// @brief Why the first effects could not be made.
enum class OfferEffectError {
  /// @brief The attempt was 0.
  invalid_attempt,
  /// @brief The console's request id is above 65535.
  invalid_peer_req_id,
  /// @brief Our request id was 0.
  invalid_own_req_id,
};

using net::nat::holepunch::SessionMessage;

/// @brief Where an offer trade stands.
enum class OfferWaitState {
  /// @brief Not begun.
  idle,
  /// @brief Waiting for the console's offer.
  waiting_offer,
  /// @brief Offer acknowledged and ours sent; waiting for the console's
  /// RESULT.
  waiting_result,
  /// @brief The trade is done; the console's candidates can be probed.
  ready_to_probe,
  /// @brief Timed out, stopped or terminated.
  failed,
};

/// @brief The state machine of one offer trade, as Boost.SML.
///
/// Messages for another attempt are ignored. Not thread-safe; movable.
class OfferWait {
 public:
  /// @brief Builds an idle wait.
  OfferWait();
  /// @brief Releases the machine.
  ~OfferWait();
  /// @brief Moves the wait.
  OfferWait(OfferWait&&) noexcept;
  /// @brief Moves the wait.
  ///
  /// @return This wait.
  OfferWait& operator=(OfferWait&&) noexcept;
  /// @brief Begins a trade.
  ///
  /// @param attempt The attempt; not 0.
  /// @param own_req_id Our request id; not 0.
  /// @return `true` when it began; `false` when not idle.
  [[nodiscard]] bool begin(std::uint64_t attempt, std::uint64_t own_req_id);
  /// @brief Takes a PSN hole-punch message: an OFFER with a connection
  /// request, a RESULT or a TERMINATE; others are ignored.
  ///
  /// @param attempt The attempt.
  /// @param message The message.
  void on_message(std::uint64_t attempt,
                  const net::nat::holepunch::SessionMessage& message);
  /// @brief Takes the console's offer. The first asks to acknowledge it and
  /// send ours; a repeat while waiting for the RESULT only asks to acknowledge
  /// it again.
  ///
  /// @param attempt The attempt.
  /// @param peer_req_id The console's request id; above 65535 is ignored.
  void on_console_offer(std::uint64_t attempt, std::uint64_t peer_req_id);
  /// @brief Takes the console's RESULT for our offer, finishing the trade.
  ///
  /// @param attempt The attempt.
  /// @param req_id The request id it answers; others are ignored.
  /// @param error The result's error; not read.
  void on_result(std::uint64_t attempt, std::uint64_t req_id,
                 std::uint32_t error);
  /// @brief Fails the trade while it waits.
  ///
  /// @param attempt The attempt.
  /// @return `true` when the trade failed now.
  [[nodiscard]] bool timeout(std::uint64_t attempt);
  /// @brief Fails the trade while it waits, as OfferWait::timeout.
  ///
  /// @param attempt The attempt.
  /// @return `true` when the trade failed now.
  [[nodiscard]] bool cancel(std::uint64_t attempt);
  /// @brief Takes the effects asked for.
  ///
  /// @return The effects, oldest first.
  std::vector<OfferEffect> take_effects();
  /// @brief Reads where the trade stands.
  ///
  /// @return The state.
  OfferWaitState state() const noexcept;

 private:
  /// @brief The Boost.SML machine; defined in the implementation.
  class Machine;
  /// @brief The machine.
  std::unique_ptr<Machine> machine_;
  /// @brief The attempt.
  std::uint64_t attempt_{};
  /// @brief Our request id.
  std::uint64_t own_req_id_{};
  /// @brief Effects not yet taken.
  std::vector<OfferEffect> effects_{};
};

/// @brief An offer trade that also keeps the console's connection request
/// until the trade is done. Not thread-safe.
class OfferInbox {
 public:
  /// @brief Begins a trade, forgetting an earlier console request.
  ///
  /// @param attempt The attempt; not 0.
  /// @param own_req_id Our request id; not 0.
  /// @return `true` when it began.
  [[nodiscard]] bool begin(std::uint64_t attempt, std::uint64_t own_req_id);

  /// @brief Takes a PSN hole-punch message, keeping the console's first
  /// connection request.
  ///
  /// @param attempt The attempt.
  /// @param message The message.
  void on_message(std::uint64_t attempt,
                  net::nat::holepunch::SessionMessage&& message);

  /// @brief Fails the trade while it waits, forgetting the console's request.
  ///
  /// @param attempt The attempt.
  /// @return `true` when the trade failed now.
  [[nodiscard]] bool timeout(std::uint64_t attempt);

  /// @brief Fails the trade while it waits, forgetting the console's request.
  ///
  /// @param attempt The attempt.
  /// @return `true` when the trade failed now.
  [[nodiscard]] bool cancel(std::uint64_t attempt);

  /// @brief Takes the effects asked for.
  ///
  /// @return The effects, oldest first.
  auto take_effects() -> std::vector<OfferEffect>;

  /// @brief Takes the console's connection request once the trade is done.
  ///
  /// @return The request; empty before the trade is done or once taken.
  [[nodiscard]] auto take_peer_request()
      -> std::optional<net::nat::holepunch::ConnRequest>;

  /// @brief Reads where the trade stands.
  ///
  /// @return The state.
  auto state() const noexcept -> OfferWaitState { return wait_.state(); }

 private:
  /// @brief The trade.
  OfferWait wait_{};
  /// @brief The attempt.
  std::uint64_t attempt_{};
  /// @brief The console's connection request.
  std::optional<net::nat::holepunch::ConnRequest> peer_request_{};
};

/// @brief Why sending an offer effect failed.
enum class OfferSendErrorKind : std::uint8_t {
  /// @brief The effect is for another attempt, or attempt 0.
  stale_attempt,
  /// @brief The request id is out of range.
  invalid_req_id,
  /// @brief The acknowledgement's error is above 65535.
  invalid_error,
  /// @brief PSN refused the message.
  session_failure,
};

/// @brief Why sending an offer effect failed: a refusal before sending, or
/// PSN's error.
class OfferSendError {
 public:
  /// @brief The effect is for another attempt, or attempt 0.
  /// @return The error.
  static constexpr auto stale_attempt() noexcept -> OfferSendError {
    return OfferSendError{OfferSendErrorKind::stale_attempt};
  }
  /// @brief The request id is out of range.
  /// @return The error.
  static constexpr auto invalid_req_id() noexcept -> OfferSendError {
    return OfferSendError{OfferSendErrorKind::invalid_req_id};
  }
  /// @brief The acknowledgement's error is above 65535.
  /// @return The error.
  static constexpr auto invalid_error() noexcept -> OfferSendError {
    return OfferSendError{OfferSendErrorKind::invalid_error};
  }
  /// @brief PSN refused the message.
  /// @param session PSN's error.
  /// @return The error.
  static constexpr auto refused(psn::remote_play::SessionError session) noexcept
      -> OfferSendError {
    return OfferSendError{session};
  }

  /// @brief What failed.
  /// @return The kind.
  constexpr auto kind() const noexcept -> OfferSendErrorKind {
    return session_ ? OfferSendErrorKind::session_failure : kind_;
  }
  /// @brief PSN's error, for OfferSendErrorKind::session_failure.
  /// @return The error, or nothing for a refusal before sending.
  constexpr auto session() const noexcept
      -> std::optional<psn::remote_play::SessionError> {
    return session_;
  }

 private:
  /// @brief Makes a refusal before sending.
  /// @param kind What failed.
  constexpr explicit OfferSendError(OfferSendErrorKind kind) noexcept
      : kind_(kind) {}
  /// @brief Makes PSN's refusal.
  /// @param session PSN's error.
  constexpr explicit OfferSendError(
      psn::remote_play::SessionError session) noexcept
      : kind_(OfferSendErrorKind::session_failure), session_(session) {}

  /// @brief The refusal before sending.
  OfferSendErrorKind kind_;
  /// @brief PSN's error.
  std::optional<psn::remote_play::SessionError> session_;
};

/// @brief What failed in the PSN offer trade.
enum class OfferPsnErrorKind : std::uint8_t {
  /// @brief The attempt was 0, or the trade could not begin.
  invalid_attempt,
  /// @brief Our request id was out of range.
  invalid_own_req_id,
  /// @brief The trade was stopped.
  cancelled,
  /// @brief The deadline passed.
  timed_out,
  /// @brief The push connection failed.
  push_failure,
  /// @brief A session message could not be decoded.
  invalid_message,
  /// @brief Sending a message failed.
  send_failure,
  /// @brief The console terminated the trade, or it ended without the
  /// console's request.
  terminated,
};

/// @brief Why the PSN offer trade failed, with the receive's or send's error
/// that caused it.
class OfferPsnError {
 public:
  /// @brief Makes an error with no receive or send behind it.
  /// @param kind What failed.
  constexpr explicit OfferPsnError(OfferPsnErrorKind kind) noexcept
      : kind_(kind) {}
  /// @brief Makes the error of a failed receive: cancelled, timed out or a
  /// push failure, as the receive says.
  /// @param push The receive's error.
  /// @return The error.
  static constexpr auto received(
      psn::remote_play::BoundedPushReceiveError push) noexcept
      -> OfferPsnError {
    using Push = psn::remote_play::BoundedPushReceiveErrorKind;
    const auto kind =
        push.kind == Push::cancelled   ? OfferPsnErrorKind::cancelled
        : push.kind == Push::timed_out ? OfferPsnErrorKind::timed_out
                                       : OfferPsnErrorKind::push_failure;
    return OfferPsnError{kind, Cause{push}};
  }
  /// @brief Makes the error of a failed send.
  /// @param kind OfferPsnErrorKind::cancelled or timed_out when the
  ///     send was stopped, otherwise send_failure.
  /// @param send The send's error.
  /// @return The error.
  static constexpr auto sent(OfferPsnErrorKind kind,
                             OfferSendError send) noexcept -> OfferPsnError {
    return OfferPsnError{kind, Cause{send}};
  }

  /// @brief What failed.
  /// @return The kind.
  constexpr auto kind() const noexcept -> OfferPsnErrorKind { return kind_; }
  /// @brief The push receive's error, when receiving failed.
  /// @return The error, or nothing.
  constexpr auto push() const noexcept
      -> std::optional<psn::remote_play::BoundedPushReceiveError> {
    if (const auto* push = std::get_if<psn::remote_play::BoundedPushReceiveError>(&cause_))
      return *push;
    return std::nullopt;
  }
  /// @brief The send's error, when sending failed.
  /// @return The error, or nothing.
  constexpr auto send() const noexcept -> std::optional<OfferSendError> {
    if (const auto* send = std::get_if<OfferSendError>(&cause_)) return *send;
    return std::nullopt;
  }

 private:
  /// @brief The receive's or send's error behind the failure, if any.
  using Cause =
      std::variant<std::monostate, psn::remote_play::BoundedPushReceiveError,
                   OfferSendError>;

  /// @brief Makes an error with its cause.
  /// @param kind What failed.
  /// @param cause What caused it.
  constexpr OfferPsnError(OfferPsnErrorKind kind, Cause cause) noexcept
      : kind_(kind), cause_(cause) {}

  /// @brief What failed.
  OfferPsnErrorKind kind_;
  /// @brief What caused it.
  Cause cause_;
};

/// @brief Drives an offer trade: receives messages, sends the effects they
/// ask for, until the console's request is in hand.
///
/// Blocks in the receive and send functions. Use on one thread.
class OfferDrive {
 public:
  /// @brief Receives the next hole-punch message; blocks.
  using Receive = std::function<
      Expected<net::nat::holepunch::SessionMessage, OfferPsnError>()>;
  /// @brief Sends one effect; blocks.
  using Send = std::function<Expected<void, OfferPsnError>(const OfferEffect&)>;

  /// @brief Builds the drive.
  ///
  /// @param receive Receives messages.
  /// @param send Sends effects.
  /// @param trace Records the trade's states; may be null.
  OfferDrive(Receive receive, Send send,
             const diagnostics::Trace* trace = nullptr);

  /// @brief Runs the trade; blocks until it finishes.
  ///
  /// @param attempt The attempt; not 0.
  /// @param own_req_id Our request id; 1 to 65535.
  /// @return The console's connection request.
  /// @retval OfferPsnErrorKind::invalid_attempt The attempt is 0 or
  ///     the trade could not begin.
  /// @retval OfferPsnErrorKind::invalid_own_req_id Our request id is
  ///     out of range.
  /// @retval OfferPsnErrorKind::terminated The console terminated the
  ///     trade, or it ended without the console's request.
  /// @retval OfferPsnError The receive's or send's error, unchanged.
  [[nodiscard]] auto run(std::uint64_t attempt, std::uint64_t own_req_id)
      -> Expected<net::nat::holepunch::ConnRequest, OfferPsnError>;

 private:
  /// @brief Records the trade's state.
  void trace_state() const noexcept;
  /// @brief The trade.
  OfferInbox inbox_{};
  /// @brief Receives messages.
  Receive receive_;
  /// @brief Sends effects.
  Send send_;
  /// @brief Records the states; may be null.
  const diagnostics::Trace* trace_;
};

/// @brief What our connection request is built from. Plain data.
struct OfferRequestInput {
  /// @brief Our session id.
  std::uint32_t local_sid{};
  /// @brief The console's session id; 0 when not known.
  std::uint32_t console_sid{};
  /// @brief Our hashed id.
  SecretArray<net::holepunch_hashed_id_size> local_hashed_id;
  /// @brief The candidates offered; at least two.
  std::vector<net::nat::holepunch::Candidate> candidates;
};

/// @brief Our offer and what gathering found. Plain data.
struct PreparedOffer {
  /// @brief The local candidate.
  net::nat::holepunch::Candidate local;
  /// @brief What STUN suggests the NAT allocates.
  net::nat::stun::StunAllocationEstimate allocation;
  /// @brief Our connection request.
  net::nat::holepunch::ConnRequest request;
};

/// @brief Why probing the console's candidates failed.
enum class OfferProbeError : std::uint8_t {
  /// @brief Our offer cannot be probed from.
  invalid_local_offer,
  /// @brief The console's offer cannot be probed.
  invalid_peer_offer,
  /// @brief The probe was stopped.
  cancelled,
  /// @brief No candidate to probe.
  no_candidates,
  /// @brief A probe reply carried the wrong session id.
  invalid_sid,
  /// @brief The random provider failed.
  random_failed,
  /// @brief No candidate answered in time.
  wait_timeout,
  /// @brief The network failed while waiting.
  wait_net,
  /// @brief A reply could not be read while waiting.
  wait_protocol,
  /// @brief No candidate could be selected.
  selection_failed,
};

/// @brief The OFFER half of PSN hole punching. Static functions only; the
/// PSN calls block.
class Offer {
 public:
  /// @brief Makes the effects of the console's first offer.
  ///
  /// @param attempt The attempt; not 0.
  /// @param peer_req_id The console's request id; at most 65535.
  /// @param own_req_id Our request id; not 0.
  /// @return An acknowledgement and our offer; the reason they cannot be made.
  [[nodiscard]] static auto make_initial_effects(std::uint64_t attempt,
                                                 std::uint64_t peer_req_id,
                                                 std::uint64_t own_req_id)
      -> Expected<std::array<OfferEffect, 2>, OfferEffectError>;

  /// @brief Sends one effect through PSN; blocks on PSN.
  ///
  /// @param effect The effect.
  /// @param current_attempt The attempt now running.
  /// @param client Sends PSN requests.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param local_offer Our connection request, sent with our offer.
  /// @param stop Stops the request.
  /// @return Nothing; the reason it was not sent.
  [[nodiscard]] static auto send_effect(
      const OfferEffect& effect, std::uint64_t current_attempt,
      psn::remote_play::SessionClient& client,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family,
      const net::nat::holepunch::ConnRequest& local_offer,
      const platform::StopToken& stop) -> Expected<void, OfferSendError>;

  /// @brief Builds our connection request, with NAT type 2.
  ///
  /// @param input What it is built from.
  /// @return The request; ConnRequestError::invalid_schema for fewer than
  ///     two candidates.
  [[nodiscard]] static auto build_conn_request(OfferRequestInput input)
      -> Expected<net::nat::holepunch::ConnRequest,
                  net::nat::holepunch::ConnRequestError>;

  /// @brief Builds our offer from gathered candidates.
  ///
  /// @param gathered The gathered candidates.
  /// @param local_sid Our session id.
  /// @param console_sid The console's session id; 0 when not known.
  /// @param local_hashed_id Our hashed id.
  /// @return The prepared offer; as Offer::build_conn_request.
  [[nodiscard]] static auto prepare(
      net::nat::holepunch::GatheredCandidates gathered, std::uint32_t local_sid,
      std::uint32_t console_sid,
      SecretArray<net::holepunch_hashed_id_size> local_hashed_id)
      -> Expected<PreparedOffer, net::nat::holepunch::ConnRequestError>;

  /// @brief Probes the console's candidates from our sockets until one
  /// answers; blocks on the network.
  ///
  /// @param local_offer Our connection request.
  /// @param peer_offer The console's connection request.
  /// @param sockets Our bound sockets; consumed.
  /// @param dns Resolves candidate hosts.
  /// @param poller Waits on the sockets.
  /// @param clock The monotonic clock.
  /// @param random Draws probe values.
  /// @param stop Stops the probe.
  /// @param retry How probes are retried.
  /// @return The selected candidate and its socket; the probe's failure.
  [[nodiscard]] static auto probe_peer(
      const net::nat::holepunch::ConnRequest& local_offer,
      const net::nat::holepunch::ConnRequest& peer_offer,
      net::nat::holepunch::BoundSockets sockets, platform::DnsProvider& dns,
      platform::Poller& poller, const platform::ClockProvider& clock,
      platform::SecureRandomProvider& random, const platform::StopToken& stop,
      net::nat::holepunch::ProbeRetryPolicy retry = {})
      -> Expected<net::nat::holepunch::SelectedCandidate, OfferProbeError>;

  /// @brief Trades offers with the console through PSN: waits for its offer on
  /// the push connection, acknowledges it, sends ours and waits for its
  /// RESULT, then checks the PSN session. Blocks up to the deadline.
  ///
  /// Only the session-message notifications count. The console's session id is
  /// cut to 16 bits. After our offer went out, later PSN send failures other
  /// than a stop or a 4xx refusal are ignored.
  ///
  /// @param attempt The attempt; not 0.
  /// @param own_req_id Our request id; 1 to 65535.
  /// @param session Sends PSN requests.
  /// @param push Receives PSN notifications.
  /// @param connection The push connection.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param local_offer Our connection request.
  /// @param parent_stop The caller's stop.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param trace Records the trade's states; may be null.
  /// @return The console's connection request; the failure.
  [[nodiscard]] static auto run_psn(
      std::uint64_t attempt, std::uint64_t own_req_id,
      psn::remote_play::SessionClient& session,
      psn::remote_play::PushClient& push,
      platform::WebSocketConnection& connection,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family,
      const net::nat::holepunch::ConnRequest& local_offer,
      platform::StopToken parent_stop, const platform::ClockProvider& clock,
      net::RemoteWaitDeadline deadline_microseconds,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<net::nat::holepunch::ConnRequest, OfferPsnError>;

  /// @brief Decodes a PSN notification into the inbox.
  ///
  /// @param inbox The inbox.
  /// @param attempt The attempt.
  /// @param notification The notification.
  /// @return Nothing; the decode error.
  [[nodiscard]] static auto feed_inbox(
      OfferInbox& inbox, std::uint64_t attempt,
      const psn::rest::Notification& notification)
      -> Expected<void, psn::remote_play::HolepunchMessage::Error>;

 private:
  /// @brief Names a failed send of the trade: cancelled when the caller
  /// stopped it, timed out when the trade's deadline passed, otherwise a send
  /// failure.
  ///
  /// @param error The send's error.
  /// @param parent The caller's stop token.
  /// @param send_stop The stop state the send ran under.
  /// @return The kind.
  static auto send_failure_kind(const OfferSendError& error,
                                const platform::StopToken& parent,
                                const net::DeadlineStopState& send_stop)
      -> OfferPsnErrorKind;
};

}  // namespace libchiaking::play::internet::holepunch
