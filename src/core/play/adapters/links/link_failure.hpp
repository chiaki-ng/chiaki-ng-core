/// @file
/// @brief Declares LinkFailure, why a link failed: the stage it was at, the
/// stream error, and the detailed cause from whichever layer failed.
#pragma once

#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/error.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/links/internet/psn_failure.hpp"
#include "src/core/play/adapters/links/internet/route_failure.hpp"
#include "src/core/play/adapters/links/internet/wake_failure.hpp"
#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/model/venue.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::play {

/// @brief A link's failure: the stage, the stream error and one detailed
/// cause, with everything a log line, the player's message and the public
/// EndReason need from it.
///
/// The cause is one of many layer-specific failures, or none. A failure may
/// be marked cancelled, when the player or the session stopped it, and is
/// stamped with the venue it happened on. A value; safe to copy and share
/// once built.
class LinkFailure {
 public:
  /// @brief A stage a link goes through.
  using Stage = net::protocol::remote_play::FailureStage;
  /// @brief The stream's own error.
  using Error = net::protocol::remote_play::StreamError;

  /// @brief The audio channel count asked for is not one the target streams.
  class AudioRequestRefused {
   public:
    /// @brief Checks a channel count against a console.
    ///
    /// @param target The console's registration target; a PS5 or a PS4.
    /// @param channels The channel count asked for; 0 means the default.
    /// @return The refusal when the console cannot stream that count; empty
    ///     otherwise.
    static auto for_console(
        net::protocol::remote_play::RegistrationTarget target,
        std::uint64_t channels) noexcept -> std::optional<AudioRequestRefused>;
    /// @brief Checks a channel count against a cloud service.
    ///
    /// @param service LegacyCloudStream or PS5 cloud.
    /// @param channels The channel count asked for; 0 means the default.
    /// @return The refusal when the service cannot stream that count; empty
    ///     otherwise.
    static auto for_cloud(cloud::CloudService service,
                          std::uint64_t channels) noexcept
        -> std::optional<AudioRequestRefused>;
    /// @brief Describes the refusal.
    ///
    /// @return That surround is not supported for a valid count, or that the
    ///     count is not supported.
    auto describe() const noexcept -> std::string_view;

   private:
    /// @brief Records the refused count.
    ///
    /// @param channels The channel count.
    explicit AudioRequestRefused(std::uint64_t channels) noexcept
        : channels_(channels) {}
    /// @brief Checks a channel count against a target.
    ///
    /// @param target The target.
    /// @param channels The channel count.
    /// @return The refusal; empty when supported.
    static auto checked(net::protocol::remote_play::AudioRequestTarget target,
                        std::uint64_t channels) noexcept
        -> std::optional<AudioRequestRefused>;

    /// @brief The channel count asked for.
    std::uint64_t channels_;
  };

  /// @brief The console's address could not be resolved.
  struct ConsoleUnresolved {
    /// @brief The resolver's error.
    platform::NetError error;
  };

  /// @brief The console refused with a reason code.
  struct ConsoleRefusal {
    /// @brief The console's reason code.
    std::uint32_t reason{};
    /// @brief The HTTP status of the refusal.
    std::uint16_t http_status{};
  };

  /// @brief The control channel's startup exchange failed.
  struct CtrlStartup {
    /// @brief The exchange's error.
    net::protocol::remote_play::LocalCtrlError error;
    /// @brief Bytes sent before it failed.
    std::uint64_t sent{};
    /// @brief Bytes received before it failed.
    std::uint64_t received{};
  };

  /// @brief Reading from the internet control connection failed.
  struct RemoteCtrlRead {
    /// @brief The read's error.
    internet::remote_ctrl::PumpError error;
    /// @brief The size of the packet being read, in bytes.
    std::uint64_t packet_bytes{};
    /// @brief The RUDP framing error, when framing failed.
    std::optional<net::transport::rudp::FrameError> framing{};
  };

  /// @brief Draining stale datagrams after the transport switch failed.
  struct SwitchDrain {
    /// @brief The drain's error.
    net::transport::rudp::RemoteDatagramDrainError error;
  };

