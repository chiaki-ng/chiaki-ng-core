/// @file
/// @brief Declares the Senkusha test's lifecycle: the states, the events that
/// move it, the effects it asks for, and the Boost.SML machine that ties them.
///
/// The machine decides; Client::Impl acts. Every event carries the id
/// of the attempt it belongs to, and Lifecycle drops events for any other
/// attempt, so a late answer from an earlier attempt cannot move the current
/// one. Each transition queues effects (send the BIG, switch echo on, publish
/// the result) in LifecycleContext, which the client then runs in order.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

namespace libchiaking::net::protocol::senkusha::lifecycle_detail {
namespace sml = boost::sml;

/// @brief Constructed; start() not yet called.
struct IdleState {};
/// @brief The Takion handshake is running; the connect deadline is armed.
struct ConnectingTakionState {};
/// @brief Connected; waiting for the negotiated version to be committed.
struct NegotiatingVersionState {};
/// @brief The BIG has been queued; left at once for the matching await state.
struct SendingBigState {};
/// @brief Waiting for a console's BANG.
struct AwaitingBangState {};
/// @brief Waiting for a CloudStream datacenter to echo the BIG back.
struct AwaitingBigEchoState {};
/// @brief Waiting for the echo switch-on command to be acknowledged.
struct EnablingEchoState {};
/// @brief Round-trip probes are running.
struct MeasuringRttState {};
/// @brief Waiting for the echo switch-off command to be acknowledged.
struct DisablingEchoState {};
/// @brief The inbound MTU search is running.
struct MeasuringInboundMtuState {};
/// @brief Waiting for the far end to confirm the outbound MTU echo is on.
struct StartingOutboundMtuState {};
/// @brief The outbound MTU search is running.
struct MeasuringOutboundMtuState {};
/// @brief Waiting for the outbound MTU switch-off to be acknowledged.
struct FinishingOutboundMtuState {};
/// @brief Bandwidth rounds are running.
struct MeasuringBandwidthState {};
/// @brief Closing the session or keeping its route.
struct DisconnectingState {};
/// @brief The test succeeded; terminal.
struct CompletedState {};
/// @brief The test failed; terminal.
struct FailedState {};

/// @brief The caller started the test.
struct StartRequested {
  /// @brief The new attempt's id; not 0.
  std::uint64_t attempt_id{};
  /// @brief When start() was called, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief How long the handshake may take, in microseconds.
  std::uint64_t connect_timeout_us{};
  /// @brief The pinned version, or empty to wait for negotiation.
  std::optional<std::uint32_t> fixed_version{};
  /// @brief How the BIG will be answered.
  BigResponsePolicy big_response{BigResponsePolicy::typed_bang};
  /// @brief Whether the bandwidth step is to run.
  bool measure_bandwidth{};
};

/// @brief The Takion handshake completed.
struct TakionConnected {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief When it was observed, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief How long to wait for the next answer, in microseconds.
  std::uint64_t response_timeout_us{};
};

/// @brief The Takion negotiation committed a version.
struct ProfileCommitted {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief The committed Takion version.
  std::uint32_t version{};
  /// @brief When it was observed, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief How long to wait for the BIG's answer, in microseconds.
  std::uint64_t response_timeout_us{};
};

/// @brief The console's BANG passed validation.
struct BangAccepted {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief All the bytes of the BIG came back from a CloudStream datacenter.
struct BigEchoReceived {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The echo switch-on command was acknowledged.
struct EchoEnabled {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Every round-trip probe has been settled and at least one answered.
struct RttMeasured {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief What the probes found.
  RttStatistics statistics{};
};
/// @brief Every round-trip probe has been settled without a usable result.
struct RttUnmeasured {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why; reported once echo has been switched off again.
  Error error{Error::no_rtt_samples};
};
/// @brief The echo switch-off command was acknowledged.
struct EchoDisabled {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The inbound MTU search finished.
struct InboundMtuMeasured {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief The inbound MTU, in bytes.
  std::uint32_t mtu{};
};
/// @brief The far end confirmed (with a client MTU command of id 1) that it
/// will echo outbound probes.
struct OutboundMtuReady {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The outbound MTU search finished.
struct OutboundMtuMeasured {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief The outbound MTU, in bytes.
  std::uint32_t mtu{};
  /// @brief The search's final upper bound, in bytes, sent in the switch-off
  /// command.
  std::uint32_t upper_bound{};
};
/// @brief The outbound MTU switch-off command was acknowledged.
struct OutboundMtuDisabled {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The bandwidth step finished.
struct BandwidthMeasured {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief What it found; empty when nothing could be measured, which does not
  /// fail the test.
  std::optional<BandwidthMeasurement> measurement{};
};
/// @brief The session was closed or its route kept and the result recorded.
struct DisconnectCompleted {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief A measurement step failed.
struct MeasurementFailed {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why.
  Error error{Error::invalid_state};
};
/// @brief The BANG failed validation, or the BIG could not be sent.
struct BangRejected {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why.
  Error error{Error::malformed_response};
};
/// @brief The armed deadline passed with nothing outstanding to retry.
struct DeadlineExpired {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The caller cancelled.
struct CancelRequested {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The Takion session failed or closed.
struct TakionFailed {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};

/// @brief Effect: start the Takion session.
struct StartTakion {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: send the profile's BIG.
struct SendBig {
  /// @brief The committed Takion version.
  std::uint32_t version{};
};
/// @brief Effect: send the echo switch-on command.
struct BeginRtt {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: start sending round-trip probes.
struct BeginRttProbes {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: send the echo switch-off command.
struct DisableEcho {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: start the inbound MTU search.
struct BeginInboundMtu {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: send the outbound MTU switch-on command (id 1).
struct BeginOutboundMtu {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
  /// @brief The inbound MTU, sent as the request and the downstream MTU.
  std::uint32_t inbound_mtu{};
};
/// @brief Effect: start the outbound MTU search.
struct BeginOutboundMtuProbes {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
  /// @brief The inbound MTU, used as the first candidate.
  std::uint32_t inbound_mtu{};
};
/// @brief Effect: send the outbound MTU switch-off command (id 2).
struct DisableOutboundMtu {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
  /// @brief The inbound MTU, sent as the downstream MTU.
  std::uint32_t inbound_mtu{};
  /// @brief The outbound search's final upper bound, sent as the request.
  std::uint32_t upper_bound{};
};
/// @brief Effect: start the bandwidth rounds.
struct BeginBandwidth {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
  /// @brief The inbound MTU, sent with each round.
  std::uint32_t inbound_mtu{};
};
/// @brief Effect: build the result, send a best-effort DISCONNECT, and close
/// the session or keep its route.
struct BeginDisconnect {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: tell the sink the test completed.
struct PublishSuccess {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: send a DISCONNECT if the session is still connected.
struct BestEffortDisconnect {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: stop the Takion session.
struct StopTakion {
  /// @brief The attempt it is for.
  std::uint64_t attempt_id{};
};
/// @brief Effect: tell the sink the test failed.
struct PublishFailure {
  /// @brief Why.
  Error error{};
};

/// @brief Any effect the lifecycle can ask for.
using LifecycleEffect =
    std::variant<StartTakion, SendBig, BeginRtt, BeginRttProbes, DisableEcho,
                 BeginInboundMtu, BeginOutboundMtu, BeginOutboundMtuProbes,
                 DisableOutboundMtu, BeginBandwidth, BeginDisconnect,
                 PublishSuccess, BestEffortDisconnect, StopTakion,
                 PublishFailure>;

/// @brief The lifecycle's memory: the queued effects, the attempt, the armed
/// deadline and every measurement so far.
///
/// Owned by Client::Impl and shared by reference with Lifecycle and the
/// machine's actions; used from the test's thread only. Measurements are reset
/// when an attempt begins and filled in as each step finishes.
struct LifecycleContext {
  /// @brief Effects queued by transitions, oldest first, for the client to run.
  std::deque<LifecycleEffect> effects;
  /// @brief The running attempt's id; empty before start and once it ends.
  std::optional<std::uint64_t> active_attempt_id{};
  /// @brief How the BIG will be answered.
  BigResponsePolicy big_response{BigResponsePolicy::typed_bang};
  /// @brief When the current wait expires, in monotonic microseconds; empty
  /// while nothing is awaited.
  std::optional<std::uint64_t> deadline_us{};
  /// @brief The pinned version, or empty when negotiating.
  std::optional<std::uint32_t> fixed_version{};
  /// @brief The version the BIG was sent for, once known.
  std::optional<std::uint32_t> committed_version{};
  /// @brief Round-trip result, once measured.
  std::optional<RttStatistics> rtt_statistics{};
  /// @brief Inbound MTU in bytes, once measured.
  std::optional<std::uint32_t> inbound_mtu{};
  /// @brief Outbound MTU in bytes, once measured.
  std::optional<std::uint32_t> outbound_mtu{};
  /// @brief Whether the bandwidth step is to run.
  bool measure_bandwidth{};
  /// @brief Bandwidth result, once measured; may stay empty.
  std::optional<BandwidthMeasurement> bandwidth{};
  /// @brief A round-trip failure held back until echo is switched off.
  std::optional<Error> pending_rtt_error{};
  /// @brief Why the test failed; set once, on failure.
  std::optional<Error> terminal_error{};
  /// @brief Events dropped because they named another attempt.
  std::size_t stale_events{};
  /// @brief Events the machine had no transition for in its current state, or
  /// start requests that were refused.
  std::size_t unexpected_events{};

