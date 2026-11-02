/// @file
/// @brief Declares the Takion client's lifecycle: states, events, effects and
/// the Boost.SML machine that decides how the client moves from idle to
/// connected to closed or failed.
///
/// As in the Senkusha lifecycle, the machine only decides: each transition
/// queues effects (generate the tag, begin the handshake, close the socket,
/// publish a failure) that Client runs, and running them may feed it
/// further events.
#pragma once

#include "src/core/net/protocol/takion/client_types.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion::client_detail {
namespace sml = boost::sml;

/// @brief Constructed; not started.
struct IdleState {};
/// @brief The handshake is running.
struct HandshakingState {};
/// @brief The association is up.
struct ConnectedState {};
/// @brief A DISCONNECT was sent; the association stays up, answering
/// acknowledgements and resending, until it is acknowledged or the drain
/// deadline passes.
struct DisconnectingState {};
/// @brief The socket is being closed.
struct StoppingState {};
/// @brief Stopped.
struct ClosedState {};
/// @brief Failed.
struct FailedState {};

/// @brief Event: start() was called.
struct StartRequested {};
/// @brief Event: the verification tag was picked.
struct StartMaterialReady {
  /// @brief The tag; never 0.
  VerificationTag local_tag{};
  /// @brief The initial sequence number; the same value as the tag.
  InitialSequenceNumber local_initial_sequence_number{};
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};
/// @brief Event: the random source failed; the client returns to idle.
struct StartMaterialFailed {
  /// @brief Why.
  ClientError error{ClientError::random_failure};
};
/// @brief Event: the handshake completed.
struct HandshakeConnected {
  /// @brief What was agreed.
  EstablishedHandshake handshake{};
};
/// @brief Event: a stop was asked for.
struct StopRequested {
  /// @brief Whether to report Closed when the socket is closed.
  bool emit_closed{true};
};
/// @brief Event: something failed for good.
struct TerminalFailure {
  /// @brief Why.
  ClientError error{ClientError::invalid_state};
};
/// @brief Event: the first INIT could not be sent.
struct OpeningSendFailed {
  /// @brief Why.
  ClientError error{ClientError::transport_failure};
};
/// @brief Event: a DISCONNECT went out with fragments still unacknowledged.
struct DisconnectSent {
  /// @brief When the drain gives up waiting, in monotonic microseconds.
  std::uint64_t deadline_us{};
};
/// @brief Event: a drain ended, because its DISCONNECT was acknowledged or its
/// deadline passed.
struct DisconnectSettled {
  /// @brief The drain it ends; a settlement of an older drain is ignored.
  std::uint32_t drain{};
};
/// @brief Event: the socket was closed.
struct CloseCompleted {
  /// @brief Whether to report Closed.
  bool emit_closed{true};
};

/// @brief Effect: pick the verification tag.
struct GenerateStartMaterial {};
/// @brief Effect: start the handshake.
struct BeginHandshake {
  /// @brief The tag.
  VerificationTag local_tag{};
  /// @brief The initial sequence number.
  InitialSequenceNumber local_initial_sequence_number{};
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};
/// @brief Effect: create the reliable channel, and commit a fixed version or
/// send the negotiation offer.
struct ActivateSession {
  /// @brief What was agreed.
  EstablishedHandshake handshake{};
};
/// @brief Effect: close the socket.
struct CloseTransport {
  /// @brief Whether to report Closed afterwards.
  bool emit_closed{true};
};
/// @brief Effect: report the failure and close the socket.
struct PublishTerminalFailure {
  /// @brief Why.
  ClientError error{ClientError::invalid_state};
};
/// @brief Effect: report Closed.
struct PublishClosed {};
/// @brief Effect: make the current call fail with an error.
struct RefuseRequest {
  /// @brief The error.
  ClientError error{ClientError::invalid_state};
};

/// @brief Any effect; a TakionCommand effect executes that command.
using LifecycleEffect =
    std::variant<GenerateStartMaterial, BeginHandshake, ActivateSession,
                 TakionCommand, CloseTransport, PublishTerminalFailure,
                 PublishClosed, RefuseRequest>;

/// @brief A DISCONNECT drain in progress. Plain data.
struct DisconnectDrain {
  /// @brief Which drain this is, counted from 1.
  std::uint32_t id{};
  /// @brief When it gives up waiting, in monotonic microseconds.
  std::uint64_t deadline_us{};
};

