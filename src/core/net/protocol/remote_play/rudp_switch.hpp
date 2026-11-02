/// @file
/// @brief The switch of a PSN session's stream onto its own data route.
///
/// Over the internet the control channel and the stream start on separate
/// RUDP routes. Once the stream route is calibrated, the client sends an RUDP
/// switch request on the control route and waits up to rudp_switch_timeout
/// (5 s) for the console's control message 0x34, servicing the control
/// channel meanwhile. Only then does the stream take over its route.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/ctrl_lifecycle.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::net::protocol::remote_play::transport_switch_detail {
namespace sml = boost::sml;

/// @brief The phase of a switch.
enum class State : std::uint8_t { idle, waiting, ready, cancelled, timed_out };
/// @var libchiaking::net::protocol::remote_play::transport_switch_detail::State::idle
/// @brief No switch running, or the last confirmed one was handed over.
/// @var libchiaking::net::protocol::remote_play::transport_switch_detail::State::waiting
/// @brief The request was sent; waiting for the confirmation.
/// @var libchiaking::net::protocol::remote_play::transport_switch_detail::State::ready
/// @brief Confirmed; the route may be handed over.
/// @var libchiaking::net::protocol::remote_play::transport_switch_detail::State::cancelled
/// @brief Abandoned before confirmation.
/// @var libchiaking::net::protocol::remote_play::transport_switch_detail::State::timed_out
/// @brief No confirmation arrived in time.

/// @brief State tag: idle.
struct Idle {};
/// @brief State tag: waiting for the confirmation.
struct AwaitingConfirmation {};
/// @brief State tag: confirmed.
struct Confirmed {};
/// @brief State tag: cancelled.
struct Cancelled {};
/// @brief State tag: timed out.
struct TimedOut {};

/// @brief Event: a switch attempt starts.
struct Begin {
  /// @brief The attempt; must be nonzero and newer than any adopted before.
  std::uint64_t attempt_id{};
};
/// @brief Event: the console's confirmation arrived.
struct ConfirmationArrived {
  /// @brief The attempt it confirms.
  std::uint64_t attempt_id{};
};
/// @brief Event: the confirmation is overdue.
struct ConfirmationOverdue {
  /// @brief The attempt that timed out.
  std::uint64_t attempt_id{};
};
/// @brief Event: the switch is abandoned.
struct Abandon {
  /// @brief The attempt abandoned.
  std::uint64_t attempt_id{};
};
/// @brief Event: the confirmed route is handed over.
struct HandOver {};

/// @brief Counts of events the barrier refused.
struct Stats {
  /// @brief Confirmations repeated after the first.
  std::size_t duplicate_acks{};
  /// @brief Confirmations for another attempt or phase.
  std::size_t stale_acks{};
  /// @brief Timeouts for another attempt or phase.
  std::size_t stale_timeouts{};
  /// @brief Cancellations for another attempt or phase.
  std::size_t stale_cancels{};
  /// @brief Other refused events.
  std::size_t unexpected{};
};

/// @brief The barrier's data.
struct Context {
  /// @brief The attempt being switched.
  std::optional<std::uint64_t> attempt;
  /// @brief The newest attempt ever adopted; older ones are refused.
  std::uint64_t highest_accepted_id{};
  /// @brief Whether the last event was accepted.
  bool accepted{};
  /// @brief Refusal counts.
  Stats stats;
};

/// @brief The transition table.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The boost::sml table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto fresh = [](const Begin& e, const Context& c) {
      return e.attempt_id != 0 && e.attempt_id > c.highest_accepted_id;
    };
    const auto current = [](const auto& e, const Context& c) {
      return c.attempt == e.attempt_id;
    };
    const auto adopt = [](const Begin& e, Context& c) {
      c.attempt = e.attempt_id;
      c.highest_accepted_id = e.attempt_id;
      c.accepted = true;
    };
    const auto accept = [](Context& c) { c.accepted = true; };
    const auto release = [](Context& c) {
      c.attempt.reset();
      c.accepted = true;
    };
    const auto count_duplicate = [](Context& c) { ++c.stats.duplicate_acks; };

    return make_transition_table(
        *state<Idle> + sml::event<Begin>[fresh] / adopt =
            state<AwaitingConfirmation>,
        state<Cancelled> + sml::event<Begin>[fresh] / adopt =
            state<AwaitingConfirmation>,
        state<TimedOut> + sml::event<Begin>[fresh] / adopt =
            state<AwaitingConfirmation>,
        state<AwaitingConfirmation> + sml::event<ConfirmationArrived>[current] /
                                          accept = state<Confirmed>,
        state<AwaitingConfirmation> +
            sml::event<ConfirmationOverdue>[current] / accept = state<TimedOut>,
        state<AwaitingConfirmation> + sml::event<Abandon>[current] / accept =
            state<Cancelled>,
        state<Confirmed> +
            sml::event<ConfirmationArrived>[current] / count_duplicate,
        state<Confirmed> + sml::event<HandOver> / release = state<Idle>);
  }
};

