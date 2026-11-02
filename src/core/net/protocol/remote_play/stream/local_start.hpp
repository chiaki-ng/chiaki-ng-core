/// @file
/// @brief Declares LocalStart and LocalCalibratedPipeline, which take a home
/// stream from a running control channel through Senkusha calibration to the
/// active Takion session.
///
/// Once the control channel has the session id, the start opens a UDP route
/// to the console, calibrates it with Senkusha (MTU and round trip), and then
/// starts the active session on the stream port with the BIG contents
/// LocalStreamPreparation makes. When calibration fails the pipeline may fall
/// back to the route Senkusha handed back, or to a fresh route with the
/// reference calibration.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/direct_route.hpp"
#include "src/core/net/protocol/remote_play/stream/launch_spec.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start_lifecycle.hpp"
#include "src/core/net/protocol/senkusha/senkusha.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why the active session could not start.
enum class LocalActiveStartError : std::uint8_t {
  /// @brief The route has no transport, or the target or Takion profile does
  /// not fit it.
  invalid_route,
  /// @brief The BIG contents could not be prepared.
  prepare_failed,
  /// @brief The session failed to start.
  start_failed,
};

/// @brief What starting the active session needs. References must outlive
/// the call.
struct LocalActiveInputs {
  /// @brief The monotonic clock.
  platform::ClockProvider& clock;
  /// @brief Makes the Takion client's mutex.
  platform::ThreadingProvider& threading;
  /// @brief Randomness.
  platform::SecureRandomProvider& random;
  /// @brief The platform crypto provider.
  platform::CryptoProvider& crypto;
  /// @brief The Takion crypto provider.
  takion::CryptoProvider& takion_crypto;
  /// @brief The registered console.
  const RegisteredConsole& console;
  /// @brief The codec to ask for.
  VideoCodec codec;
  /// @brief The picture to ask for.
  LaunchSpecVideo video;
  /// @brief The session id.
  const SessionId& session_id;
  /// @brief The 16-byte session nonce.
  std::span<const std::byte> nonce;
  /// @brief The Takion client configuration.
  takion::ClientConfig config{};
  /// @brief Whether to present a DualSense; nothing for the console's own
  /// (DualSense on PS5).
  std::optional<bool> dualsense{};
  /// @brief How many controllers take part.
  std::uint8_t pad_count{1};
  /// @brief Whether a frame lost to FEC asks the console for a keyframe.
  bool enable_idr_on_fec_failure{};
  /// @brief The caller's tuning.
  SessionTuning tuning{};
};

/// @brief A codec and picture chosen for a home stream.
struct LocalVideoSelection {
  /// @brief The codec.
  VideoCodec codec;
  /// @brief The picture.
  LaunchSpecVideo video;
};

/// @brief Why the pipeline failed.
enum class LocalPipelineError : std::uint8_t {
  /// @brief The call does not fit the pipeline's phase.
  invalid_state,
  /// @brief Senkusha failed and no fallback applied.
  senkusha_failed,
  /// @brief No route for the active session could be opened.
  route_unavailable,
  /// @brief The active session could not start.
  active_start_failed,
  /// @brief The active session failed.
  active_pump_failed,
  /// @brief Stopping failed.
  stop_failed,
};

/// @brief What the pipeline needs. References must outlive the pipeline.
struct LocalPipelineInputs {
  /// @brief The monotonic clock.
  platform::ClockProvider& clock;
  /// @brief Makes the Takion client's mutex.
  platform::ThreadingProvider& threading;
  /// @brief Randomness.
  platform::SecureRandomProvider& random;
  /// @brief The platform crypto provider.
  platform::CryptoProvider& crypto;
  /// @brief The Takion crypto provider.
  takion::CryptoProvider& takion_crypto;
  /// @brief The registered console.
  const RegisteredConsole& console;
  /// @brief The codec to ask for.
  VideoCodec codec;
  /// @brief The picture to ask for.
  LaunchSpecVideo video;
  /// @brief The Takion client configuration.
  takion::ClientConfig takion_config{};
  /// @brief Turns the calibrated route into the stream's route (opening the
  /// stream port, or switching a PSN route); none uses it as is.
  std::function<Expected<EstablishedRoute, LocalPipelineError>(
      EstablishedRoute)>
      transition_route{};
  /// @brief Opens a fresh route when calibration cannot run or fails; none
  /// disables that fallback.
  std::function<Expected<EstablishedRoute, LocalPipelineError>()>
      fallback_route{};
  /// @brief The calibration to use on the route Senkusha hands back after a
  /// failure; none disables that fallback.
  std::optional<senkusha::Result> retained_route_fallback{};
  /// @brief Where steps are traced, or null.
  const diagnostics::Trace* trace{};
  /// @brief Whether to present a DualSense; nothing for the console's own.
  std::optional<bool> dualsense{};
  /// @brief How many controllers take part.
  std::uint8_t pad_count{1};
  /// @brief Whether a frame lost to FEC asks for a keyframe.
  bool enable_idr_on_fec_failure{};
  /// @brief The caller's tuning.
  SessionTuning tuning{};
};

