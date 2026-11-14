#include "src/core/net/protocol/remote_play/stream/local_start.hpp"

#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/video_presets.hpp"

#include <memory>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto LocalStart::effective_launch(const LocalVideoSelection& selection) noexcept
    -> event::EffectiveLaunch {
  const auto codec = selection.codec == VideoCodec::h265 ||
                             selection.codec == VideoCodec::h265_hdr
                         ? selection.codec
                         : VideoCodec::h264;
  return {codec, selection.video.width, selection.video.height,
          selection.video.max_fps, selection.video.bitrate_kbps};
}

auto LocalStart::apply_server_type(LocalVideoSelection selection,
                                   std::optional<std::uint8_t> server_type,
                                   bool auto_downgrade) noexcept
    -> LocalVideoSelection {
  if (!server_type) return selection;
  if (*server_type == 0 && auto_downgrade &&
      selection.video.height == video_preset_1080p.height) {
    selection.codec = VideoCodec::h264;
    selection.video.width = video_preset_720p.width;
    selection.video.height = video_preset_720p.height;
    selection.video.bitrate_kbps = video_preset_720p.bitrate_kbps;
    selection.video.max_fps =
        selection.video.max_fps == video_fps_60 ? video_fps_60 : video_fps_30;
  }
  if ((*server_type == 0 || *server_type == 1) &&
      selection.codec != VideoCodec::h264)
    selection.codec = VideoCodec::h264;
  return selection;
}

auto LocalStart::select_active_control_profile(
    RegistrationTarget target, std::optional<bool> dualsense) noexcept
    -> std::optional<ActiveControlProfile> {
  const auto version = RpVersion::of(target);
  if (!version) return std::nullopt;
  const bool ps5 = version->ps5();
  ActiveControlProfile profile{
      dualsense.value_or(ps5) ? takion::ControllerConnectionType::dualsense
                              : takion::ControllerConnectionType::dualshock4,
      ps5 ? AudioAssemblyPolicy::console_by_mode
          : AudioAssemblyPolicy::console_units};
  profile.peer_silence_timeout_us = console_peer_silence_timeout_us;
  profile.periodic_heartbeat = !ps5;
  return profile;
}

auto LocalStart::start_active(EstablishedRoute route,
                              const LocalActiveInputs& inputs)
    -> Expected<std::unique_ptr<ActiveSession>, LocalActiveStartError> {
  if (!route.transport) return unexpected(LocalActiveStartError::invalid_route);
  auto control_profile =
      select_active_control_profile(inputs.console.target, inputs.dualsense);
  if (!control_profile) return unexpected(LocalActiveStartError::invalid_route);

  control_profile->hevc = inputs.codec != VideoCodec::h264;
  control_profile->enable_idr_on_fec_failure = inputs.enable_idr_on_fec_failure;
  control_profile->pad_count = inputs.pad_count;
  ActiveControlProfile::apply_session_tuning(*control_profile, inputs.tuning);
  control_profile->bang_session_key =
      inputs.session_id.synthesized() ? BangSessionKeyPolicy::report_mismatch
                                      : BangSessionKeyPolicy::required;
  if (inputs.tuning.negotiate_takion_version)
    control_profile->takion_offer =
        takion::takion_negotiation_offer(takion::SessionScope::console);

  const auto profile = LocalStreamPreparation::console_active_profile(
      inputs.console.target, inputs.tuning.takion_version_override);
  if (!profile || route.calibration.takion_version !=
                      route.calibration.wire_profile.version())
    return unexpected(LocalActiveStartError::invalid_route);
  route.calibration.takion_version = profile->version();
  route.calibration.wire_profile = *profile;

  const LocalStreamInputs prepare_inputs{
      inputs.crypto,
      inputs.random,
      inputs.console,
      inputs.codec,
      inputs.video,
      route.calibration,
      inputs.session_id,
      inputs.nonce,
      static_cast<std::uint8_t>(inputs.tuning.launch_audio_channels())};
  auto prepared = LocalStreamPreparation::prepare(prepare_inputs);
  if (!prepared) return unexpected(LocalActiveStartError::prepare_failed);

  auto active =
      ActiveSession::start(std::move(route), inputs.clock, inputs.threading,
                           inputs.random, inputs.crypto, inputs.takion_crypto,
                           std::move(prepared.value().handshake_key),
                           std::move(prepared.value().prepared_ecdh),
                           std::move(prepared.value().session_key),
                           std::move(prepared.value().encrypted_launch_spec),
                           inputs.config, *control_profile);
  if (!active) return unexpected(LocalActiveStartError::start_failed);
  return std::move(active.value());
}

