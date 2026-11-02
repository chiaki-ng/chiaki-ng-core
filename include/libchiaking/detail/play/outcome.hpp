/// @file
/// @brief Declares how a session ended: Outcome, EndReason, ConsoleQuit and
/// media faults.
#pragma once

#include "libchiaking/detail/media/media_fault.hpp"
#include "libchiaking/detail/play/launch_refused.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::play {

/// @brief Says what became of a request to put the console into rest mode on
/// leaving.
///
/// @see Leave::rest_console
enum class RestOutcome : std::uint8_t {
  /// @brief No such request was made.
  none,
  /// @brief The console was asked to rest.
  sent,
  /// @brief Rest was requested but the request was not sent.
  refused,
};

/// @brief Carries the reason a console gave for ending or refusing a session.
///
/// Consoles close remote play with a reason of their own. It is reported next
/// to the library's own EndReason because either can be the more useful one to
/// show.
///
/// @see Outcome::quit
class ConsoleQuit {
 public:
  /// @brief Names the console's reason.
  enum class Reason : std::uint8_t {
    /// @brief The session was stopped in the ordinary way; not an error.
    stopped,
    /// @brief The request to open a session failed for a reason the console
    /// did not give.
    session_request_unknown,
    /// @brief The console refused the connection that carries the session
    /// request.
    session_request_connection_refused,
    /// @brief Remote play is already in use on the console.
    console_in_use,
    /// @brief Remote play crashed on the console.
    remote_play_crashed,
    /// @brief The console's remote play version does not match the one this
    /// client speaks.
    remote_play_version_mismatch,
    /// @brief The control connection failed for a reason the console did
    /// not give.
    control_unknown,
    /// @brief The control connection could not be made.
    control_connect_failed,
    /// @brief The console refused the control connection.
    control_connection_refused,
    /// @brief The stream connection failed for a reason the console did not
    /// give.
    stream_unknown,
    /// @brief The console closed the stream connection.
    console_disconnected,
    /// @brief The console shut down or went to rest; not an error.
    console_shut_down,
    /// @brief The cloud server closed the stream because the streaming
    /// time limit was reached; not an error.
    time_limit_reached,
    /// @brief The cloud server ended a game trial whose play time ran
    /// out; not an error.
    trial_ended,
    /// @brief Pairing the console as part of an internet start failed.
    internet_pairing_failed,
  };

  /// @brief Longest console message kept, in bytes; longer text is cut.
  static constexpr std::size_t text_limit = 255;

  /// @brief Records a console's reason and optional message.
  ///
  /// @param reason Reason the console gave.
  /// @param text Message that came with it; cut to ConsoleQuit::text_limit.
  explicit ConsoleQuit(Reason reason, std::string_view text = {});

  /// @brief Returns the console's reason.
  ///
  /// @return The reason.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Returns the message the console sent with the reason; may be
  /// empty.
  ///
  /// @return The console's message, or an empty string.
  auto text() const noexcept -> const std::string& { return text_; }
  /// @brief Reports whether the reason describes a failure.
  ///
  /// @return `false` for Reason::stopped, Reason::console_shut_down,
  ///     Reason::time_limit_reached and Reason::trial_ended, `true` otherwise.
  auto is_error() const noexcept -> bool {
    return reason_ != Reason::stopped && reason_ != Reason::console_shut_down &&
           reason_ != Reason::time_limit_reached &&
           reason_ != Reason::trial_ended;
  }

  /// @brief Compares reason and message.
  friend auto operator==(const ConsoleQuit&, const ConsoleQuit&)
      -> bool = default;

 private:
  Reason reason_;
  std::string text_;
};