  /// @brief Tells whether an event belongs to the running attempt.
  ///
  /// @param attempt_id The event's attempt id.
  /// @return Whether an attempt is running and has that id.
  bool is_current(std::uint64_t attempt_id) const noexcept {
    return active_attempt_id && *active_attempt_id == attempt_id;
  }

  /// @brief Tells whether a deadline can be computed without overflow.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @param timeout_us The wait, in microseconds.
  /// @return Whether `now_us + timeout_us` fits 64 bits.
  static bool deadline_fits(std::uint64_t now_us,
                            std::uint64_t timeout_us) noexcept {
    return now_us <= std::numeric_limits<std::uint64_t>::max() - timeout_us;
  }

  /// @brief Arms the deadline at `now_us + timeout_us`; the caller has checked
  /// it fits.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @param timeout_us The wait, in microseconds.
  void arm_deadline(std::uint64_t now_us, std::uint64_t timeout_us) {
    deadline_us = now_us + timeout_us;
  }

  /// @brief Fails a started attempt: records the error, clears the deadline,
  /// and queues a best-effort DISCONNECT, a session stop and the failure
  /// notice, in that order.
  ///
  /// @param attempt_id The attempt that failed.
  /// @param error Why.
  void fail(std::uint64_t attempt_id, Error error) {
    terminal_error = error;
    deadline_us.reset();
    effects.emplace_back(BestEffortDisconnect{attempt_id});
    effects.emplace_back(StopTakion{attempt_id});
    effects.emplace_back(PublishFailure{error});
    active_attempt_id.reset();
  }