/// @brief A home stream from calibration to the end of its active session.
///
/// The owner pumps it on the stream's worker thread; not thread-safe.
class LocalCalibratedPipeline {
 public:
  /// @brief Makes a pipeline that is calibrating.
  ///
  /// @param client The started Senkusha client, or null for a pipeline that
  ///     skips calibration.
  /// @param ticket The control channel's session id and nonce.
  /// @param inputs What the pipeline needs.
  LocalCalibratedPipeline(std::unique_ptr<senkusha::Client> client,
                          LocalCtrlReadyTicket ticket,
                          LocalPipelineInputs inputs) noexcept;
  LocalCalibratedPipeline(const LocalCalibratedPipeline&) = delete;
  LocalCalibratedPipeline& operator=(const LocalCalibratedPipeline&) = delete;

  /// @brief Makes a pipeline that skips calibration and starts the active
  /// session on a fresh route with the reference calibration.
  ///
  /// @param ticket The control channel's session id and nonce.
  /// @param inputs What the pipeline needs; must have a fallback route.
  /// @return The pipeline, already active.
  /// @retval LocalPipelineError::senkusha_failed No fallback route was given.
  /// @retval LocalPipelineError Why the route or session failed.
  [[nodiscard]] static auto start_reference_fallback(
      LocalCtrlReadyTicket ticket, LocalPipelineInputs inputs)
      -> Expected<std::unique_ptr<LocalCalibratedPipeline>, LocalPipelineError>;

  /// @brief Makes a pipeline that skips calibration and runs the route the
  /// caller already holds with the retained-route calibration, applying the
  /// route transition first.
  ///
  /// @param route The route, such as a hole-punched internet data route.
  /// @param ticket The control channel's session id and nonce.
  /// @param inputs What the pipeline needs; must have a retained-route
  ///     calibration.
  /// @return The pipeline, already active.
  /// @retval LocalPipelineError::senkusha_failed No retained-route
  ///     calibration was given.
  /// @retval LocalPipelineError::route_unavailable The route has no
  ///     transport, or its transition failed.
  /// @retval LocalPipelineError Why the session failed to start.
  [[nodiscard]] static auto start_uncalibrated(RouteCandidate route,
                                               LocalCtrlReadyTicket ticket,
                                               LocalPipelineInputs inputs)
      -> Expected<std::unique_ptr<LocalCalibratedPipeline>, LocalPipelineError>;

  /// @brief Advances calibration or the active session by one step.
  ///
  /// Never blocks for long; the session's own waits are bounded.
  ///
  /// @return Nothing while it runs.
  /// @retval LocalPipelineError::invalid_state Stopped or failed.
  /// @retval LocalPipelineError Why calibration, the route or the session
  ///     failed.
  [[nodiscard]] auto pump_once() -> Expected<void, LocalPipelineError>;
  /// @brief Tells Senkusha its route has data.
  ///
  /// @param ready_at_us Monotonic microseconds when it became readable.
  void mark_route_readable(std::uint64_t ready_at_us) noexcept;
  /// @brief Why stopping the active session failed, if it did.
  ///
  /// @return The Takion error, or nothing.
  [[nodiscard]] auto stop_failure() const noexcept
      -> std::optional<takion::ClientError> {
    return stop_failure_;
  }
  /// @brief Stops whatever runs: cancels Senkusha or stops the session.
  ///
  /// @return Nothing on success.
  /// @retval LocalPipelineError::stop_failed Releasing failed.
  [[nodiscard]] auto request_stop() -> Expected<void, LocalPipelineError>;
  /// @brief The phase.
  ///
  /// @return The state.
  auto state() const noexcept -> LocalPipelineState {
    return lifecycle_.state();
  }
  /// @brief The active session.
  ///
  /// @return The session, or null unless active.
  auto active() noexcept -> ActiveSession* {
    return state() == LocalPipelineState::active ? active_.get() : nullptr;
  }
  /// @brief The active session.
  ///
  /// @return The session, or null unless active.
  auto active() const noexcept -> const ActiveSession* {
    return state() == LocalPipelineState::active ? active_.get() : nullptr;
  }
  /// @brief The session whose media may still be drained, even after it
  /// failed.
  ///
  /// @return The session, or null.
  auto media_source() noexcept -> ActiveSession* { return active_.get(); }
  /// @brief The Takion version the session settled on.
  ///
  /// @return The version, or nothing before it is committed.
  auto committed_takion_version() const noexcept
      -> std::optional<std::uint32_t> {
    return active_ ? active_->committed_takion_version() : std::nullopt;
  }
  /// @brief Why the active session failed.
  ///
  /// @return The failure, or nothing.
  [[nodiscard]] auto active_failure() const noexcept
      -> const std::optional<ActiveSessionFailure>& {
    return active_failure_;
  }
  /// @brief Why calibration failed.
  ///
  /// @return The Senkusha error, or nothing.
  [[nodiscard]] auto calibration_failure() const noexcept
      -> const std::optional<senkusha::Error>& {
    return calibration_failure_;
  }