/// @brief Explains one failure of a session: what went wrong and during which
/// step.
///
/// An Outcome can hold two of these: the failure that ended the session
/// (Outcome::reason) and a failure while closing it down afterwards
/// (Outcome::cleanup).
class EndReason {
 public:
  /// @brief Names what went wrong.
  ///
  /// The `control_` causes concern the control connection to the console or
  /// server. The `session_request_` causes concern the request that opens a
  /// session, which comes before it.
  enum class Cause : std::uint8_t {
    /// @brief An operation was attempted in a state that does not allow it.
    invalid_state,
    /// @brief The preferences could not be turned into a valid stream
    /// request.
    invalid_preferences,
    /// @brief A platform provider the session needs is missing.
    provider_unavailable,
    /// @brief The control connection failed without a more specific cause.
    control_failed,
    /// @brief No network path to the console or server could be
    /// established.
    route_failed,
    /// @brief The stream stopped working without a more specific cause.
    pipeline_failed,
    /// @brief The console rejected the controller this client presented.
    controller_rejected,
    /// @brief The console rejected the login PIN.
    login_pin_rejected,
    /// @brief The session could not be closed down cleanly.
    shutdown_failed,
    /// @brief The control connection hit a network error.
    control_network_failure,
    /// @brief The control connection timed out.
    control_timeout,
    /// @brief A control response was cut short.
    control_truncated,
    /// @brief A control response could not be understood.
    control_invalid_response,
    /// @brief A control response exceeded the size this client accepts.
    control_too_large,
    /// @brief The console or server refused the control request.
    control_refused,
    /// @brief The control connection could not be handed over to the
    /// stream.
    control_handoff_failure,
    /// @brief The response to the session request was cut short.
    session_request_truncated,
    /// @brief The session request hit a network error.
    session_request_network_failure,
    /// @brief The session request timed out.
    session_request_timeout,
    /// @brief The response to the session request could not be understood.
    session_request_invalid_response,
    /// @brief The response to the session request exceeded the size this
    /// client accepts.
    session_request_too_large,
    /// @brief The console refused the session request.
    session_request_refused,
    /// @brief The console speaks a different remote play version.
    session_request_version_mismatch,
    /// @brief The console is already in a remote play session.
    session_request_in_use,
    /// @brief An argument was invalid.
    invalid_argument,
    /// @brief The console refused the connection for the session request.
    session_request_connection_refused,
    /// @brief The console or server refused the control connection.
    control_connection_refused,
    /// @brief A resource the session needs could not be obtained.
    resource_exhausted,
    /// @brief The console refused the session request because it no
    /// longer accepts the pairing kept for this account.
    ///
    /// The console answered the session request with "registration
    /// failed" or "unknown PSN account", which happens when the pairing
    /// was removed on the console or the console was reset. The pairing
    /// is still kept; the library never removes it on its own. Offer the
    /// player consoles::KnownConsoles::unpair, then pairing again.
    /// EndReason::console_reason still carries the console's code.
    pairing_rejected,
    /// @brief Over the internet, PSN connected to a PS4 that the player has
    /// not paired with this client.
    ///
    /// PSN reaches the account's primary PS4, which need not be the PS4 the
    /// session was started for. The session ends as soon as the console
    /// states who it is, before any stream is opened, and nothing about it
    /// is saved: neither the pairing it granted nor its name. The attempt is
    /// not retried. Ask the player to pair that PS4 at home first, or to make
    /// the intended PS4 the primary one on their account. Sessions with a
    /// PS5 never end this way.
    unpaired_console,
    /// @brief The console or cloud server stopped sending anything for
    /// longer than the stream's silence limit, as when the network drops.
    connection_lost,
  };