  /// @brief Fails an attempt whose session never started: records the error and
  /// queues only the failure notice.
  ///
  /// @param error Why.
  void fail_before_start(Error error) {
    terminal_error = error;
    deadline_us.reset();
    effects.emplace_back(PublishFailure{error});
    active_attempt_id.reset();
  }
};

/// @brief The guards and actions of the lifecycle's transition tables.
///
/// Each is a stateless lambda taking the triggering event and the context; the
/// guards return whether a transition applies, the actions update the context
/// and queue effects.
struct LifecycleRules {
  /// @brief Guard: the event's response deadline fits 64 bits.
  static constexpr auto deadline_fits = [](const auto& event) noexcept {
    return LifecycleContext::deadline_fits(event.now_us,
                                           event.response_timeout_us);
  };
  /// @brief Guard: the start request's connect deadline fits 64 bits.
  static constexpr auto connect_deadline_fits =
      [](const StartRequested& event) noexcept {
        return LifecycleContext::deadline_fits(event.now_us,
                                               event.connect_timeout_us);
      };
  /// @brief Guard: the profile pins a Takion version.
  static constexpr auto fixed = [](const LifecycleContext& context) noexcept {
    return context.fixed_version.has_value();
  };
  /// @brief Guard: the Takion version is to be negotiated.
  static constexpr auto negotiated =
      [](const LifecycleContext& context) noexcept {
        return !context.fixed_version.has_value();
      };
  /// @brief Action: begins an attempt, resetting every measurement, queueing
  /// StartTakion and arming the connect deadline.
  static constexpr auto begin = [](const StartRequested& event,
                                   LifecycleContext& context) {
    context.active_attempt_id = event.attempt_id;
    context.big_response = event.big_response;
    context.fixed_version = event.fixed_version;
    context.measure_bandwidth = event.measure_bandwidth;
    context.bandwidth.reset();
    context.committed_version.reset();
    context.rtt_statistics.reset();
    context.inbound_mtu.reset();
    context.outbound_mtu.reset();
    context.pending_rtt_error.reset();
    context.terminal_error.reset();
    context.deadline_us.reset();
    context.effects.emplace_back(StartTakion{event.attempt_id});
    context.arm_deadline(event.now_us, event.connect_timeout_us);
  };
  /// @brief Action: fails a start whose connect deadline would overflow.
  static constexpr auto fail_start_overflow = [](const StartRequested& event,
                                                 LifecycleContext& context) {
    context.active_attempt_id = event.attempt_id;
    context.fail_before_start(Error::arithmetic_overflow);
  };
  /// @brief Action: arms the response deadline while negotiation runs.
  static constexpr auto await_negotiation = [](const TakionConnected& event,
                                               LifecycleContext& context) {
    context.arm_deadline(event.now_us, event.response_timeout_us);
  };
  /// @brief Action: commits the pinned version, arms the response deadline and
  /// queues SendBig.
  static constexpr auto send_fixed_big = [](const TakionConnected& event,
                                            LifecycleContext& context) {
    context.arm_deadline(event.now_us, event.response_timeout_us);
    context.committed_version = context.fixed_version;
    context.effects.emplace_back(SendBig{*context.fixed_version});
  };
  /// @brief Action: commits the negotiated version, arms the response deadline
  /// and queues SendBig.
  static constexpr auto send_negotiated_big = [](const ProfileCommitted& event,
                                                 LifecycleContext& context) {
    context.arm_deadline(event.now_us, event.response_timeout_us);
    context.committed_version = event.version;
    context.effects.emplace_back(SendBig{event.version});
  };
  /// @brief Action: fails the attempt because a response deadline would
  /// overflow.
  static constexpr auto fail_deadline_overflow = [](const auto& event,
                                                    LifecycleContext& context) {
    context.fail(event.attempt_id, Error::arithmetic_overflow);
  };
  /// @brief Action: clears the deadline and queues BeginRtt after a valid BANG.
  static constexpr auto accept_bang = [](const BangAccepted& event,
                                         LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(BeginRtt{event.attempt_id});
  };
  /// @brief Action: clears the deadline and queues BeginRtt after the BIG's
  /// echo.
  static constexpr auto accept_big_echo = [](const BigEchoReceived& event,
                                             LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(BeginRtt{event.attempt_id});
  };
  /// @brief Action: gives up waiting for the BIG's echo and queues BeginRtt
  /// anyway; a missing echo is not a failure.
  static constexpr auto echo_timeout = [](const DeadlineExpired& event,
                                          LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(BeginRtt{event.attempt_id});
  };
  /// @brief Guard: a BANG answers the BIG.
  static constexpr auto typed_bang =
      [](const LifecycleContext& context) noexcept {
        return context.big_response == BigResponsePolicy::typed_bang;
      };
  /// @brief Guard: the BIG is echoed back.
  static constexpr auto fragmented_big_echo =
      [](const LifecycleContext& context) noexcept {
        return context.big_response == BigResponsePolicy::fragmented_big_echo;
      };
  /// @brief Action: queues BeginRttProbes.
  static constexpr auto begin_rtt_probes = [](const EchoEnabled& event,
                                              LifecycleContext& context) {
    context.effects.emplace_back(BeginRttProbes{event.attempt_id});
  };
  /// @brief Action: stores the round-trip result and queues DisableEcho.
  static constexpr auto disable_echo = [](const RttMeasured& event,
                                          LifecycleContext& context) {
    context.deadline_us.reset();
    context.rtt_statistics = event.statistics;
    context.effects.emplace_back(DisableEcho{event.attempt_id});
  };
  /// @brief Action: holds the round-trip error and queues DisableEcho, so echo
  /// is switched off before the test fails.
  static constexpr auto disable_echo_unmeasured =
      [](const RttUnmeasured& event, LifecycleContext& context) {
        context.deadline_us.reset();
        context.pending_rtt_error = event.error;
        context.effects.emplace_back(DisableEcho{event.attempt_id});
      };
  /// @brief Guard: a round-trip error is being held.
  static constexpr auto rtt_pending =
      [](const LifecycleContext& context) noexcept {
        return context.pending_rtt_error.has_value();
      };
  /// @brief Action: fails the attempt with the held round-trip error.
  static constexpr auto fail_pending_rtt = [](const EchoDisabled& event,
                                              LifecycleContext& context) {
    context.fail(event.attempt_id, *context.pending_rtt_error);
  };
  /// @brief Action: queues BeginInboundMtu.
  static constexpr auto begin_inbound_mtu = [](const EchoDisabled& event,
                                               LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(BeginInboundMtu{event.attempt_id});
  };
  /// @brief Action: stores the inbound MTU and queues BeginOutboundMtu.
  static constexpr auto begin_outbound_mtu = [](const InboundMtuMeasured& event,
                                                LifecycleContext& context) {
    context.deadline_us.reset();
    context.inbound_mtu = event.mtu;
    context.effects.emplace_back(BeginOutboundMtu{event.attempt_id, event.mtu});
  };
  /// @brief Action: queues BeginOutboundMtuProbes with the inbound MTU.
  static constexpr auto begin_outbound_mtu_probes =
      [](const OutboundMtuReady& event, LifecycleContext& context) {
        context.deadline_us.reset();
        context.effects.emplace_back(
            BeginOutboundMtuProbes{event.attempt_id, *context.inbound_mtu});
      };
  /// @brief Action: stores the outbound MTU and queues DisableOutboundMtu.
  static constexpr auto disable_outbound_mtu =
      [](const OutboundMtuMeasured& event, LifecycleContext& context) {
        context.deadline_us.reset();
        context.outbound_mtu = event.mtu;
        context.effects.emplace_back(DisableOutboundMtu{
            event.attempt_id, *context.inbound_mtu, event.upper_bound});
      };
  /// @brief Action: queues BeginDisconnect.
  static constexpr auto begin_disconnect = [](const OutboundMtuDisabled& event,
                                              LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(BeginDisconnect{event.attempt_id});
  };
  /// @brief Guard: the bandwidth step is wanted and the inbound MTU is known.
  static constexpr auto measures_bandwidth =
      [](const LifecycleContext& context) noexcept {
        return context.measure_bandwidth && context.inbound_mtu.has_value();
      };
  /// @brief Action: queues BeginBandwidth with the inbound MTU.
  static constexpr auto begin_bandwidth = [](const OutboundMtuDisabled& event,
                                             LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(
        BeginBandwidth{event.attempt_id, *context.inbound_mtu});
  };
  /// @brief Action: stores the bandwidth result, if any, and queues
  /// BeginDisconnect.
  static constexpr auto bandwidth_measured = [](const BandwidthMeasured& event,
                                                LifecycleContext& context) {
    context.deadline_us.reset();
    context.bandwidth = event.measurement;
    context.effects.emplace_back(BeginDisconnect{event.attempt_id});
  };
  /// @brief Action: queues PublishSuccess and ends the attempt.
  static constexpr auto complete = [](const DisconnectCompleted& event,
                                      LifecycleContext& context) {
    context.deadline_us.reset();
    context.effects.emplace_back(PublishSuccess{event.attempt_id});
    context.active_attempt_id.reset();
  };
  /// @brief Action: fails the attempt with the BANG's error.
  static constexpr auto reject_bang = [](const BangRejected& event,
                                         LifecycleContext& context) {
    context.fail(event.attempt_id, event.error);
  };
  /// @brief Action: fails the attempt with the measurement's error.
  static constexpr auto measurement_failed = [](const MeasurementFailed& event,
                                                LifecycleContext& context) {
    context.fail(event.attempt_id, event.error);
  };
  /// @brief Action: fails the attempt with timeout.
  static constexpr auto timeout = [](const DeadlineExpired& event,
                                     LifecycleContext& context) {
    context.fail(event.attempt_id, Error::timeout);
  };
  /// @brief Action: fails the attempt with cancelled.
  static constexpr auto cancel = [](const CancelRequested& event,
                                    LifecycleContext& context) {
    context.fail(event.attempt_id, Error::cancelled);
  };
  /// @brief Action: fails the attempt with takion_failure.
  static constexpr auto takion_failed = [](const TakionFailed& event,
                                           LifecycleContext& context) {
    context.fail(event.attempt_id, Error::takion_failure);
  };