  /// @brief The detailed cause: none, or the failure of the layer that failed.
  using Cause = std::variant<
      std::monostate, AudioRequestRefused, ConsoleUnresolved, ConsoleRefusal,
      CtrlStartup, net::protocol::remote_play::CtrlReaderError,
      net::protocol::remote_play::ActiveSessionFailure,
      net::protocol::senkusha::Error, net::protocol::takion::ClientError,
      platform::PlatformError, PsnFailure, WakeFailure, RouteFailure,
      internet::PsnRemoteRegistrationFailure,
      net::protocol::remote_play::RemoteSessionInitFailure,
      internet::remote_ctrl::ConnectFailure,
      internet::remote_ctrl::OutboundStepError, RemoteCtrlRead,
      net::protocol::remote_play::RudpSwitchBarrierError, SwitchDrain,
      net::transport::rudp::RemoteDatagramDrainError, LaunchFailure, EndReason>;

  /// @brief The reason a console gives when it is shutting down the stream.
  static constexpr std::string_view remote_shutdown_reason =
      "Server shutting down";
  /// @brief The reason a cloud server gives when it ends the stream because
  /// the streaming time limit, such as a trial's play time, was reached.
  static constexpr std::string_view time_limit_reason = "MaxPlaytimeTimeout";
  /// @brief The reason a cloud server gives when it ends a game trial whose
  /// play time ran out; it only means that after a trial time warning.
  static constexpr std::string_view server_shutdown_reason =
      "Shutdown requested by server";

  /// @brief Builds a failure.
  ///
  /// @param stage The stage.
  /// @param result The stream error.
  /// @param cause The detailed cause; none by default.
  LinkFailure(Stage stage, Error result, Cause cause = std::monostate{});

  /// @brief A failure of the active stream after it was set up.
  ///
  /// @param failure The active session's failure.
  /// @return A pipeline failure at the stage the handshake was in.
  static auto lost(net::protocol::remote_play::ActiveSessionFailure failure)
      -> LinkFailure;
  /// @brief A session request the console refused.
  ///
  /// @param failure The session request's failure.
  /// @return A session_init failure, cancelled when the request was stopped.
  static auto refused(
      const net::protocol::remote_play::RemoteSessionInitFailure& failure)
      -> LinkFailure;
  /// @brief An internet control connection that could not be made.
  ///
  /// @param failure The connection's failure.
  /// @return A ctrl_startup failure, cancelled when it was stopped.
  static auto refused(const internet::remote_ctrl::ConnectFailure& failure)
      -> LinkFailure;
  /// @brief An internet pairing that failed.
  ///
  /// @param failure The registration's failure.
  /// @return A registration route failure, cancelled when it was stopped.
  static auto unpaired(const internet::PsnRemoteRegistrationFailure& failure)
      -> LinkFailure;
  /// @brief A cloud launch that failed.
  ///
  /// @param failure The launch's failure.
  /// @return A route failure at the cloud stage the launch failed in.
  static auto unlaunched(LaunchFailure failure) -> LinkFailure;
  /// @brief A failure a scripted link reports.
  ///
  /// @param reason The reported end reason.
  /// @return The failure at the reason's stage, cancelled when the player
  ///     left.
  static auto reported(const EndReason& reason) -> LinkFailure;
  /// @brief A step given up because the link was stopped.
  ///
  /// @param stage The stage.
  /// @param result The stream error.
  /// @return A cancelled failure.
  static auto abandoned(Stage stage, Error result) -> LinkFailure;
  /// @brief The PSN signalling connection was lost.
  ///
  /// @param stage The stage.
  /// @param error The push connection's error.
  /// @return A shutdown failure when an acknowledgement was lost during
  ///     shutdown; a route failure otherwise.
  static auto signalling_lost(Stage stage, psn::remote_play::PushError error)
      -> LinkFailure;
  /// @brief Closing the stream failed.
  ///
  /// @param error The Takion client's error, when it gave one.
  /// @return A shutdown failure.
  static auto shut_down(std::optional<net::protocol::takion::ClientError> error)
      -> LinkFailure;