  /// @brief Names the step of connecting or playing during which a failure
  /// happened.
  ///
  /// The `cloud_` steps only occur in cloud sessions, and the account session
  /// and hole-punch steps only in internet sessions.
  enum class Step : std::uint8_t {
    /// @brief The step is not known.
    unknown,
    /// @brief Finding the console on the network.
    finding_console,
    /// @brief Pairing with the console.
    pairing,
    /// @brief Asking the console to open a session.
    session_request,
    /// @brief Opening the control connection.
    control_startup,
    /// @brief Obtaining the session identifier over the control connection.
    control_session_id,
    /// @brief Agreeing features over the control connection.
    control_features,
    /// @brief Receiving on the control connection.
    control_receive,
    /// @brief Measuring the connection before the stream starts.
    calibration,
    /// @brief Switching the session onto its stream route.
    route_switch,
    /// @brief Connecting the stream.
    stream_connect,
    /// @brief The stream's opening handshake.
    stream_handshake,
    /// @brief Setting up the stream's encryption.
    stream_crypto,
    /// @brief Receiving the description of the stream's video and audio.
    stream_description,
    /// @brief Receiving and handling media.
    media,
    /// @brief Attaching or using a controller.
    controller,
    /// @brief Opening the PSN session used to reach a console over the
    /// internet.
    account_session_open,
    /// @brief Starting that PSN session.
    account_session_start,
    /// @brief Opening a path through the router for the control connection.
    control_holepunch,
    /// @brief Opening a path through the router for the stream.
    data_holepunch,
    /// @brief Launching the title on the cloud, without a more specific
    /// step.
    cloud_launch,
    /// @brief Preparing the stream connection.
    stream_prepare,
    /// @brief Closing the session down.
    shutdown,
    /// @brief Checking that this device may use cloud streaming.
    cloud_device_check,
    /// @brief Obtaining the client identifiers for the cloud service.
    cloud_client_ids,
    /// @brief Fetching the cloud service's configuration.
    cloud_config,
    /// @brief Starting the cloud session.
    cloud_start,
    /// @brief Obtaining authorisation codes for the cloud session.
    cloud_codes,
    /// @brief Authorising the cloud session.
    cloud_authorise,
    /// @brief Locking the cloud session to this client.
    cloud_lock,
    /// @brief Measuring and choosing a datacenter.
    cloud_datacenter,
    /// @brief Waiting for a server to be allocated.
    cloud_allocate,
    /// @brief Resolving the title on the older cloud service.
    cloud_title,
    /// @brief Clearing the title for cloud streaming with the catalog.
    cloud_clearance,
  };

  /// @brief Supporting detail that accompanies a cause.
  struct Facts {
    /// @brief Step during which the failure happened.
    Step step{Step::unknown};
    /// @brief Reason code the console or server sent, or 0 when it sent
    /// none.
    std::uint32_t console_reason{};
    /// @brief HTTP status of the failed request, or 0 when the failure was
    /// not an HTTP response.
    std::uint16_t http_status{};
    /// @brief Whether the failure was the result of the player leaving, not
    /// a fault.
    bool left{};
    /// @brief Page the player can open to resolve the problem; empty when
    /// there is none.
    std::string action_url{};
    /// @brief Short description for diagnostics.
    std::string detail{};
    /// @brief Why a cloud launch was refused, when that is what failed.
    std::optional<LaunchRefused> launch_refused{};
  };

  /// @brief Longest action URL kept, in bytes; longer text is cut.
  static constexpr std::size_t url_limit = 2047;
  /// @brief Longest detail text kept, in bytes; longer text is cut.
  static constexpr std::size_t detail_limit = 200;

  /// @brief Records a cause with no supporting detail.
  ///
  /// @param cause What went wrong.
  explicit EndReason(Cause cause);
  /// @brief Records a cause with its supporting detail.
  ///
  /// @param cause What went wrong.
  /// @param facts Supporting detail. Facts::action_url is cut to
  ///     EndReason::url_limit and Facts::detail to EndReason::detail_limit.
  EndReason(Cause cause, Facts facts);