/// @brief The lifecycle's memory, shared by reference with the machine.
struct LifecycleContext {
  /// @brief Makes the context.
  ///
  /// @param valid_configuration Whether the client's configuration passed
  ///     validation; start is refused otherwise.
  explicit LifecycleContext(bool valid_configuration) noexcept
      : configured(valid_configuration) {}

  /// @brief Whether the configuration passed validation.
  const bool configured;
  /// @brief Effects queued, oldest first.
  std::deque<LifecycleEffect> effects;
  /// @brief Why the client failed, once it has.
  std::optional<ClientError> terminal_error;
  /// @brief Whether a version is committed.
  bool profile_committed{};
  /// @brief Whether keys are installed.
  bool crypto_installed{};
  /// @brief Whether closing the socket has begun; it is attempted once.
  bool close_attempted{};
  /// @brief The DISCONNECT drain while disconnecting.
  std::optional<DisconnectDrain> drain;
  /// @brief Drains begun so far.
  std::uint32_t drains_begun{};
  /// @brief Events no transition applied to.
  std::size_t unexpected_events{};

  /// @brief Queues a refusal of the current call.
  ///
  /// @param error The error.
  void refuse_request(ClientError error) {
    effects.emplace_back(RefuseRequest{error});
  }

  /// @brief Records the terminal error and queues its report.
  ///
  /// @param error The error.
  void publish_terminal_failure(ClientError error) {
    terminal_error = error;
    effects.emplace_back(PublishTerminalFailure{error});
  }

  /// @brief Records the terminal error, queues its report, and refuses the
  /// current call.
  ///
  /// @param error The error.
  void record_terminal_failure(ClientError error) {
    publish_terminal_failure(error);
    refuse_request(error);
  }
};

/// @brief The client's transition table: Idle to Handshaking on a start with a
/// valid configuration; Handshaking to Connected when the handshake completes;
/// Connected to Disconnecting when a DISCONNECT is sent, and Disconnecting to
/// Stopping when that drain settles; any live state to Stopping on a stop, and
/// Stopping to Closed when the socket is closed; Handshaking, Connected,
/// Disconnecting or Stopping to Failed on a terminal failure. A start outside
/// Idle, or a stop in an unexpected state, is refused; a settlement of a drain
/// that is not the current one is ignored.
struct LifecycleFsm {
  /// @brief Builds the table.
  ///
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;

    const auto configured = [](const LifecycleContext& context) noexcept {
      return context.configured;
    };
    const auto generate_start_material = [](LifecycleContext& context) {
      context.effects.emplace_back(GenerateStartMaterial{});
    };
    const auto begin_handshake = [](const StartMaterialReady& event,
                                    LifecycleContext& context) {
      context.effects.emplace_back(BeginHandshake{
          event.local_tag,
          event.local_initial_sequence_number,
          event.now_microseconds,
      });
    };
    const auto retry_start = [](LifecycleContext& context,
                                const StartMaterialFailed& event) {
      context.refuse_request(event.error);
    };
    const auto activate_session = [](const HandshakeConnected& event,
                                     LifecycleContext& context) {
      context.effects.emplace_back(ActivateSession{event.handshake});
    };
    const auto begin_stop = [](const StopRequested& event,
                               LifecycleContext& context) {
      if (!context.close_attempted) {
        context.close_attempted = true;
        context.effects.emplace_back(CloseTransport{event.emit_closed});
      }
    };
    const auto begin_drain = [](const DisconnectSent& event,
                                LifecycleContext& context) {
      context.drain =
          DisconnectDrain{++context.drains_begun, event.deadline_us};
    };
    const auto current_drain = [](const DisconnectSettled& event,
                                  const LifecycleContext& context) noexcept {
      return context.drain && context.drain->id == event.drain;
    };
    const auto end_drain = [](LifecycleContext& context) {
      context.drain.reset();
      if (!context.close_attempted) {
        context.close_attempted = true;
        context.effects.emplace_back(CloseTransport{true});
      }
    };
    const auto abandon_drain = [](const StopRequested& event,
                                  LifecycleContext& context) {
      context.drain.reset();
      if (!context.close_attempted) {
        context.close_attempted = true;
        context.effects.emplace_back(CloseTransport{event.emit_closed});
      }
    };
    const auto fail_drain = [](const TerminalFailure& event,
                               LifecycleContext& context) {
      context.drain.reset();
      context.record_terminal_failure(event.error);
    };
    const auto complete_stop = [](const CloseCompleted& event,
                                  LifecycleContext& context) {
      if (event.emit_closed) context.effects.emplace_back(PublishClosed{});
    };
    const auto reject_configuration = [](LifecycleContext& context) {
      context.refuse_request(ClientError::invalid_configuration);
    };
    const auto record_failure = [](const TerminalFailure& event,
                                   LifecycleContext& context) {
      context.record_terminal_failure(event.error);
    };
    const auto record_opening_failure = [](const OpeningSendFailed& event,
                                           LifecycleContext& context) {
      context.publish_terminal_failure(event.error);
    };
    const auto ignore_event = [](LifecycleContext&) {};
    const auto unexpected_start = [](LifecycleContext& context) {
      ++context.unexpected_events;
      context.refuse_request(ClientError::invalid_state);
    };
    const auto unexpected_command = [](LifecycleContext& context) {
      ++context.unexpected_events;
      context.record_terminal_failure(ClientError::invalid_state);
    };
    const auto fail_unexpected_stopping_event = [](LifecycleContext& context) {
      ++context.unexpected_events;
      context.record_terminal_failure(ClientError::invalid_state);
    };