  /// @brief Guard: the attempt has failed.
  static constexpr auto failed = [](const LifecycleContext& context) noexcept {
    return context.terminal_error.has_value();
  };
};

/// @brief The establishing sub-machine: Takion handshake, version, BIG and
/// its answer.
///
/// Connecting moves to SendingBig (fixed version) or NegotiatingVersion
/// (negotiated) on TakionConnected; NegotiatingVersion moves to SendingBig on
/// ProfileCommitted; SendingBig passes straight on to AwaitingBang or
/// AwaitingBigEcho. A valid BANG, a rejected BANG, the BIG's echo, or the end
/// of the echo wait finishes the sub-machine.
struct EstablishingFsm {
  /// @brief Builds the transition table.
  ///
  /// @return The Boost.SML transition table.
  auto operator()() const noexcept {
    using namespace sml;
    using R = LifecycleRules;

    return make_transition_table(
        *state<ConnectingTakionState> +
            sml::event<TakionConnected>[R::deadline_fits && R::fixed] /
                R::send_fixed_big = state<SendingBigState>,
        state<ConnectingTakionState> +
            sml::event<TakionConnected>[R::deadline_fits && R::negotiated] /
                R::await_negotiation = state<NegotiatingVersionState>,
        state<ConnectingTakionState> +
            sml::event<TakionConnected>[!R::deadline_fits] /
                R::fail_deadline_overflow = X,
        state<NegotiatingVersionState> +
            sml::event<ProfileCommitted>[R::deadline_fits] /
                R::send_negotiated_big = state<SendingBigState>,
        state<NegotiatingVersionState> +
            sml::event<ProfileCommitted>[!R::deadline_fits] /
                R::fail_deadline_overflow = X,
        state<SendingBigState>[R::typed_bang] = state<AwaitingBangState>,
        state<SendingBigState>[R::fragmented_big_echo] =
            state<AwaitingBigEchoState>,
        state<AwaitingBangState> + sml::event<BangAccepted> / R::accept_bang =
            X,
        state<AwaitingBangState> + sml::event<BangRejected> / R::reject_bang =
            X,
        state<AwaitingBigEchoState> +
            sml::event<BigEchoReceived> / R::accept_big_echo = X,
        state<AwaitingBigEchoState> +
            sml::event<DeadlineExpired> / R::echo_timeout = X);
  }
};

/// @brief The measuring sub-machine: echo on, round-trip probes, echo off,
/// inbound MTU, outbound MTU on, outbound probes, outbound off, optional
/// bandwidth, disconnect.
struct MeasuringFsm {
  /// @brief Builds the transition table.
  ///
  /// @return The Boost.SML transition table.
  auto operator()() const noexcept {
    using namespace sml;
    using R = LifecycleRules;

    return make_transition_table(
        *state<EnablingEchoState> +
            sml::event<EchoEnabled> / R::begin_rtt_probes =
            state<MeasuringRttState>,
        state<MeasuringRttState> + sml::event<RttMeasured> / R::disable_echo =
            state<DisablingEchoState>,
        state<MeasuringRttState> +
            sml::event<RttUnmeasured> / R::disable_echo_unmeasured =
            state<DisablingEchoState>,
        state<DisablingEchoState> +
            sml::event<EchoDisabled>[!R::rtt_pending] / R::begin_inbound_mtu =
            state<MeasuringInboundMtuState>,
        state<DisablingEchoState> +
            sml::event<EchoDisabled>[R::rtt_pending] / R::fail_pending_rtt = X,
        state<MeasuringInboundMtuState> +
            sml::event<InboundMtuMeasured> / R::begin_outbound_mtu =
            state<StartingOutboundMtuState>,
        state<StartingOutboundMtuState> +
            sml::event<OutboundMtuReady> / R::begin_outbound_mtu_probes =
            state<MeasuringOutboundMtuState>,
        state<MeasuringOutboundMtuState> +
            sml::event<OutboundMtuMeasured> / R::disable_outbound_mtu =
            state<FinishingOutboundMtuState>,
        state<FinishingOutboundMtuState> +
            sml::event<OutboundMtuDisabled>[R::measures_bandwidth] /
                R::begin_bandwidth = state<MeasuringBandwidthState>,
        state<FinishingOutboundMtuState> +
            sml::event<OutboundMtuDisabled>[!R::measures_bandwidth] /
                R::begin_disconnect = state<DisconnectingState>,
        state<MeasuringBandwidthState> +
            sml::event<BandwidthMeasured> / R::bandwidth_measured =
            state<DisconnectingState>,
        state<DisconnectingState> +
            sml::event<DisconnectCompleted> / R::complete = X);
  }
};

/// @brief The top-level machine: Idle, then Establishing, then Measuring, then
/// Completed; Failed from either sub-machine on cancel, Takion failure, an
/// expired deadline, a failed measurement, or a failure recorded inside it.
struct LifecycleFsm {
  /// @brief Builds the transition table.
  ///
  /// @return The Boost.SML transition table.
  auto operator()() const noexcept {
    using namespace sml;
    using R = LifecycleRules;
    const auto establishing = state<EstablishingFsm>;
    const auto measuring = state<MeasuringFsm>;

    return make_transition_table(
        *state<IdleState> +
            sml::event<StartRequested>[R::connect_deadline_fits] / R::begin =
            establishing,
        state<IdleState> +
            sml::event<StartRequested>[!R::connect_deadline_fits] /
                R::fail_start_overflow = state<FailedState>,

        establishing[R::failed] = state<FailedState>, establishing = measuring,
        establishing + sml::event<CancelRequested> / R::cancel =
            state<FailedState>,
        establishing + sml::event<TakionFailed> / R::takion_failed =
            state<FailedState>,
        establishing + sml::event<DeadlineExpired> / R::timeout =
            state<FailedState>,

        measuring[R::failed] = state<FailedState>,
        measuring = state<CompletedState>,
        measuring + sml::event<CancelRequested> / R::cancel =
            state<FailedState>,
        measuring + sml::event<TakionFailed> / R::takion_failed =
            state<FailedState>,
        measuring + sml::event<DeadlineExpired> / R::timeout =
            state<FailedState>,
        measuring + sml::event<MeasurementFailed> / R::measurement_failed =
            state<FailedState>);
  }
};

/// @brief The Senkusha lifecycle machine with its attempt filter.
///
/// Holds a reference to the context it was built with, which must outlive it.
/// Not thread-safe; used from the test's thread only.
class Lifecycle {
 public:
  /// @brief Builds an idle machine over a context.
  ///
  /// @param context The context the machine's actions update.
  explicit Lifecycle(LifecycleContext& context)
      : context_(context), machine_(context) {}