  /// @brief Returns what went wrong.
  ///
  /// @return The cause.
  auto cause() const noexcept -> Cause { return cause_; }
  /// @brief Returns the step during which the failure happened.
  ///
  /// @return The step, or EndReason::Step::unknown when it is not known.
  auto step() const noexcept -> Step { return facts_.step; }
  /// @brief Returns the reason code the console or server sent, or 0 when it
  /// sent none.
  ///
  /// @return The reason code, or 0.
  auto console_reason() const noexcept -> std::uint32_t {
    return facts_.console_reason;
  }
  /// @brief Returns the HTTP status of the failed request, or 0 when there
  /// was none.
  ///
  /// @return The HTTP status, or 0.
  auto http_status() const noexcept -> std::uint16_t {
    return facts_.http_status;
  }
  /// @brief Reports whether this "failure" is only the trace of the player
  /// leaving.
  ///
  /// @return `true` when the reason only records that the player left.
  auto was_left() const noexcept -> bool { return facts_.left; }
  /// @brief Returns a page the player can open to resolve the problem; empty
  /// when there is none.
  ///
  /// @return The page, or an empty string.
  auto action_url() const noexcept -> const std::string& {
    return facts_.action_url;
  }
  /// @brief Returns a short description for diagnostics; may be empty.
  auto detail() const noexcept -> const std::string& { return facts_.detail; }
  /// @brief Returns why a cloud launch was refused, when that is what failed.
  ///
  /// @return The refusal, or nothing when the failure was not a refused launch.
  auto launch_refused() const noexcept -> const std::optional<LaunchRefused>& {
    return facts_.launch_refused;
  }

 private:
  Cause cause_;
  Facts facts_;
};

/// @brief Says how a session ended, with everything known about why.
///
/// Every session ends with exactly one Outcome. It is delivered through
/// SessionEvents::on_ended and stays available from Session::outcome and
/// Session::wait_until_ended.
///
/// The Outcome weighs the evidence in a fixed order. A start that never began
/// is Ending::not_started. A session the player left without any failure is
/// Ending::left. If nothing failed while playing but closing down did, it is
/// Ending::shutdown_failed. Otherwise a failure is classed by where it
/// happened: Ending::media_failed, Ending::start_failed or
/// Ending::session_failed. With no failure at all the session is
/// Ending::finished, unless the console gave a reason other than
/// ConsoleQuit::Reason::stopped, which makes it Ending::console_quit.
class Outcome {
 public:
  /// @brief Names the way a session ended.
  enum class Ending : std::uint8_t {
    /// @brief The session ran and closed without a failure.
    finished,
    /// @brief The player left and nothing failed.
    left,
    /// @brief The session was refused before it began.
    not_started,
    /// @brief The connection could not be started.
    start_failed,
    /// @brief The connection failed after it had been started, while
    /// connecting or while playing.
    session_failed,
    /// @brief The session itself was fine but closing it down failed.
    shutdown_failed,
    /// @brief The frontend's video or audio handling failed; see
    /// Outcome::media_fault.
    media_failed,
    /// @brief The console ended the session for a reason of its own; see
    /// Outcome::quit.
    console_quit,
  };

  /// @brief Names the part of a session's life in which its main loop
  /// stopped.
  enum class Phase : std::uint8_t {
    /// @brief The loop ended without a failure.
    none,
    /// @brief Starting the connection failed.
    start,
    /// @brief The connection failed after it had been started.
    session,
    /// @brief The connection failed after it had been started and reported
    /// no cause.
    session_unknown,
    /// @brief A media failure stopped the session.
    media
  };

  /// @brief Raw findings an Outcome is classified from.
  ///
  /// The library fills this in when a session ends. Frontends read the result
  /// through Outcome and only build Evidence themselves in tests.
  struct Evidence {
    /// @brief Set when the session was refused before it began; the ending
    /// is then decided by this field alone.
    std::optional<EndReason::Cause> not_started{};
    /// @brief Whether the player asked to leave.
    bool left{};
    /// @brief Part of the session's life in which its main loop stopped.
    Phase phase{Phase::none};
    /// @brief Cause the main loop stopped with, when it recorded one.
    std::optional<EndReason::Cause> phase_cause{};
    /// @brief Media failure, if one occurred.
    std::optional<media::MediaFault> media{};
    /// @brief Failure reported by the connection to the console or server.
    std::optional<EndReason> primary{};
    /// @brief Cause closing the connection failed with, if it failed.
    std::optional<EndReason::Cause> stop_cause{};
    /// @brief Failure reported while closing the session down.
    std::optional<EndReason> cleanup{};
    /// @brief Reason the console gave, if it gave one.
    std::optional<ConsoleQuit> quit{};
    /// @brief What became of a request to put the console into rest mode.
    RestOutcome rest{RestOutcome::none};
    /// @brief One-line trace of the findings above, for diagnostics.
    std::string trace{};
  };