  /// @brief Reports whether the console ended the stream because it is
  /// shutting down.
  ///
  /// @param failure The active session's failure.
  /// @return `true` for a peer disconnect with
  /// LinkFailure::remote_shutdown_reason.
  static auto shuts_down(
      const net::protocol::remote_play::ActiveSessionFailure& failure) noexcept
      -> bool;
  /// @brief The quit reason an active session's failure stands for.
  ///
  /// A peer disconnect is matched exactly against
  /// LinkFailure::remote_shutdown_reason, LinkFailure::time_limit_reason and,
  /// after a trial time warning, LinkFailure::server_shutdown_reason; no other
  /// text is interpreted.
  ///
  /// @param failure The active session's failure.
  /// @param trial_warned Whether the server warned that a game trial's play
  ///     time was running out during this stream.
  /// @return QuitReason::stream_connection_remote_shutdown,
  ///     QuitReason::stream_connection_time_limit_reached or
  ///     QuitReason::stream_connection_trial_ended for those reasons,
  ///     QuitReason::stream_connection_remote_disconnected for any other peer
  ///     disconnect, and QuitReason::stream_connection_unknown for a failure
  ///     that is not a peer disconnect.
  static auto peer_quit(
      const net::protocol::remote_play::ActiveSessionFailure& failure,
      bool trial_warned = false) noexcept
      -> net::protocol::remote_play::QuitReason;
  /// @brief Logs a peer disconnect and its reason; ignored for other failures
  /// or without a log.
  ///
  /// @param diagnostics Receives the lines; may be null.
  /// @param failure The active session's failure.
  static void log_disconnect(
      platform::DiagnosticsProvider* diagnostics,
      const net::protocol::remote_play::ActiveSessionFailure& failure) noexcept;

  /// @brief Stamps the venue the failure happened on.
  ///
  /// @param venue The venue.
  void reached_through(Venue venue) noexcept { route_ = venue; }
  /// @brief Marks the failure cancelled.
  void cancel() noexcept { cancelled_ = true; }

  /// @brief Reads the stage.
  ///
  /// @return The stage.
  auto stage() const noexcept -> Stage { return stage_; }
  /// @brief Reads the stream error.
  ///
  /// @return The error.
  auto result() const noexcept -> Error { return result_; }
  /// @brief Reads the venue.
  ///
  /// @return The venue; empty when not stamped.
  auto route() const noexcept -> std::optional<Venue> { return route_; }
  /// @brief Reads the cause.
  ///
  /// @return The cause.
  auto cause() const noexcept -> const Cause& { return cause_; }
  /// @brief Reads the cause as one alternative.
  ///
  /// @tparam T One of the Cause alternatives.
  /// @return The cause when it is a @p T; null otherwise.
  template <class T>
  auto as() const noexcept -> const T* {
    return std::get_if<T>(&cause_);
  }

  /// @brief Reports whether the failure was a cancellation.
  ///
  /// @return `true` when marked, or when the PSN, wake, route or switch cause
  ///     says it was stopped.
  auto cancelled() const noexcept -> bool;
  /// @brief Reports whether the attempt failed because this machine had no
  /// network, so it never reached PSN or the console.
  ///
  /// @return `true` when the PSN cause says so; `false` otherwise, a console
  ///     that cannot be reached included.
  auto offline() const noexcept -> bool;
  /// @brief Gives the HTTP status behind the failure.
  ///
  /// @return The cause's status; 0 when it has none.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief Gives the console's reason code.
  ///
  /// @return The code from a refusal, session request, registration or
  ///     reported reason; 0 otherwise.
  auto console_reason() const noexcept -> std::uint32_t;
  /// @brief Gives the bytes received before the failure.
  ///
  /// @return The count from the cause; 0 when it has none.
  auto received_bytes() const noexcept -> std::uint64_t;
  /// @brief Gives the bytes sent before the failure.
  ///
  /// @return The count for a control startup failure; 0 otherwise.
  auto sent_bytes() const noexcept -> std::uint64_t;
  /// @brief Gives the address where the player can fix the problem.
  ///
  /// @return The cloud launch's upgrade URL or the reported reason's; empty
  ///     otherwise.
  auto action_url() const -> std::string_view;
  /// @brief Gives the service's own message.
  ///
  /// @return The cloud launch's detail or the reported reason's; empty
  ///     otherwise.
  auto message() const -> std::string_view;
  /// @brief Gives the quit reason the failure implies.
  ///
  /// @return For a lost stream, the peer's disconnect or shutdown with its
  ///     text; stopped for a cancellation; otherwise the reason the error,
  ///     registration or stage implies, or none.
  auto quit() const -> net::protocol::remote_play::QuitDetail;
  /// @brief Describes the failure in a few words.
  ///
  /// @return The cause's description; "cancelled" for a cancellation the cause
  ///     does not explain; otherwise the stream error's text.
  auto describe() const noexcept -> std::string_view;
  /// @brief Writes the failure as a sentence for the player and the log.
  ///
  /// @return The venue, stage and description, with bytes, cloud detail,
  ///     console reason, HTTP status and reason code as present.
  auto sentence() const -> std::string;
  /// @brief Builds the public end reason.
  ///
  /// @param refused Why the cloud launch was refused, when it was.
  /// @return The reason with its cause, step, codes, cancellation, address and
  ///     message.
  auto end_reason(std::optional<LaunchRefused> refused = std::nullopt) const
      -> EndReason;
  /// @brief Names the public cause.
  ///
  /// @return The cause of a reported reason;
  ///     EndReason::Cause::pairing_rejected for a refused pairing;
  ///     EndReason::Cause::connection_lost when the peer fell silent;
  ///     otherwise the stream error's cause.
  auto end_cause() const noexcept -> EndReason::Cause;

