/// @file
/// @brief Declares Trace, the structured log of a session's stages: one line
/// when a step starts, one when it ends with its outcome, error code and
/// duration, and warnings when the library takes a fallback.
///
/// Lines go to the host's DiagnosticsProvider in a fixed `key=value` shape so
/// that a support log can be read by people and searched by tools alike.
#pragma once
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace libchiaking::diagnostics {
/// @brief A step of finding, starting, running or ending a session, as
/// trace lines name it.
///
/// The order matters: label() lists one label per value in this order.
enum class Operation {
  /// @brief Looking for a UPnP Internet gateway on the local network.
  gateway_discover,
  /// @brief Asking the UPnP gateway to forward a UDP port.
  gateway_map,
  /// @brief Removing the port mappings made on the UPnP gateway.
  gateway_release,
  /// @brief Opening the extra sockets used to probe how the NAT allocates
  /// ports.
  nat_sockets,
  /// @brief A state change of the holepunch OFFER state machine.
  offer_state,
  /// @brief A state change of the holepunch ACCEPT state machine.
  accept_state,
  /// @brief Draining the peer's probe responses that arrive late.
  late_probe,
  /// @brief Downloading the preferred list of IPv4 STUN servers.
  stun_hosts,
  /// @brief Downloading the preferred list of IPv6 STUN servers.
  stun_hosts_ipv6,
  /// @brief Resolving a STUN server's host name.
  stun_resolve,
  /// @brief A STUN binding request, which learns the public address and port.
  stun_binding,
  /// @brief Whether the NAT allocates public ports at random, as measured.
  nat_random,
  /// @brief Building the request that registers (pairs) a console.
  registration_request,
  /// @brief The reliable-UDP handshake with a console reached over the
  /// internet.
  rudp_connect,
  /// @brief The console's acknowledgement that finishes registration.
  registration_finish,
  /// @brief One poll of the cloud session lock.
  lock_poll,
  /// @brief The pause before polling the cloud session lock again.
  lock_wait,
  /// @brief Which route candidate, by index, was chosen.
  candidate_selected,
  /// @brief Releasing the PSN session membership at the end.
  psn_release,
  /// @brief Closing the connection to PSN's push service.
  psn_push_close,
  /// @brief A stage that could not be identified.
  unknown,
  /// @brief Resolving the console's address.
  resolve,
  /// @brief Registering (pairing) with a console as a whole.
  registration,
  /// @brief The Remote Play session initialisation request to the console.
  session_init,
  /// @brief Opening the Remote Play control (ctrl) connection.
  ctrl_startup,
  /// @brief Waiting for the session identifier on the control connection.
  ctrl_session_id,
  /// @brief Negotiating features on the control connection.
  ctrl_features,
  /// @brief The control connection's receive loop.
  ctrl_receive,
  /// @brief Measuring the network before streaming starts.
  calibration,
  /// @brief Moving the stream's data onto another route.
  route_switch,
  /// @brief Connecting the stream transport (Takion).
  active_connect,
  /// @brief The BIG/BANG exchange that opens a stream.
  active_bang,
  /// @brief Setting up the stream's encryption.
  active_crypto,
  /// @brief Receiving the stream's information from the console.
  stream_info,
  /// @brief Delivering audio and video.
  media,
  /// @brief Delivering controller input.
  controller,
  /// @brief Creating the PSN session an internet start uses.
  psn_open,
  /// @brief Waking the console and asking it, through PSN, to start Remote
  /// Play.
  psn_start,
  /// @brief Punching through NAT for the control route.
  control_holepunch,
  /// @brief Punching through NAT for the data route.
  data_holepunch,
  /// @brief Provisioning a cloud streaming session as a whole.
  cloud_provision,
  /// @brief Preparing the active stream before it connects.
  active_prepare,
  /// @brief Cleaning up after a session.
  shutdown,
  /// @brief Reading or creating the cloud installation's identity.
  cloud_identity,
  /// @brief Fetching the cloud client identifiers.
  cloud_client_ids,
  /// @brief Fetching the cloud service configuration.
  cloud_config,
  /// @brief Creating the cloud session.
  cloud_start,
  /// @brief Obtaining the OAuth codes the cloud service needs.
  cloud_oauth,
  /// @brief Authorising the entitlement to stream the title.
  cloud_authorize,
  /// @brief Acquiring the cloud session lock.
  cloud_lock,
  /// @brief Measuring datacenters and choosing one.
  cloud_datacenter,
  /// @brief Allocating a cloud instance.
  cloud_allocate,
  /// @brief Resolving the LegacyCloudStream title to stream.
  cloud_title,
  /// @brief Binding the sockets used for holepunching.
  sockets,
  /// @brief Gathering the local and public route candidates.
  candidate_gather,
  /// @brief Building the holepunch OFFER message.
  offer_prepare,
  /// @brief Sending the OFFER and waiting for the RESULT.
  offer_exchange,
  /// @brief Probing route candidates to find one that works.
  candidate_probe,
  /// @brief Exchanging the holepunch ACCEPT message.
  accept_exchange,
  /// @brief How many route candidates are available.
  candidate_summary,
  /// @brief Retrying with an owned entitlement after the first was refused.
  entitlement_retry,
  /// @brief One poll of a cloud instance allocation.
  allocation_poll,
  /// @brief The pause before polling the allocation again.
  allocation_wait,
  /// @brief Retrying a console start with another protocol version.
  local_version_retry,
  /// @brief The first encoded audio arriving.
  first_audio,
  /// @brief The first encoded video arriving.
  first_video,
  /// @brief A state change of the stream state machine.
  stream_state,
  /// @brief A state change of the active handshake state machine.
  active_state,
  /// @brief An observation of the media stream's health.
  media_health,
  /// @brief The failure that ended the session.
  primary_failure,
  /// @brief A failure while cleaning up, after the session had already ended.
  cleanup_failure,
  /// @brief The frontend asking the session to stop.
  stop_requested,
  /// @brief The checks run before a cloud launch.
  cloud_launch_check,
  /// @brief Measuring the bandwidth to the cloud datacenter.
  cloud_bandwidth
};
/// @brief How a traced step went.
enum class Outcome { begin, succeeded, failed, cancelled, observed, retry };
/// @var Outcome::begin
/// @brief The step started.
/// @var Outcome::succeeded
/// @brief The step finished successfully.
/// @var Outcome::failed
/// @brief The step failed; logged as an error.
/// @var Outcome::cancelled
/// @brief The step ended because a stop was requested.
/// @var Outcome::observed
/// @brief Not a step but a fact noticed along the way, such as a state
/// change or a measured value.
/// @var Outcome::retry
/// @brief The step failed and will be tried again; logged as a warning.

/// @brief Which kind of session a trace line belongs to; every line starts
/// with it.
enum class Component {
  local,
  remote,
  control_route,
  data_route,
  cloud,
  legacy_cloud_stream
};
/// @var Component::local
/// @brief A console on the local network.
/// @var Component::remote
/// @brief A console reached over the internet through PSN.
/// @var Component::control_route
/// @brief The control route of an internet session; logged as
/// `remote.control`.
/// @var Component::data_route
/// @brief The data route of an internet session; logged as `remote.data`.
/// @var Component::cloud
/// @brief A PlayStation Plus cloud streaming session.
/// @var Component::legacy_cloud_stream
/// @brief A LegacyCloudStream cloud streaming session.

/// @brief Spells an operation as its log keyword, such as `stun_binding`.
///
/// @param op The operation.
/// @return The enumerator's name in static storage, or `unknown` for a value
///     outside the enumeration.
inline auto name(Operation op) noexcept -> const char* {
  const auto spelled = magic_enum::enum_name(op);
  return spelled.empty() ? "unknown" : spelled.data();
}
/// @brief Spells an outcome as its log keyword, such as `succeeded`.
///
/// @param outcome The outcome.
/// @return The enumerator's name in static storage, or `unknown` for a value
///     outside the enumeration.
inline auto name(Outcome outcome) noexcept -> const char* {
  const auto spelled = magic_enum::enum_name(outcome);
  return spelled.empty() ? "unknown" : spelled.data();
}
/// @brief Spells a component as its log prefix, such as `remote.data`.
///
/// @param component The component.
/// @return The prefix in static storage, or `unknown` for a value outside the
///     enumeration.
inline auto name(Component component) noexcept -> const char* {
  constexpr std::array<const char*, magic_enum::enum_count<Component>()> names{
      "local", "remote", "remote.control", "remote.data", "cloud", "psnow"};
  const auto index = magic_enum::enum_index(component);
  return index ? names[*index] : "unknown";
}
/// @brief Describes an operation in words, for the readable part of a line,
/// such as "STUN binding request".
///
/// @param op The operation.
/// @return The description in static storage, or "Unknown stage" for a value
///     outside the enumeration.
inline auto label(Operation op) noexcept -> const char* {
  constexpr std::array<const char*, magic_enum::enum_count<Operation>()> labels{
      "Discover UPnP gateway",
      "Map UDP port through gateway",
      "Release gateway port mappings",
      "Additional NAT probe sockets",
      "Holepunch OFFER FSM",
      "Holepunch ACCEPT FSM",
      "Late peer probe responses",
      "STUN server list lookup",
      "IPv6 STUN server list lookup",
      "STUN server resolution",
      "STUN binding request",
      "Random NAT allocation detection",
      "Registration request construction",
      "RUDP handshake",
      "Registration FINISH acknowledgement",
      "Cloud lock poll",
      "Cloud lock retry delay",
      "Selected route candidate index",
      "PSN session release",
      "PSN push connection close",
      "Unknown stage",
      "Console address resolution",
      "Console registration",
      "Remote Play session initialization",
      "Ctrl connection",
      "Ctrl session identifier",
      "Ctrl feature negotiation",
      "Ctrl receive loop",
      "Network calibration",
      "Data route switch",
      "Stream transport connection",
      "Stream BANG negotiation",
      "Stream encryption setup",
      "Stream information",
      "Media delivery",
      "Controller delivery",
      "PSN session creation",
      "Console wake and PSN start",
      "Control route hole punching",
      "Data route hole punching",
      "Cloud provisioning",
      "Active stream preparation",
      "Session cleanup",
      "Cloud installation identity",
      "Cloud client configuration",
      "Cloud service configuration",
      "Cloud session creation",
      "Cloud OAuth authorization",
      "Cloud entitlement authorization",
      "Cloud session lock",
      "Datacenter measurement and selection",
      "Cloud instance allocation",
      "LegacyCloudStream title resolution",
      "Holepunch socket binding",
      "Route candidate gathering",
      "Holepunch OFFER construction",
      "Holepunch OFFER and RESULT exchange",
      "Route candidate probing",
      "Holepunch ACCEPT exchange",
      "Available route candidates",
      "Owned entitlement fallback",
      "Cloud allocation poll",
      "Cloud allocation retry delay",
      "Console protocol version retry",
      "First encoded audio received",
      "First encoded video received",
      "Stream FSM",
      "Active handshake FSM",
      "Media health",
      "Primary failure",
      "Cleanup failure",
      "Stop request",
      "Cloud launch checks",
      "Cloud bandwidth measurement",
  };
  const auto index = magic_enum::enum_index(op);
  return index ? labels[*index] : "Unknown stage";
}

/// @brief Reduces an error to the number a trace line logs as its `value`.
///
/// An enumeration gives its underlying value; a struct with a `kind` member,
/// or else a `step` member, gives that member's code, recursively; anything
/// else gives 0.
///
/// @tparam E The error type.
/// @param error The error.
/// @return The error's code.
template <class E>
auto error_code(const E& error) noexcept -> std::uint64_t {
  if constexpr (std::is_enum_v<E>)
    return static_cast<std::uint64_t>(error);
  else if constexpr (requires { error.kind; })
    return error_code(error.kind);
  else if constexpr (requires { error.step; })
    return error_code(error.step);
  else
    return 0;
}

/// @brief Writes the trace lines of one session, or one part of it, to the
/// host's log.
///
/// Holds the log, the clock and the stop token by reference or copy; the log
/// and clock must outlive it. Without a log every call does nothing, so code
/// can trace unconditionally. Writing never throws and never blocks beyond
/// the log's own write. Copyable; a Trace may be used from several threads
/// at once, as DiagnosticsProvider::write must accept concurrent calls.
class Trace {
 public:
  /// @brief A fallback or workaround the library chose, logged as a warning
  /// with its reason.
  enum class Decision {
    /// @brief Calibration failed, so the route kept from before is used
    /// with reference calibration values.
    retained_calibration_route,
    /// @brief Calibration failed and a fallback route exists, so a fresh
    /// active route is opened.
    fresh_calibration_route,
    /// @brief The route skips calibration and runs with reference
    /// calibration values, as on an internet route.
    uncalibrated_route,
    /// @brief The preferred STUN server list could not be downloaded, so
    /// the built-in servers are used.
    builtin_stun_servers,
    /// @brief No STUN server answered, so only the local address is offered
    /// and the console's answers to our probes have to open the route; the
    /// detail is the discovery error.
    unmapped_offer,
    /// @brief The console sent no session identifier before the deadline,
    /// so one is synthesised.
    synthetic_session_id,
    /// @brief The first entitlement was refused, so an owned entitlement is
    /// resolved again.
    owned_entitlement,
    /// @brief The cloud instance is not ready, so allocation is polled
    /// again; the detail is the delay in seconds.
    allocation_waiting,
    /// @brief The allocation request failed transiently and is retried; the
    /// detail is the delay in seconds.
    allocation_network_retry,
    /// @brief The session lock is not acquired yet, so it is polled again;
    /// the detail is the delay in seconds.
    lock_waiting,
    /// @brief The lock request failed transiently and is retried; the
    /// detail is the delay in seconds.
    lock_network_retry,
    /// @brief The console's BANG carries a session key that does not match
    /// the synthesised session identifier sent in BIG, and is
    /// accepted anyway.
    bang_session_key_mismatch
  };

  /// @brief Starts tracing for a component.
  ///
  /// @param sink The host's log, or null to trace nothing.
  /// @param clock The monotonic clock durations are measured with; must
  ///     outlive the trace.
  /// @param component The component every line is prefixed with.
  /// @param stop The session's stop token; a step that fails after it fired
  ///     is logged as cancelled rather than failed.
  Trace(platform::DiagnosticsProvider* sink,
        const platform::ClockProvider& clock, Component component,
        platform::StopToken stop = {}) noexcept
      : sink_(sink),
        clock_(&clock),
        component_(component),
        stop_(std::move(stop)) {}

  /// @brief Writes one line about a step.
  ///
  /// @param op The step.
  /// @param outcome How it went; decides the line's severity.
  /// @param value A number to log with it: an error code, a count or a
  ///     measurement, depending on the step.
  /// @param duration_us How long the step took, in microseconds of the
  ///     monotonic clock; 0 when not measured.
  void record(Operation op, Outcome outcome, std::uint64_t value = 0,
              std::uint64_t duration_us = 0) const noexcept {
    write_record(op, outcome, value, duration_us, "none");
  }
  /// @brief Writes an observed state of a state machine, with its numeric
  /// value and name.
  ///
  /// @tparam State The state machine's state enumeration.
  /// @param op The state machine's step.
  /// @param state The state it is now in.
  template <class State>
    requires std::is_enum_v<State>
  void record_state(Operation op, State state) const noexcept {
    const auto spelled = magic_enum::enum_name(state);
    write_record(op, Outcome::observed,
                 static_cast<std::uint64_t>(std::to_underlying(state)), 0,
                 spelled.empty() ? std::string_view{"none"} : spelled);
  }
  /// @brief Writes a warning that the library took a fallback.
  ///
  /// @param decision The fallback.
  /// @param observed The number that led to it, logged as the detail; when
  ///     nothing is known, the detail is 0 and the reason `unknown`.
  void decision(Decision decision,
                std::optional<std::uint64_t> observed = std::uint64_t{
                    0}) const noexcept {
    write_decision(decision, observed.value_or(0),
                   observed ? std::string_view{"described above"}
                            : std::string_view{"unknown"});
  }
  /// @brief Writes a warning that the library took a fallback, naming the
  /// failure that caused it.
  ///
  /// @tparam Reason The failure's enumeration.
  /// @param decision The fallback.
  /// @param reason The failure, logged by value and name; nothing when it
  ///     is unknown.
  template <class Reason>
    requires std::is_enum_v<Reason>
  void decision(Decision decision,
                std::optional<Reason> reason) const noexcept {
    if (!reason) {
      this->decision(decision, std::optional<std::uint64_t>{});
      return;
    }
    const auto spelled = magic_enum::enum_name(*reason);
    write_decision(
        decision, static_cast<std::uint64_t>(std::to_underlying(*reason)),
        spelled.empty() ? std::string_view{"described above"} : spelled);
  }
  /// @brief Runs a step, writing a line when it starts and one when it ends.
  ///
  /// Runs `operation` on the calling thread and blocks for as long as it
  /// does.
  ///
  /// @tparam F A callable taking no arguments and returning an Expected or
  ///     anything else that tests as success and has `error()`.
  /// @param op The step.
  /// @param operation The step's work.
  /// @return What `operation` returned.
  template <class F>
  auto call(Operation op, F&& operation) const {
    record(op, Outcome::begin);
    return call_from(op, now(), std::forward<F>(operation));
  }
  /// @brief Runs a step that started earlier, writing only the line when it
  /// ends, its duration counted from `begin`.
  ///
  /// The end line is succeeded, cancelled when the stop token has fired,
  /// or failed with error_code() of the error.
  ///
  /// @tparam F As for call().
  /// @param op The step.
  /// @param begin When the step started, a reading of now().
  /// @param operation The step's remaining work.
  /// @return What `operation` returned.
  template <class F>
  auto call_from(Operation op, std::uint64_t begin, F&& operation) const {
    auto result = std::forward<F>(operation)();
    const auto end = now();
    record(op,
           result                   ? Outcome::succeeded
           : stop_.stop_requested() ? Outcome::cancelled
                                    : Outcome::failed,
           result ? 0 : error_code(result.error()),
           end >= begin ? end - begin : 0);
    return result;
  }
  /// @brief Reads the trace's clock.
  ///
  /// @return Microseconds of the monotonic clock.
  auto now() const noexcept -> std::uint64_t {
    return clock_->now_microseconds();
  }
  /// @brief Returns the log lines go to.
  ///
  /// @return The log, or null when tracing is off.
  auto sink() const noexcept -> platform::DiagnosticsProvider* { return sink_; }
  /// @brief Returns the component lines are prefixed with.
  ///
  /// @return The component.
  auto component() const noexcept -> Component { return component_; }

 private:
  /// @brief The longest line written, in bytes; longer lines are cut.
  static constexpr std::size_t line_limit = 639;

  /// @brief Words an outcome for the readable part of a line.
  ///
  /// @param outcome The outcome.
  /// @return "started", "completed", "failed", "cancelled", "retrying" or
  ///     "observed".
  static auto result_of(Outcome outcome) noexcept -> std::string_view {
    switch (outcome) {
      case Outcome::begin:
        return "started";
      case Outcome::succeeded:
        return "completed";
      case Outcome::failed:
        return "failed";
      case Outcome::cancelled:
        return "cancelled";
      case Outcome::retry:
        return "retrying";
      case Outcome::observed:
        break;
    }
    return "observed";
  }
  /// @brief Picks a line's severity from its outcome.
  ///
  /// @param outcome The outcome.
  /// @return Error for failed, warning for retry, info otherwise.
  static auto severity_of(Outcome outcome) noexcept -> platform::LogSeverity {
    if (outcome == Outcome::failed) return platform::LogSeverity::error;
    return outcome == Outcome::retry ? platform::LogSeverity::warning
                                     : platform::LogSeverity::info;
  }
  /// @brief Words a fallback for its warning line.
  ///
  /// @param decision The fallback.
  /// @return The sentence explaining it.
  static auto message_of(Decision decision) noexcept -> std::string_view {
    switch (decision) {
      case Decision::retained_calibration_route:
        return "Using the retained route with reference calibration "
               "because calibration failed";
      case Decision::fresh_calibration_route:
        return "Opening a fresh active route because calibration failed "
               "and a fallback is available";
      case Decision::uncalibrated_route:
        return "Skipping calibration and switching straight to the stream "
               "with reference calibration values";
      case Decision::builtin_stun_servers:
        return "Using built-in STUN servers because the preferred "
               "server-list request failed";
      case Decision::unmapped_offer:
        return "Offering only the local address because no STUN server "
               "answered";
      case Decision::synthetic_session_id:
        return "Synthesizing a session identifier because the console "
               "did not provide one before the deadline";
      case Decision::owned_entitlement:
        return "Resolving an owned entitlement again because the "
               "initial entitlement was rejected";
      case Decision::allocation_waiting:
        return "Polling allocation again because the instance is not "
               "ready; delay in seconds";
      case Decision::allocation_network_retry:
        return "Retrying allocation because the request failed "
               "transiently; delay in seconds";
      case Decision::lock_waiting:
        return "Polling the session lock again because it is not "
               "acquired; delay in seconds";
      case Decision::lock_network_retry:
        return "Retrying the session lock because the request failed "
               "transiently; delay in seconds";
      case Decision::bang_session_key_mismatch:
        return "Accepting a BANG whose session key does not match the "
               "synthesized session identifier sent in BIG";
    }
    return "Unknown decision";
  }

  /// @brief Cuts a line to line_limit and writes it; the log must be set.
  ///
  /// @param severity The line's severity.
  /// @param line The line.
  void write(platform::LogSeverity severity, std::string line) const noexcept {
    if (line.size() > line_limit) line.resize(line_limit);
    sink_->write({severity, line});
  }
  /// @brief Formats and writes a step line, when there is a log.
  ///
  /// A formatting failure drops the line silently.
  ///
  /// @param op The step.
  /// @param outcome How it went.
  /// @param value The number logged with it.
  /// @param duration_us Its duration in microseconds.
  /// @param state The state name logged with it, or `none`.
  void write_record(Operation op, Outcome outcome, std::uint64_t value,
                    std::uint64_t duration_us,
                    std::string_view state) const noexcept {
    if (!sink_) return;
    try {
      write(severity_of(outcome),
            std::format("{}: {} {}. {} operation={} outcome={} value={} "
                        "duration_us={} state={}",
                        name(component_), label(op), result_of(outcome),
                        name(component_), name(op), name(outcome), value,
                        duration_us, state));
    } catch (...) {
    }
  }
  /// @brief Formats and writes a fallback warning, when there is a log.
  ///
  /// @param decision The fallback.
  /// @param cause The number logged as the detail.
  /// @param reason The failure's name, or a placeholder.
  void write_decision(Decision decision, std::uint64_t cause,
                      std::string_view reason) const noexcept {
    if (!sink_) return;
    try {
      write(platform::LogSeverity::warning,
            std::format("{}: {}. detail={} reason={}", name(component_),
                        message_of(decision), cause, reason));
    } catch (...) {
    }
  }

  /// @brief The host's log, or null.
  platform::DiagnosticsProvider* sink_;
  /// @brief The monotonic clock; never null.
  const platform::ClockProvider* clock_;
  /// @brief The prefix of every line.
  Component component_;
  /// @brief The session's stop token, to tell cancellation from failure.
  platform::StopToken stop_;
};

/// @brief Runs a step through a trace when there is one, and plainly
/// otherwise.
///
/// @tparam F As for Trace::call().
/// @param trace The trace, or null.
/// @param op The step.
/// @param operation The step's work, run on the calling thread.
/// @return What `operation` returned.
template <class F>
auto observe(const Trace* trace, Operation op, F&& operation) {
  if (trace) return trace->call(op, std::forward<F>(operation));
  return std::forward<F>(operation)();
}

}  // namespace libchiaking::diagnostics