  /// @brief Classifies a session's ending from the findings.
  ///
  /// @param evidence What was observed when the session ended.
  explicit Outcome(Evidence evidence);
  /// @brief Builds the outcome of a session that was refused before it began.
  ///
  /// @param cause Why it was refused.
  /// @return An outcome with Ending::not_started and that cause.
  static auto not_started(EndReason::Cause cause) -> Outcome;

  /// @brief Returns the way the session ended.
  ///
  /// @return The ending.
  auto ending() const noexcept -> Ending { return ending_; }
  /// @brief Reports whether the session ended as Ending::finished.
  ///
  /// A session the player left is not counted as succeeded; test
  /// Outcome::was_left as well when leaving is not an error.
  ///
  /// @return `true` only for Ending::finished.
  auto succeeded() const noexcept -> bool {
    return ending_ == Ending::finished;
  }
  /// @brief Reports whether the player asked to leave.
  ///
  /// This can be `true` together with a failure ending, when something failed
  /// after the player had asked to leave.
  ///
  /// @return `true` when the player asked to leave.
  auto was_left() const noexcept -> bool { return evidence_.left; }
  /// @brief Returns the cause behind the ending.
  ///
  /// @return The cause for Ending::not_started, Ending::start_failed,
  ///     Ending::session_failed and Ending::shutdown_failed; `std::nullopt`
  ///     for the other endings.
  auto cause() const noexcept -> std::optional<EndReason::Cause> {
    return cause_;
  }
  /// @brief Returns the media failure, if one occurred.
  ///
  /// @return The media failure, or nothing when media never failed.
  auto media_fault() const noexcept -> const std::optional<media::MediaFault>& {
    return evidence_.media;
  }
  /// @brief Returns the failure that ended the session.
  ///
  /// @return The failure with its step and detail, owned by this Outcome;
  ///     `nullptr` when the connection reported none or it was only the trace
  ///     of the player leaving.
  auto reason() const noexcept -> const EndReason*;
  /// @brief Returns the failure that occurred while closing the session down.
  ///
  /// @return The failure, owned by this Outcome, or `nullptr` when closing
  ///     down succeeded.
  auto cleanup() const noexcept -> const EndReason*;
  /// @brief Returns why a cloud launch was refused.
  ///
  /// @return The refusal, owned by this Outcome, or `nullptr` when the
  ///     session did not end on a refused launch.
  /// @see LaunchRefused
  auto launch_refused() const noexcept -> const LaunchRefused*;
  /// @brief Returns the reason the console gave for ending the session, if it
  /// gave one.
  ///
  /// @return The console's reason, or nothing when it gave none.
  auto quit() const noexcept -> const std::optional<ConsoleQuit>& {
    return evidence_.quit;
  }
  /// @brief Returns what became of a request to put the console into rest
  /// mode.
  ///
  /// @return What became of the rest request.
  auto rest() const noexcept -> RestOutcome { return evidence_.rest; }
  /// @brief Returns a one-line trace of the findings, for diagnostics; not
  /// for display.
  ///
  /// @return The trace line.
  auto trace() const noexcept -> const std::string& { return evidence_.trace; }

 private:
  void settle(Ending ending, std::optional<EndReason::Cause> cause) noexcept;
  void classify() noexcept;

  Evidence evidence_;
  Ending ending_{Ending::finished};
  std::optional<EndReason::Cause> cause_{};
};

}  // namespace libchiaking::play