 private:
  /// @brief Advances Senkusha.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto pump_calibration() -> Expected<void, LocalPipelineError>;
  /// @brief Advances the active session.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto pump_active() -> Expected<void, LocalPipelineError>;
  /// @brief Takes the calibrated route and moves to the active session.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto finish_calibration() -> Expected<void, LocalPipelineError>;
  /// @brief Prepares and starts the active session on a route.
  ///
  /// @param route The route.
  /// @return Nothing, or LocalPipelineError::active_start_failed.
  [[nodiscard]] auto start_active(EstablishedRoute route)
      -> Expected<void, LocalPipelineError>;
  /// @brief Applies the route transition, then starts the active session.
  ///
  /// @param route The calibrated route.
  /// @return As pump_once().
  [[nodiscard]] auto transition_to_active(EstablishedRoute route)
      -> Expected<void, LocalPipelineError>;
  /// @brief Chooses the retained or fresh route fallback after Senkusha
  /// failed.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto handle_calibration_failure()
      -> Expected<void, LocalPipelineError>;
  /// @brief Runs on the route Senkusha handed back, with don't-fragment off.
  ///
  /// @param retained The route.
  /// @param error Why calibration failed.
  /// @return As pump_once().
  [[nodiscard]] auto open_retained_route(RouteCandidate retained,
                                         std::optional<senkusha::Error> error)
      -> Expected<void, LocalPipelineError>;
  /// @brief Runs a route with don't-fragment off and the retained-route
  /// calibration, applying the route transition first.
  ///
  /// @param route The route.
  /// @return As pump_once().
  [[nodiscard]] auto enter_on_reference(RouteCandidate route)
      -> Expected<void, LocalPipelineError>;
  /// @brief Opens a fresh route with the reference calibration.
  ///
  /// @param error Why calibration failed.
  /// @return As pump_once().
  [[nodiscard]] auto open_fresh_route(std::optional<senkusha::Error> error)
      -> Expected<void, LocalPipelineError>;
  /// @brief Fails while opening a route, releasing the ticket and Senkusha.
  ///
  /// @param error Why.
  /// @return The error.
  [[nodiscard]] auto fail_opening(LocalPipelineError error)
      -> Unexpected<LocalPipelineError>;
  /// @brief Cancels Senkusha.
  ///
  /// @return Nothing, or LocalPipelineError::stop_failed.
  [[nodiscard]] auto cancel_calibration() -> Expected<void, LocalPipelineError>;
  /// @brief Stops the active session.
  ///
  /// @return Nothing, or LocalPipelineError::stop_failed.
  [[nodiscard]] auto stop_active() -> Expected<void, LocalPipelineError>;

  /// @brief The Senkusha state last traced.
  std::optional<senkusha::State> logged_calibration_;
  /// @brief The Senkusha client while calibrating.
  std::unique_ptr<senkusha::Client> client_;
  /// @brief The session id and nonce until the session starts.
  std::optional<LocalCtrlReadyTicket> ticket_;
  /// @brief What the pipeline needs.
  LocalPipelineInputs inputs_;
  /// @brief The active session.
  std::unique_ptr<ActiveSession> active_;
  /// @brief The pipeline's state machine.
  local_pipeline_lifecycle::Lifecycle lifecycle_;
  /// @brief Why the session failed.
  std::optional<ActiveSessionFailure> active_failure_;
  /// @brief Why stopping failed.
  std::optional<takion::ClientError> stop_failure_;
  /// @brief Why calibration failed.
  std::optional<senkusha::Error> calibration_failure_;
};