/// @brief The barrier that holds the stream route until the console confirms
/// the switch.
///
/// Every call returns whether the event was accepted; refusals are counted in
/// stats(). Not thread-safe.
class RemoteTransportSwitch {
 public:
  /// @brief Makes an idle barrier.
  RemoteTransportSwitch() : machine_(context_) {}
  RemoteTransportSwitch(const RemoteTransportSwitch&) = delete;
  RemoteTransportSwitch& operator=(const RemoteTransportSwitch&) = delete;

  /// @brief Starts waiting for an attempt's confirmation.
  ///
  /// @param attempt_id A nonzero attempt newer than any before.
  /// @return True when accepted.
  [[nodiscard]] bool begin(std::uint64_t attempt_id) noexcept {
    return accepts(Begin{attempt_id}, context_.stats.unexpected);
  }
  /// @brief Records the confirmation.
  ///
  /// @param attempt_id The attempt confirmed.
  /// @return True when it confirms the waiting attempt (a repeat after
  ///     confirmation is counted and returns false).
  [[nodiscard]] bool on_ack(std::uint64_t attempt_id) noexcept {
    return accepts(ConfirmationArrived{attempt_id}, context_.stats.stale_acks);
  }
  /// @brief Records that the confirmation is overdue.
  ///
  /// @param attempt_id The attempt.
  /// @return True when it was the waiting attempt.
  [[nodiscard]] bool timeout(std::uint64_t attempt_id) noexcept {
    return accepts(ConfirmationOverdue{attempt_id},
                   context_.stats.stale_timeouts);
  }
  /// @brief Abandons the waiting attempt.
  ///
  /// @param attempt_id The attempt.
  /// @return True when it was the waiting attempt.
  [[nodiscard]] bool cancel(std::uint64_t attempt_id) noexcept {
    return accepts(Abandon{attempt_id}, context_.stats.stale_cancels);
  }
  /// @brief Hands over a confirmed switch, returning to idle.
  ///
  /// @return True when a switch was confirmed.
  [[nodiscard]] bool take_ready() noexcept {
    return accepts(HandOver{}, context_.stats.unexpected);
  }

  /// @brief Whether an attempt is waiting for its confirmation.
  ///
  /// @param attempt_id The attempt.
  /// @return True when waiting.
  auto awaits(std::uint64_t attempt_id) const noexcept -> bool {
    return machine_.is(sml::state<AwaitingConfirmation>) &&
           context_.attempt == attempt_id;
  }

  /// @brief The phase.
  ///
  /// @return The state.
  State state() const noexcept {
    if (machine_.is(sml::state<AwaitingConfirmation>)) return State::waiting;
    if (machine_.is(sml::state<Confirmed>)) return State::ready;
    if (machine_.is(sml::state<Cancelled>)) return State::cancelled;
    if (machine_.is(sml::state<TimedOut>)) return State::timed_out;
    return State::idle;
  }
  /// @brief The attempt being switched.
  ///
  /// @return The attempt, or nothing.
  std::optional<std::uint64_t> current_attempt() const noexcept {
    return context_.attempt;
  }
  /// @brief The refusal counts.
  ///
  /// @return The counts.
  const Stats& stats() const noexcept { return context_.stats; }

 private:
  /// @brief Feeds an event and reports acceptance.
  ///
  /// @tparam Event The event type.
  /// @param event The event.
  /// @param refusals The count to bump when the machine refuses it.
  /// @return Whether the event was accepted.
  template <class Event>
  bool accepts(const Event& event, std::size_t& refusals) noexcept {
    context_.accepted = false;
    if (!machine_.process_event(event)) ++refusals;
    return context_.accepted;
  }

  /// @brief The barrier's data.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace libchiaking::net::protocol::remote_play::transport_switch_detail

namespace libchiaking::net::protocol::remote_play {

/// @brief Holds the PSN stream's data route while the switch is confirmed,
/// closing it if the switch fails.
///
/// Owns the route until take_route(); destruction closes a route still held.
/// Not thread-safe.
class RemoteDataRouteGate {
 public:
  /// @brief Closes a route still held.
  ~RemoteDataRouteGate();