 private:
  /// @brief Names the stage an active handshake state belongs to.
  ///
  /// @param state The state.
  /// @return The stage; Stage::unknown for a closed or failed handshake.
  static auto stage_of(
      net::protocol::remote_play::ActiveHandshakeState state) noexcept -> Stage;
  /// @brief Names the cloud stage a launch failed in.
  ///
  /// @param failure The launch's failure.
  /// @return The stage.
  static auto stage_of(const LaunchFailure& failure) noexcept -> Stage;
  /// @brief Names the stream error of a session request failure.
  ///
  /// @param failure The failure.
  /// @return The error.
  static auto result_of(
      const net::protocol::remote_play::RemoteSessionInitFailure&
          failure) noexcept -> Error;
  /// @brief Names the stream error of an internet control connection failure.
  ///
  /// @param failure The failure.
  /// @return The error.
  static auto result_of(
      const internet::remote_ctrl::ConnectFailure& failure) noexcept -> Error;
  /// @brief Describes a stream error.
  ///
  /// @param result The error.
  /// @param console_reason The console's reason code, which singles out a
  ///     Remote Play crash.
  /// @return A fixed sentence.
  static auto result_text(Error result, std::uint32_t console_reason) noexcept
      -> std::string_view;

  /// @brief Reports whether the failure implies no quit reason.
  ///
  /// @return `true` for a transport switch failure or a session request
  ///     handshake failure.
  auto quits_without_reason() const noexcept -> bool;
  /// @brief Reports whether the console rejected the pairing.
  ///
  /// @return `true` for a reported pairing rejection, or a refused session
  ///     request with a registration or PSN id reason.
  auto pairing_rejected() const noexcept -> bool;
  /// @brief Reports whether the cause still describes a cancelled failure.
  ///
  /// @return `true` for audio refusals, registration and launch failures, and
  ///     route failures that drained a late probe.
  auto explains_cancellation() const noexcept -> bool;
  /// @brief Writes the cloud launch's detail for the sentence.
  ///
  /// @return The refusal kind and check status, the title cause, or the
  ///     reservation reason and best round trip; empty without a launch.
  auto cloud_detail() const -> std::string;
  /// @brief Names the LegacyCloudStream title step that failed.
  ///
  /// @return The step; empty when no title step failed.
  auto title_stage() const noexcept -> std::optional<std::string_view>;