/// @brief Why calibration could not start.
enum class LocalCalibrationStartError : std::uint8_t {
  /// @brief The ticket's attempt is 0.
  invalid_ticket,
  /// @brief The route has no transport.
  invalid_route,
  /// @brief The route could not be opened.
  route_unavailable,
  /// @brief Senkusha failed to start and no fallback applied.
  senkusha_start_failed,
};

/// @brief What starting calibration needs.
struct LocalCalibrationInputs {
  /// @brief What the pipeline needs.
  LocalPipelineInputs pipeline;
  /// @brief Where Senkusha reports.
  senkusha::EventSink& senkusha_events;
  /// @brief The Senkusha configuration.
  senkusha::ClientConfig senkusha_config{};
};

/// @brief The steps of starting a home stream after the control channel.
///
/// Stateless; every member is static.
class LocalStart {
 public:
  /// @brief What the start asked for, as reported to the caller.
  ///
  /// @param selection The codec and picture.
  /// @return The launch; codecs other than H.265 report H.264.
  static auto effective_launch(const LocalVideoSelection& selection) noexcept
      -> event::EffectiveLaunch;

  /// @brief Adjusts the codec and picture to the console's server type.
  ///
  /// A base PS4 (type 0) asked for 1080p is lowered to 720p at 10000 kbit/s
  /// when `auto_downgrade` is set; any PS4 (types 0 and 1) gets H.264.
  ///
  /// @param selection The caller's choice.
  /// @param server_type The console's `RP-Server-Type`, if it sent one.
  /// @param auto_downgrade Whether 1080p may be lowered.
  /// @return The adjusted choice.
  static auto apply_server_type(LocalVideoSelection selection,
                                std::optional<std::uint8_t> server_type,
                                bool auto_downgrade) noexcept
      -> LocalVideoSelection;

  /// @brief The control profile of an active session for a console variant.
  ///
  /// @param target The variant.
  /// @param dualsense Whether to present a DualSense; nothing for PS5 yes, PS4
  ///     no.
  /// @return The profile (controller type and audio assembly), or nothing for
  ///     an unknown variant.
  [[nodiscard]] static auto select_active_control_profile(
      RegistrationTarget target,
      std::optional<bool> dualsense = std::nullopt) noexcept
      -> std::optional<ActiveControlProfile>;

  /// @brief Prepares the BIG contents and starts the active session.
  ///
  /// The Takion version is the console's own (or the tuning's override), and a
  /// synthesised session id lets a mismatched BANG session key through.
  ///
  /// @param route The calibrated route; its transport is taken over.
  /// @param inputs What the session needs.
  /// @return The started session.
  /// @retval LocalActiveStartError Why it could not start.
  [[nodiscard]] static auto start_active(EstablishedRoute route,
                                         const LocalActiveInputs& inputs)
      -> Expected<std::unique_ptr<ActiveSession>, LocalActiveStartError>;

  /// @brief The route of the active session: the calibrated route's console
  /// address on the stream port, without don't-fragment if calibration ran
  /// without it.
  ///
  /// @param base The route settings to start from.
  /// @param calibrated The calibrated route.
  /// @param active_port The stream port.
  /// @return The settings.
  static auto active_route_config(const DirectRouteConfig& base,
                                  const EstablishedRoute& calibrated,
                                  std::uint16_t active_port) noexcept
      -> DirectRouteConfig;

  /// @brief Starts Senkusha on an open route.
  ///
  /// When Senkusha cannot start, a fresh-route fallback is tried.
  ///
  /// @param route The route; taken over.
  /// @param ticket The control channel's session id and nonce.
  /// @param inputs What calibration and the pipeline need.
  /// @return The pipeline.
  /// @retval LocalCalibrationStartError Why it could not start.
  [[nodiscard]] static auto start_calibration(RouteCandidate route,
                                              LocalCtrlReadyTicket ticket,
                                              LocalCalibrationInputs inputs)
      -> Expected<std::unique_ptr<LocalCalibratedPipeline>,
                  LocalCalibrationStartError>;

  /// @brief Opens a direct route and starts Senkusha on it.
  ///
  /// @param network The network provider.
  /// @param route_config Where the route goes.
  /// @param ticket The control channel's session id and nonce.
  /// @param inputs What calibration and the pipeline need.
  /// @return The pipeline.
  /// @retval LocalCalibrationStartError Why it could not start.
  [[nodiscard]] static auto start_calibration(
      platform::NetworkProvider& network, const DirectRouteConfig& route_config,
      LocalCtrlReadyTicket ticket, LocalCalibrationInputs inputs)
      -> Expected<std::unique_ptr<LocalCalibratedPipeline>,
                  LocalCalibrationStartError>;
};

}  // namespace libchiaking::net::protocol::remote_play