    return make_transition_table(
        *state<IdleState> +
            event<StartRequested>[configured] / generate_start_material =
            state<HandshakingState>,
        state<IdleState> +
            event<StartRequested>[!configured] / reject_configuration,

        state<HandshakingState> + event<StartMaterialReady> / begin_handshake,
        state<HandshakingState> + event<StartMaterialFailed> / retry_start =
            state<IdleState>,
        state<HandshakingState> + event<HandshakeConnected> / activate_session =
            state<ConnectedState>,

        state<ConnectedState> + event<DisconnectSent> / begin_drain =
            state<DisconnectingState>,
        state<DisconnectingState> + event<DisconnectSettled>[current_drain] /
                                        end_drain = state<StoppingState>,
        state<DisconnectingState> +
            event<DisconnectSettled>[!current_drain] / ignore_event,
        state<DisconnectingState> + event<StopRequested> / abandon_drain =
            state<StoppingState>,
        state<DisconnectingState> + event<TerminalFailure> / fail_drain =
            state<FailedState>,
        state<DisconnectingState> +
            event<StartRequested> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<DisconnectingState> +
            event<StartMaterialReady> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<DisconnectingState> +
            event<StartMaterialFailed> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<DisconnectingState> +
            event<HandshakeConnected> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<StoppingState> + event<DisconnectSettled> / ignore_event,
        state<ClosedState> + event<DisconnectSettled> / ignore_event,
        state<FailedState> + event<DisconnectSettled> / ignore_event,

        state<StoppingState> +
            event<StartRequested> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<StoppingState> +
            event<StartMaterialReady> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<StoppingState> +
            event<StartMaterialFailed> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<StoppingState> +
            event<HandshakeConnected> / fail_unexpected_stopping_event =
            state<FailedState>,
        state<IdleState> + event<StopRequested> / begin_stop =
            state<StoppingState>,
        state<HandshakingState> + event<StopRequested> / begin_stop =
            state<StoppingState>,
        state<ConnectedState> + event<StopRequested> / begin_stop =
            state<StoppingState>,
        state<StoppingState> + event<StopRequested> / ignore_event,
        state<ClosedState> + event<StopRequested> / ignore_event,
        state<FailedState> + event<StopRequested> / ignore_event,
        state<StoppingState> + event<CloseCompleted> / complete_stop =
            state<ClosedState>,
        state<FailedState> + event<CloseCompleted> / ignore_event,

        state<StoppingState> + event<TerminalFailure> / record_failure =
            state<FailedState>,

        state<HandshakingState> + event<TerminalFailure> / record_failure =
            state<FailedState>,
        state<HandshakingState> +
            event<OpeningSendFailed> / record_opening_failure =
            state<FailedState>,
        state<ConnectedState> + event<TerminalFailure> / record_failure =
            state<FailedState>,
        state<FailedState> + event<TerminalFailure> / ignore_event,

        state<_> + unexpected_event<StartRequested> / unexpected_start,
        state<_> + unexpected_event<StopRequested> / unexpected_command);
  }
};

}  // namespace libchiaking::net::protocol::takion::client_detail