  /// @brief Describes no cause.
  ///
  /// @return Empty.
  static auto describe(std::monostate) noexcept -> std::string_view;
  /// @brief Describes an audio refusal.
  ///
  /// @param refused The refusal.
  /// @return Its description.
  static auto describe(const AudioRequestRefused& refused) noexcept
      -> std::string_view;
  /// @brief Describes an unresolved console.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(ConsoleUnresolved) noexcept -> std::string_view;
  /// @brief Describes a console refusal.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(ConsoleRefusal) noexcept -> std::string_view;
  /// @brief Describes a control startup failure.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(CtrlStartup) noexcept -> std::string_view;
  /// @brief Describes a control reader error.
  ///
  /// @param error The error.
  /// @return A fixed sentence.
  static auto describe(
      net::protocol::remote_play::CtrlReaderError error) noexcept
      -> std::string_view;
  /// @brief Describes an active stream failure.
  ///
  /// @param failure The failure.
  /// @return A fixed sentence for its cause.
  static auto describe(
      const net::protocol::remote_play::ActiveSessionFailure& failure) noexcept
      -> std::string_view;
  /// @brief Describes a Senkusha error.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(net::protocol::senkusha::Error) noexcept
      -> std::string_view;
  /// @brief Describes a Takion client error.
  ///
  /// @return That disconnecting or closing the transport failed.
  static auto describe(net::protocol::takion::ClientError) noexcept
      -> std::string_view;
  /// @brief Describes a platform error.
  ///
  /// @return That the route worker could not start or complete.
  static auto describe(platform::PlatformError) noexcept -> std::string_view;
  /// @brief Describes a PSN failure.
  ///
  /// @param failure The failure.
  /// @return Its description.
  static auto describe(const PsnFailure& failure) noexcept -> std::string_view;
  /// @brief Describes a wake failure.
  ///
  /// @param failure The failure.
  /// @return Its description.
  static auto describe(const WakeFailure& failure) noexcept -> std::string_view;
  /// @brief Describes a route failure.
  ///
  /// @param failure The failure.
  /// @return Its description.
  static auto describe(const RouteFailure& failure) noexcept
      -> std::string_view;
  /// @brief Describes an internet pairing failure.
  ///
  /// @param failure The failure.
  /// @return A fixed sentence for its kind.
  static auto describe(
      const internet::PsnRemoteRegistrationFailure& failure) noexcept
      -> std::string_view;
  /// @brief Describes a session request failure.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(
      const net::protocol::remote_play::RemoteSessionInitFailure&) noexcept
      -> std::string_view;
  /// @brief Describes an internet control connection failure.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(const internet::remote_ctrl::ConnectFailure&) noexcept
      -> std::string_view;
  /// @brief Describes a control send failure.
  ///
  /// @param error The error.
  /// @return A fixed sentence.
  static auto describe(internet::remote_ctrl::OutboundStepError error) noexcept
      -> std::string_view;
  /// @brief Describes a control read failure.
  ///
  /// @param read The read failure.
  /// @return A fixed sentence for its error.
  static auto describe(const RemoteCtrlRead& read) noexcept -> std::string_view;
  /// @brief Describes a transport switch failure.
  ///
  /// @param error The error.
  /// @return A fixed sentence.
  static auto describe(
      net::protocol::remote_play::RudpSwitchBarrierError error) noexcept
      -> std::string_view;
  /// @brief Describes a failed drain after the switch.
  ///
  /// @param drain The drain failure.
  /// @return Whether no poller could be made, or the drain failed.
  static auto describe(SwitchDrain drain) noexcept -> std::string_view;
  /// @brief Describes a datagram drain error.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(net::transport::rudp::RemoteDatagramDrainError) noexcept
      -> std::string_view;
  /// @brief Describes a cloud launch failure.
  ///
  /// @param failure The failure.
  /// @param cancelled Whether the link was cancelled.
  /// @return The launch's description, unless cancelled with an unmet
  ///     precondition; then that the cloud request was cancelled, or that it
  ///     failed.
  static auto describe(const LaunchFailure& failure, bool cancelled) noexcept
      -> std::string_view;
  /// @brief Describes a reported end reason.
  ///
  /// @return Empty; the stream error's text is used.
  static auto describe(const EndReason&) noexcept -> std::string_view;

  /// @brief The stage.
  Stage stage_;
  /// @brief The stream error.
  Error result_;
  /// @brief The detailed cause.
  Cause cause_;
  /// @brief The venue it happened on; empty until stamped.
  std::optional<Venue> route_{};
  /// @brief Whether it was marked cancelled.
  bool cancelled_{};
};

}  // namespace libchiaking::play