LocalCalibratedPipeline::LocalCalibratedPipeline(
    std::unique_ptr<senkusha::Client> client, LocalCtrlReadyTicket ticket,
    LocalPipelineInputs inputs) noexcept
    : client_(std::move(client)),
      ticket_(std::move(ticket)),
      inputs_(std::move(inputs)),
      lifecycle_(inputs_.retained_route_fallback.has_value(),
                 static_cast<bool>(inputs_.fallback_route)) {}

auto LocalCalibratedPipeline::start_reference_fallback(
    LocalCtrlReadyTicket ticket, LocalPipelineInputs inputs)
    -> Expected<std::unique_ptr<LocalCalibratedPipeline>, LocalPipelineError> {
  if (!inputs.fallback_route)
    return unexpected(LocalPipelineError::senkusha_failed);
  auto pipeline = std::make_unique<LocalCalibratedPipeline>(
      nullptr, std::move(ticket), std::move(inputs));
  static_cast<void>(pipeline->lifecycle_.process(
      local_pipeline_lifecycle::CalibrationSkipped{}));
  if (pipeline->inputs_.trace)
    pipeline->inputs_.trace->decision(
        diagnostics::Trace::Decision::fresh_calibration_route, std::nullopt);
  auto route = pipeline->inputs_.fallback_route();
  if (!route) return pipeline->fail_opening(route.error());
  auto started = pipeline->start_active(std::move(route.value()));
  if (!started) return unexpected(started.error());
  return pipeline;
}

void LocalCalibratedPipeline::mark_route_readable(
    std::uint64_t ready_at_us) noexcept {
  if (state() == LocalPipelineState::calibrating && client_)
    client_->mark_readable(ready_at_us);
}

auto LocalCalibratedPipeline::fail_opening(LocalPipelineError error)
    -> Unexpected<LocalPipelineError> {
  static_cast<void>(
      lifecycle_.process(local_pipeline_lifecycle::OpeningFailed{}));
  ticket_.reset();
  client_.reset();
  return unexpected(error);
}

auto LocalCalibratedPipeline::finish_calibration()
    -> Expected<void, LocalPipelineError> {
  static_cast<void>(
      lifecycle_.process(local_pipeline_lifecycle::CalibrationSucceeded{}));
  if (inputs_.trace)
    inputs_.trace->record(diagnostics::Operation::calibration,
                          diagnostics::Outcome::succeeded);
  auto route = client_->take_established_route();
  if (!route) return fail_opening(LocalPipelineError::route_unavailable);
  client_.reset();
  return transition_to_active(std::move(*route));
}

auto LocalCalibratedPipeline::transition_to_active(EstablishedRoute route)
    -> Expected<void, LocalPipelineError> {
  if (inputs_.transition_route) {
    auto transitioned = diagnostics::observe(
        inputs_.trace, diagnostics::Operation::route_switch,
        [&] { return inputs_.transition_route(std::move(route)); });
    if (!transitioned) return fail_opening(transitioned.error());
    route = std::move(transitioned.value());
  }
  client_.reset();
  return start_active(std::move(route));
}

auto LocalCalibratedPipeline::start_active(EstablishedRoute route)
    -> Expected<void, LocalPipelineError> {
  const LocalActiveInputs active_inputs{
      inputs_.clock,         inputs_.threading,
      inputs_.random,        inputs_.crypto,
      inputs_.takion_crypto, inputs_.console,
      inputs_.codec,         inputs_.video,
      ticket_->session_id,   {ticket_->nonce.data(), ticket_->nonce.size()},
      inputs_.takion_config, inputs_.dualsense,
      inputs_.pad_count,     inputs_.enable_idr_on_fec_failure,
      inputs_.tuning,
  };
  auto active = diagnostics::observe(
      inputs_.trace, diagnostics::Operation::active_prepare, [&] {
        return LocalStart::start_active(std::move(route), active_inputs);
      });
  if (!active) return fail_opening(LocalPipelineError::active_start_failed);
  ticket_.reset();
  client_.reset();
  active_ = std::move(active.value());
  active_->set_diagnostics(inputs_.trace);
  static_cast<void>(
      lifecycle_.process(local_pipeline_lifecycle::ActiveStarted{}));
  return {};
}

auto LocalCalibratedPipeline::handle_calibration_failure()
    -> Expected<void, LocalPipelineError> {
  const auto error = client_->snapshot().error;
  if (inputs_.trace)
    inputs_.trace->record(diagnostics::Operation::calibration,
                          diagnostics::Outcome::failed,
                          error ? static_cast<unsigned>(*error) : 0);
  calibration_failure_ = error;
  auto retained = client_->take_failed_route();
  client_.reset();
  const bool route_retained = retained && retained->transport;
  static_cast<void>(
      lifecycle_.process(local_pipeline_lifecycle::CalibrationFailed{
          error == senkusha::Error::cancelled, route_retained}));
  const auto effect = lifecycle_.take_effect();
  if (effect &&
      std::holds_alternative<local_pipeline_lifecycle::OpenRetainedRoute>(
          *effect))
    return open_retained_route(std::move(*retained), error);
  if (route_retained) retained->transport.reset();
  if (effect &&
      std::holds_alternative<local_pipeline_lifecycle::OpenFreshRoute>(*effect))
    return open_fresh_route(error);
  ticket_.reset();
  return unexpected(LocalPipelineError::senkusha_failed);
}