  /// @brief Offers a start request.
  ///
  /// @param event The request.
  /// @return Whether the machine took it; false, counted as unexpected, when
  /// the
  ///     id is 0 or an attempt is already running.
  [[nodiscard]] bool process(const StartRequested& event) {
    if (event.attempt_id == 0 || context_.active_attempt_id) {
      ++context_.unexpected_events;
      return false;
    }
    return process_current(event);
  }

  /// @brief Offers an event of the running attempt.
  ///
  /// @tparam Input One of the lifecycle event types.
  /// @param event The event.
  /// @return Whether the machine took it; false, counted as stale, when it
  /// names
  ///     another attempt, or counted as unexpected when no transition applies.
  template <class Input>
  [[nodiscard]] bool process(const Input& event) {
    if (!context_.is_current(event.attempt_id)) {
      ++context_.stale_events;
      return false;
    }
    return process_current(event);
  }

  /// @brief Tells whether a start request was ever taken.
  ///
  /// @return Whether the machine has left Idle.
  bool started() const noexcept { return !machine_.is(sml::state<IdleState>); }

  /// @brief Reads the current step as the public state.
  ///
  /// @return The step, looking inside the sub-machines.
  State state() const noexcept {
    if (machine_.is(sml::state<IdleState>)) return State::idle;
    if (machine_.is(sml::state<CompletedState>)) return State::completed;
    if (machine_.is(sml::state<EstablishingFsm>)) return establishing_state();
    if (machine_.is(sml::state<MeasuringFsm>)) return measuring_state();
    return State::failed;
  }