  /// @brief Takes the stream route and starts the barrier for an attempt.
  ///
  /// @param route A remote RUDP route.
  /// @param attempt_id The attempt.
  /// @return False when the route is not a remote RUDP route, a route is
  ///     already held, or the barrier refuses.
  [[nodiscard]] bool arm(EstablishedRoute route, std::uint64_t attempt_id);

  /// @brief Records the console's confirmation.
  ///
  /// @param ack The lifecycle's switch confirmation.
  /// @return True when it confirms the waiting attempt.
  [[nodiscard]] bool on_ack(ctrl_lifecycle_detail::PublishSwitchAck ack);

  /// @brief Records a timeout and closes the route.
  ///
  /// @param attempt_id The attempt.
  /// @return True when it was the waiting attempt.
  [[nodiscard]] bool timeout(std::uint64_t attempt_id);

  /// @brief Abandons the switch and closes the route.
  ///
  /// @param attempt_id The attempt.
  /// @return True when it was the waiting attempt.
  [[nodiscard]] bool cancel(std::uint64_t attempt_id);

  /// @brief Hands over the route once confirmed.
  ///
  /// @return The route, or nothing when none is held or it is not confirmed.
  [[nodiscard]] std::optional<EstablishedRoute> take_route();

  /// @brief The barrier's phase.
  ///
  /// @return The state.
  transport_switch_detail::State state() const noexcept {
    return barrier_.state();
  }
  /// @brief The attempt being switched.
  ///
  /// @return The attempt, or nothing.
  std::optional<std::uint64_t> current_attempt() const noexcept {
    return barrier_.current_attempt();
  }
  /// @brief Whether a route is held.
  ///
  /// @return True when held.
  bool has_route() const noexcept { return route_.has_value(); }
  /// @brief The error of closing the route, if that failed.
  ///
  /// @return The error, or nothing.
  [[nodiscard]] std::optional<net::transport::datagram::TransportError>
  close_error() const noexcept {
    return close_error_;
  }
  /// @brief The barrier, for the runner.
  ///
  /// @return The barrier.
  auto barrier() noexcept -> transport_switch_detail::RemoteTransportSwitch& {
    return barrier_;
  }

 private:
  /// @brief Closes and forgets the route, keeping any close error.
  void close_owned_route();

  /// @brief The barrier.
  transport_switch_detail::RemoteTransportSwitch barrier_{};
  /// @brief The held route.
  std::optional<EstablishedRoute> route_{};
  /// @brief The error of the last close.
  std::optional<net::transport::datagram::TransportError> close_error_{};
};

/// @brief Why the switch failed.
enum class RudpSwitchBarrierError : std::uint8_t {
  /// @brief The attempt is not the waiting one, a delivery named another
  /// attempt, or the session refused a message.
  invalid_attempt,
  /// @brief The stop token fired.
  stopped,
  /// @brief The switch request packet could not be made.
  packet_failed,
  /// @brief Sending the request failed.
  send_failed,
  /// @brief The channel took fewer bytes than the request.
  short_send,
  /// @brief Receiving failed or the console closed the route.
  receive_failed,
  /// @brief The cipher failed.
  crypto_failed,
  /// @brief The inbound cipher counter reached its maximum.
  counter_exhausted,
  /// @brief An acknowledgement could not be encoded.
  ack_failed,
  /// @brief No confirmation within rudp_switch_timeout.
  timeout,
  /// @brief Servicing the control channel during the wait failed.
  service_failed,
  /// @brief A packet could not be decoded.
  malformed,
};

/// @brief A confirmed switch and the control counters after it.
struct RudpSwitchBarrierResult {
  /// @brief The attempt.
  std::uint64_t attempt_id{};
  /// @brief The next inbound cipher counter.
  std::uint64_t next_remote_crypt_counter{};
  /// @brief The next local RUDP counter.
  std::uint16_t next_local_counter{};
};

/// @brief The platform services the switch uses. References must outlive
/// the call.
struct RudpSwitchServices {
  /// @brief Random bytes for the switch request.
  platform::SecureRandomProvider& random;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock;
  /// @brief Cancels the wait.
  const platform::StopToken& stop;
  /// @brief The control route's buffer of unacknowledged packets, resent by
  /// the reliable channel.
  rudp::SendBuffer& pending;
  /// @brief Where errors are logged, if anywhere.
  platform::DiagnosticsProvider* diagnostics{};
};

/// @brief How the control session takes part in the switch.
struct RudpSwitchHooks {
  /// @brief Receives each control message that arrives during the wait;
  /// returning false fails the switch.
  std::function<bool(RudpCtrlSwitchStepResult&&)> accept_message{};
  /// @brief Services the control channel (heartbeats, passcode) after each
  /// step, given the next local RUDP counter; returns the new counter, or
  /// nothing to fail the switch.
  std::function<std::optional<std::uint16_t>(rudp::HandshakeChannel&,
                                             std::uint16_t)>
      service_effects{};
};

/// @brief Sends the switch request and waits for the console's
/// confirmation.
///
/// Stateless; every member is static. Blocks the calling thread for up to
/// rudp_switch_timeout.
class RudpSwitchRequestRunner {
 public:
  /// @brief Starts a barrier attempt, then sends and waits.
  ///
  /// @param channel The control route's channel.
  /// @param state The barrier.
  /// @param input The control session's state.
  /// @param services The platform services.
  /// @param hooks The control session's part.
  /// @return The confirmed switch.
  /// @retval RudpSwitchBarrierError::stopped The stop token fired.
  /// @retval RudpSwitchBarrierError::invalid_attempt The barrier refused.
  /// @retval RudpSwitchBarrierError Any failure of the wait; the barrier is
  ///     cancelled or timed out.
  [[nodiscard]] static auto run(
      rudp::HandshakeChannel& channel,
      transport_switch_detail::RemoteTransportSwitch& state,
      RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
      const RudpSwitchHooks& hooks = {})
      -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError>;