auto LocalCalibratedPipeline::open_retained_route(
    RouteCandidate retained, std::optional<senkusha::Error> error)
    -> Expected<void, LocalPipelineError> {
  if (inputs_.trace)
    inputs_.trace->decision(
        diagnostics::Trace::Decision::retained_calibration_route, error);
  return enter_on_reference(std::move(retained));
}

auto LocalCalibratedPipeline::enter_on_reference(RouteCandidate route)
    -> Expected<void, LocalPipelineError> {
  if (!route.transport)
    return fail_opening(LocalPipelineError::route_unavailable);
  if (route.kind == RouteKind::remote_rudp &&
      !route.transport->set_dont_fragment(false))
    return fail_opening(LocalPipelineError::route_unavailable);
  return transition_to_active(EstablishedRoute{
      {std::move(route.transport), route.local_endpoint, route.remote_endpoint,
       route.kind, DontFragmentPolicy::disabled},
      *inputs_.retained_route_fallback});
}

auto LocalCalibratedPipeline::start_uncalibrated(RouteCandidate route,
                                                 LocalCtrlReadyTicket ticket,
                                                 LocalPipelineInputs inputs)
    -> Expected<std::unique_ptr<LocalCalibratedPipeline>, LocalPipelineError> {
  if (!inputs.retained_route_fallback)
    return unexpected(LocalPipelineError::senkusha_failed);
  auto pipeline = std::make_unique<LocalCalibratedPipeline>(
      nullptr, std::move(ticket), std::move(inputs));
  static_cast<void>(pipeline->lifecycle_.process(
      local_pipeline_lifecycle::CalibrationSkipped{}));
  if (pipeline->inputs_.trace)
    pipeline->inputs_.trace->decision(
        diagnostics::Trace::Decision::uncalibrated_route, std::nullopt);
  auto entered = pipeline->enter_on_reference(std::move(route));
  if (!entered) return unexpected(entered.error());
  return pipeline;
}

auto LocalCalibratedPipeline::open_fresh_route(
    std::optional<senkusha::Error> error)
    -> Expected<void, LocalPipelineError> {
  if (inputs_.trace)
    inputs_.trace->decision(
        diagnostics::Trace::Decision::fresh_calibration_route, error);
  auto route = inputs_.fallback_route();
  if (!route) return fail_opening(LocalPipelineError::senkusha_failed);
  return start_active(std::move(route.value()));
}

auto LocalCalibratedPipeline::pump_once()
    -> Expected<void, LocalPipelineError> {
  switch (state()) {
    case LocalPipelineState::calibrating:
      return pump_calibration();
    case LocalPipelineState::active:
      return pump_active();
    case LocalPipelineState::stopped:
    case LocalPipelineState::failed:
      break;
  }
  return unexpected(LocalPipelineError::invalid_state);
}

auto LocalCalibratedPipeline::pump_active()
    -> Expected<void, LocalPipelineError> {
  if (!active_ || !active_->pump_once()) {
    if (active_) active_failure_ = active_->failure();
    static_cast<void>(
        lifecycle_.process(local_pipeline_lifecycle::ActiveFailed{}));
    if (!active_failure_ ||
        active_failure_->cause != ActiveSessionError::peer_disconnected)
      active_.reset();
    return unexpected(LocalPipelineError::active_pump_failed);
  }
  if (active_->state() == ActiveHandshakeState::closed) {
    static_cast<void>(
        lifecycle_.process(local_pipeline_lifecycle::ActiveClosed{}));
    active_.reset();
  }
  return {};
}

auto LocalCalibratedPipeline::pump_calibration()
    -> Expected<void, LocalPipelineError> {
  if (!client_) {
    static_cast<void>(
        lifecycle_.process(local_pipeline_lifecycle::CalibrationLost{}));
    ticket_.reset();
    return unexpected(LocalPipelineError::invalid_state);
  }
  const auto calibration_state = client_->snapshot().state;
  if (inputs_.trace && logged_calibration_ != calibration_state) {
    logged_calibration_ = calibration_state;
    inputs_.trace->record_state(diagnostics::Operation::calibration,
                                calibration_state);
  }
  if (client_->snapshot().state != senkusha::State::completed &&
      !client_->pump_once())
    return handle_calibration_failure();
  if (client_->snapshot().state == senkusha::State::failed)
    return handle_calibration_failure();
  if (client_->snapshot().state == senkusha::State::completed)
    return finish_calibration();
  return {};
}