 private:
  /// @brief Feeds an event to the machine, counting it if refused.
  ///
  /// @tparam Input One of the lifecycle event types.
  /// @param event The event.
  /// @return Whether a transition took it.
  template <class Input>
  [[nodiscard]] bool process_current(const Input& event) {
    if (!machine_.process_event(event)) {
      ++context_.unexpected_events;
      return false;
    }
    return true;
  }

  /// @brief Maps the establishing sub-machine's state to the public state.
  ///
  /// @return The step; failed when none matches.
  State establishing_state() const noexcept {
    const auto in = [this](auto inner) {
      return machine_.is<decltype(sml::state<EstablishingFsm>)>(inner);
    };
    if (in(sml::state<ConnectingTakionState>)) return State::connecting_takion;
    if (in(sml::state<NegotiatingVersionState>))
      return State::negotiating_version;
    if (in(sml::state<SendingBigState>)) return State::sending_big;
    if (in(sml::state<AwaitingBangState>)) return State::awaiting_bang;
    if (in(sml::state<AwaitingBigEchoState>)) return State::awaiting_big_echo;
    return State::failed;
  }

  /// @brief Maps the measuring sub-machine's state to the public state.
  ///
  /// @return The step; failed when none matches.
  State measuring_state() const noexcept {
    const auto in = [this](auto inner) {
      return machine_.is<decltype(sml::state<MeasuringFsm>)>(inner);
    };
    if (in(sml::state<EnablingEchoState>)) return State::enabling_echo;
    if (in(sml::state<MeasuringRttState>)) return State::measuring_rtt;
    if (in(sml::state<DisablingEchoState>)) return State::disabling_echo;
    if (in(sml::state<MeasuringInboundMtuState>))
      return State::measuring_inbound_mtu;
    if (in(sml::state<StartingOutboundMtuState>))
      return State::starting_outbound_mtu;
    if (in(sml::state<MeasuringOutboundMtuState>))
      return State::measuring_outbound_mtu;
    if (in(sml::state<FinishingOutboundMtuState>))
      return State::finishing_outbound_mtu;
    if (in(sml::state<MeasuringBandwidthState>))
      return State::measuring_bandwidth;
    if (in(sml::state<DisconnectingState>)) return State::disconnecting;
    return State::failed;
  }

  /// @brief The context the actions update.
  LifecycleContext& context_;
  /// @brief The Boost.SML machine.
  sml::sm<LifecycleFsm> machine_;
};

}  // namespace libchiaking::net::protocol::senkusha::lifecycle_detail