  /// @brief Sends and waits on a barrier already armed for the attempt.
  ///
  /// @param channel The control route's channel.
  /// @param state The barrier, waiting for `input.attempt_id`.
  /// @param input The control session's state.
  /// @param services The platform services.
  /// @param hooks The control session's part.
  /// @return As run().
  [[nodiscard]] static auto run_armed(
      rudp::HandshakeChannel& channel,
      transport_switch_detail::RemoteTransportSwitch& state,
      RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
      const RudpSwitchHooks& hooks = {})
      -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError>;

 private:
  /// @brief Sends the switch request reliably and reads control packets until
  /// the confirmation, a failure or the deadline.
  ///
  /// @param channel The control route's channel.
  /// @param state The barrier.
  /// @param input The control session's state.
  /// @param services The platform services.
  /// @param hooks The control session's part.
  /// @return As run().
  [[nodiscard]] static auto await_confirmation(
      rudp::HandshakeChannel& channel,
      transport_switch_detail::RemoteTransportSwitch& state,
      RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
      const RudpSwitchHooks& hooks)
      -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError>;
  /// @brief Maps a receive failure to a switch failure.
  ///
  /// @param error The receive failure.
  /// @return The switch failure, or nothing for a timeout, which keeps
  ///     waiting.
  static auto barrier_error(RudpCtrlSwitchStepError error) noexcept
      -> std::optional<RudpSwitchBarrierError>;
  /// @brief Logs an error.
  ///
  /// @param diagnostics Where to log, or null.
  /// @param message The message.
  static void report(platform::DiagnosticsProvider* diagnostics,
                     std::string_view message);
};

/// @brief A confirmed switch with the route it released.
struct RudpDataSwitchResult {
  /// @brief The confirmed switch.
  RudpSwitchBarrierResult barrier;
  /// @brief The stream's data route, now free to use.
  EstablishedRoute route;
};

/// @brief Switches a PSN stream onto its calibrated data route.
class RudpDataSwitch {
 public:
  /// @brief Holds the route, sends the switch and hands the route over once
  /// confirmed.
  ///
  /// Blocks the calling thread for up to rudp_switch_timeout. The route is
  /// closed when the switch fails.
  ///
  /// @param data_route The calibrated stream route.
  /// @param ctrl_channel The control route's channel.
  /// @param input The control session's state.
  /// @param services The platform services.
  /// @param hooks The control session's part.
  /// @return The switch and the route.
  /// @retval RudpSwitchBarrierError::invalid_attempt The route or attempt was
  ///     refused.
  /// @retval RudpSwitchBarrierError Any failure of the wait.
  [[nodiscard]] static auto switch_route(EstablishedRoute data_route,
                                         rudp::HandshakeChannel& ctrl_channel,
                                         RudpCtrlSwitchStepInput input,
                                         const RudpSwitchServices& services,
                                         const RudpSwitchHooks& hooks = {})
      -> Expected<RudpDataSwitchResult, RudpSwitchBarrierError>;
};

}  // namespace libchiaking::net::protocol::remote_play