auto LocalCalibratedPipeline::request_stop()
    -> Expected<void, LocalPipelineError> {
  static_cast<void>(
      lifecycle_.process(local_pipeline_lifecycle::StopRequested{}));
  const auto effect = lifecycle_.take_effect();
  if (!effect) return {};
  if (std::holds_alternative<local_pipeline_lifecycle::CancelCalibration>(
          *effect))
    return cancel_calibration();
  if (std::holds_alternative<local_pipeline_lifecycle::StopActive>(*effect))
    return stop_active();
  ticket_.reset();
  client_.reset();
  active_.reset();
  return {};
}

auto LocalCalibratedPipeline::cancel_calibration()
    -> Expected<void, LocalPipelineError> {
  ticket_.reset();
  if (client_ && !client_->cancel()) {
    static_cast<void>(
        lifecycle_.process(local_pipeline_lifecycle::ReleaseFailed{}));
    client_.reset();
    return unexpected(LocalPipelineError::stop_failed);
  }
  client_.reset();
  static_cast<void>(lifecycle_.process(local_pipeline_lifecycle::Released{}));
  return {};
}

auto LocalCalibratedPipeline::stop_active()
    -> Expected<void, LocalPipelineError> {
  if (!active_) {
    static_cast<void>(lifecycle_.process(local_pipeline_lifecycle::Released{}));
    return {};
  }
  const auto stopped = active_->request_stop();
  stop_failure_ = active_->stop_failure();
  active_.reset();
  if (!stopped) {
    static_cast<void>(
        lifecycle_.process(local_pipeline_lifecycle::ReleaseFailed{}));
    return unexpected(LocalPipelineError::stop_failed);
  }
  static_cast<void>(lifecycle_.process(local_pipeline_lifecycle::Released{}));
  return {};
}

auto LocalStart::active_route_config(const DirectRouteConfig& base,
                                     const EstablishedRoute& calibrated,
                                     std::uint16_t active_port) noexcept
    -> DirectRouteConfig {
  auto config = base;
  config.remote.address = calibrated.remote_endpoint.address;
  config.remote.port = active_port;
  if (calibrated.dont_fragment == DontFragmentPolicy::disabled)
    config.dont_fragment = DontFragmentPolicy::disabled;
  return config;
}

auto LocalStart::start_calibration(RouteCandidate route,
                                   LocalCtrlReadyTicket ticket,
                                   LocalCalibrationInputs inputs)
    -> Expected<std::unique_ptr<LocalCalibratedPipeline>,
                LocalCalibrationStartError> {
  if (ticket.attempt_id == 0)
    return unexpected(LocalCalibrationStartError::invalid_ticket);
  if (!route.transport)
    return unexpected(LocalCalibrationStartError::invalid_route);

  inputs.senkusha_config.profile.handoff =
      senkusha::TransportHandoffPolicy::return_route;
  if (inputs.pipeline.retained_route_fallback)
    inputs.senkusha_config.failure_handoff =
        senkusha::TransportHandoffPolicy::return_route;
  auto client = std::make_unique<senkusha::Client>(
      std::move(route), inputs.pipeline.clock, inputs.pipeline.threading,
      inputs.pipeline.random, inputs.senkusha_events,
      std::move(inputs.senkusha_config));
  if (!client->start() &&
      (!inputs.pipeline.retained_route_fallback ||
       client->snapshot().state != senkusha::State::failed)) {
    const bool cancelled =
        client->snapshot().error == senkusha::Error::cancelled;
    client.reset();
    if (cancelled || !inputs.pipeline.fallback_route)
      return unexpected(LocalCalibrationStartError::senkusha_start_failed);
    auto fallback = LocalCalibratedPipeline::start_reference_fallback(
        std::move(ticket), inputs.pipeline);
    if (!fallback)
      return unexpected(LocalCalibrationStartError::senkusha_start_failed);
    return std::move(fallback.value());
  }

  return std::make_unique<LocalCalibratedPipeline>(
      std::move(client), std::move(ticket), inputs.pipeline);
}

auto LocalStart::start_calibration(platform::NetworkProvider& network,
                                   const DirectRouteConfig& route_config,
                                   LocalCtrlReadyTicket ticket,
                                   LocalCalibrationInputs inputs)
    -> Expected<std::unique_ptr<LocalCalibratedPipeline>,
                LocalCalibrationStartError> {
  if (ticket.attempt_id == 0)
    return unexpected(LocalCalibrationStartError::invalid_ticket);
  auto route = DirectRoute::connect(network, route_config);
  if (!route) return unexpected(LocalCalibrationStartError::route_unavailable);
  return start_calibration(std::move(route.value()), std::move(ticket),
                           std::move(inputs));
}

}  // namespace libchiaking::net::protocol::remote_play
