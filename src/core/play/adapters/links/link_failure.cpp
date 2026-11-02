#include "src/core/play/adapters/links/link_failure.hpp"

#include "src/core/net/protocol/remote_play/ctrl_exchange.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/psn/graphql/check.hpp"

#include <magic_enum/magic_enum.hpp>

#include <format>
#include <type_traits>
#include <utility>

namespace libchiaking::play {

namespace rp = net::protocol::remote_play;

auto LinkFailure::AudioRequestRefused::for_console(
    rp::RegistrationTarget target, std::uint64_t channels) noexcept
    -> std::optional<AudioRequestRefused> {
  return checked(target == rp::RegistrationTarget::ps5_1
                     ? rp::AudioRequestTarget::ps5
                     : rp::AudioRequestTarget::ps4,
                 channels);
}

auto LinkFailure::AudioRequestRefused::for_cloud(
    cloud::CloudService service, std::uint64_t channels) noexcept
    -> std::optional<AudioRequestRefused> {
  return checked(service == cloud::CloudService::legacy_cloud_stream
                     ? rp::AudioRequestTarget::legacy_cloud_stream
                     : rp::AudioRequestTarget::cloud_stream,
                 channels);
}

auto LinkFailure::AudioRequestRefused::checked(rp::AudioRequestTarget target,
                                               std::uint64_t channels) noexcept
    -> std::optional<AudioRequestRefused> {
  if (rp::requested_audio_channels_supported(target, channels))
    return std::nullopt;
  return AudioRequestRefused{channels};
}

auto LinkFailure::AudioRequestRefused::describe() const noexcept
    -> std::string_view {
  return rp::valid_requested_audio_channels(channels_)
             ? "surround audio is not supported for this target"
             : "the requested audio channel count is not supported";
}

LinkFailure::LinkFailure(Stage stage, Error result, Cause cause)
    : stage_(stage), result_(result), cause_(std::move(cause)) {}

auto LinkFailure::lost(rp::ActiveSessionFailure failure) -> LinkFailure {
  const auto stage = stage_of(failure.stage);
  return LinkFailure{stage, Error::pipeline_failed, std::move(failure)};
}

auto LinkFailure::refused(const rp::RemoteSessionInitFailure& failure)
    -> LinkFailure {
  LinkFailure refused{Stage::session_init, result_of(failure), failure};
  refused.cancelled_ = failure.kind == rp::RemoteSessionInitError::stopped;
  return refused;
}

auto LinkFailure::refused(const internet::remote_ctrl::ConnectFailure& failure)
    -> LinkFailure {
  LinkFailure refused{Stage::ctrl_startup, result_of(failure), failure};
  using S = internet::remote_ctrl::StartupError;
  refused.cancelled_ =
      failure.kind == internet::remote_ctrl::ConnectError::stopped;
  if (failure.startup && failure.startup->kind == S::exchange_failed)
    refused.cancelled_ =
        static_cast<rp::RudpCtrlHttpExchangeError>(failure.startup->cause) ==
        rp::RudpCtrlHttpExchangeError::stopped;
  return refused;
}

auto LinkFailure::unpaired(
    const internet::PsnRemoteRegistrationFailure& failure) -> LinkFailure {
  LinkFailure unpaired{Stage::registration, Error::route_failed, failure};
  unpaired.cancelled_ =
      failure.kind == internet::PsnRemoteRegistrationError::stopped;
  return unpaired;
}

auto LinkFailure::unlaunched(LaunchFailure failure) -> LinkFailure {
  const auto stage = stage_of(failure);
  return LinkFailure{stage, Error::route_failed, std::move(failure)};
}

auto LinkFailure::reported(const EndReason& reason) -> LinkFailure {
  LinkFailure reported{LinkStages::stage(reason.step()),
                       LinkEvidence::error(reason.cause()), reason};
  reported.cancelled_ = reason.was_left();
  return reported;
}

auto LinkFailure::abandoned(Stage stage, Error result) -> LinkFailure {
  LinkFailure abandoned{stage, result};
  abandoned.cancelled_ = true;
  return abandoned;
}

auto LinkFailure::signalling_lost(Stage stage,
                                  psn::remote_play::PushError error)
    -> LinkFailure {
  const auto result = error.acknowledgement && stage == Stage::shutdown
                          ? Error::shutdown_failed
                          : Error::route_failed;
  return LinkFailure{stage, result, PsnFailure{std::move(error)}};
}

auto LinkFailure::shut_down(
    std::optional<net::protocol::takion::ClientError> error) -> LinkFailure {
  if (error)
    return LinkFailure{Stage::shutdown, Error::shutdown_failed, *error};
  return LinkFailure{Stage::shutdown, Error::shutdown_failed};
}

auto LinkFailure::shuts_down(const rp::ActiveSessionFailure& failure) noexcept
    -> bool {
  return failure.cause == rp::ActiveSessionError::peer_disconnected &&
         failure.peer_reason && *failure.peer_reason == remote_shutdown_reason;
}

auto LinkFailure::peer_quit(const rp::ActiveSessionFailure& failure,
                            bool trial_warned) noexcept -> rp::QuitReason {
  using Q = rp::QuitReason;
  if (failure.cause != rp::ActiveSessionError::peer_disconnected)
    return Q::stream_connection_unknown;
  if (shuts_down(failure)) return Q::stream_connection_remote_shutdown;
  if (failure.peer_reason && *failure.peer_reason == time_limit_reason)
    return Q::stream_connection_time_limit_reached;
  if (trial_warned && failure.peer_reason &&
      *failure.peer_reason == server_shutdown_reason)
    return Q::stream_connection_trial_ended;
  return Q::stream_connection_remote_disconnected;
}

void LinkFailure::log_disconnect(
    platform::DiagnosticsProvider* diagnostics,
    const rp::ActiveSessionFailure& failure) noexcept {
  if (!diagnostics ||
      failure.cause != rp::ActiveSessionError::peer_disconnected)
    return;
  try {
    diagnostics->write(
        {platform::LogSeverity::info,
         std::format(
             "Remote disconnected from StreamConnection with reason \"{}\"",
             failure.peer_reason.value_or(std::string{}))});
  } catch (...) {
  }
  diagnostics->write({platform::LogSeverity::info,
                      "StreamConnection closing after Remote disconnected"});
  diagnostics->write({platform::LogSeverity::error,
                      "Remote disconnected from StreamConnection"});
}

auto LinkFailure::cancelled() const noexcept -> bool {
  if (cancelled_) return true;
  if (const auto* psn = as<PsnFailure>()) return psn->cancelled();
  if (const auto* wake = as<WakeFailure>()) return wake->cancelled();
  if (const auto* route = as<RouteFailure>()) return route->cancelled();
  if (const auto* barrier = as<rp::RudpSwitchBarrierError>())
    return *barrier == rp::RudpSwitchBarrierError::stopped;
  return false;
}

auto LinkFailure::offline() const noexcept -> bool {
  const auto* psn = as<PsnFailure>();
  return psn && psn->offline();
}

auto LinkFailure::http_status() const noexcept -> std::uint16_t {
  return std::visit(
      [](const auto& cause) -> std::uint16_t {
        using T = std::decay_t<decltype(cause)>;
        if constexpr (std::is_same_v<T, ConsoleRefusal> ||
                      std::is_same_v<T,
                                     internet::PsnRemoteRegistrationFailure> ||
                      std::is_same_v<T, rp::RemoteSessionInitFailure>)
          return cause.http_status;
        else if constexpr (std::is_same_v<T, PsnFailure> ||
                           std::is_same_v<T, WakeFailure> ||
                           std::is_same_v<T, RouteFailure> ||
                           std::is_same_v<T, EndReason>)
          return cause.http_status();
        else if constexpr (std::is_same_v<
                               T, internet::remote_ctrl::ConnectFailure>)
          return cause.startup ? cause.startup->http_status : std::uint16_t{};
        else if constexpr (std::is_same_v<T, LaunchFailure>)
          return cause.http_status();
        else
          return {};
      },
      cause_);
}

auto LinkFailure::console_reason() const noexcept -> std::uint32_t {
  if (const auto* refusal = as<ConsoleRefusal>()) return refusal->reason;
  if (const auto* init = as<rp::RemoteSessionInitFailure>())
    return init->protocol_reason;
  if (const auto* registration = as<internet::PsnRemoteRegistrationFailure>())
    return registration->protocol_reason;
  if (const auto* reason = as<EndReason>()) return reason->console_reason();
  return 0;
}

auto LinkFailure::received_bytes() const noexcept -> std::uint64_t {
  if (const auto* startup = as<CtrlStartup>()) return startup->received;
  if (const auto* init = as<rp::RemoteSessionInitFailure>())
    return init->received_bytes;
  if (const auto* registration = as<internet::PsnRemoteRegistrationFailure>())
    return registration->received_bytes;
  if (const auto* ctrl = as<internet::remote_ctrl::ConnectFailure>())
    return ctrl->startup ? ctrl->startup->received_bytes : 0;
  if (const auto* read = as<RemoteCtrlRead>()) return read->packet_bytes;
  return 0;
}

auto LinkFailure::sent_bytes() const noexcept -> std::uint64_t {
  const auto* startup = as<CtrlStartup>();
  return startup ? startup->sent : 0;
}

auto LinkFailure::action_url() const -> std::string_view {
  if (const auto* launch = as<LaunchFailure>()) return launch->upgrade_url();
  if (const auto* reason = as<EndReason>()) return reason->action_url();
  return {};
}

auto LinkFailure::message() const -> std::string_view {
  if (const auto* launch = as<LaunchFailure>()) return launch->detail();
  if (const auto* reason = as<EndReason>()) return reason->detail();
  return {};
}

auto LinkFailure::quit() const -> rp::QuitDetail {
  using Q = rp::QuitReason;
  if (const auto* lost = as<rp::ActiveSessionFailure>()) {
    if (lost->cause != rp::ActiveSessionError::peer_disconnected)
      return {Q::stream_connection_unknown, {}};
    return {peer_quit(*lost), lost->peer_reason ? lost->peer_reason->substr(
                                                      0, rp::quit_text_limit)
                                                : std::string{}};
  }
  if (cancelled()) return {Q::stopped, {}};
  if (quits_without_reason()) return {};
  switch (result_) {
    case Error::session_init_in_use:
      return {Q::session_request_rp_in_use, {}};
    case Error::session_init_version_mismatch:
      return {Q::session_request_rp_version_mismatch, {}};
    case Error::session_init_connection_refused:
      return {Q::session_request_connection_refused, {}};
    case Error::ctrl_connection_refused:
      return {Q::ctrl_connect_failed, {}};
    default:
      break;
  }
  if (as<internet::PsnRemoteRegistrationFailure>())
    return {Q::psn_regist_failed, {}};
  switch (stage_) {
    case Stage::resolve:
    case Stage::registration:
    case Stage::session_init:
      switch (console_reason()) {
        case rp_reason_in_use:
          return {Q::session_request_rp_in_use, {}};
        case rp_reason_crash:
          return {Q::session_request_rp_crash, {}};
        case rp_reason_rp_version:
          return {Q::session_request_rp_version_mismatch, {}};
        default:
          return {Q::session_request_unknown, {}};
      }
    case Stage::ctrl_startup:
      return {Q::ctrl_connect_failed, {}};
    case Stage::ctrl_session_id:
    case Stage::ctrl_features:
    case Stage::ctrl_receive:
      return {Q::ctrl_unknown, {}};
    case Stage::calibration:
    case Stage::route_switch:
    case Stage::active_connect:
    case Stage::active_bang:
    case Stage::active_crypto:
    case Stage::stream_info:
    case Stage::media:
    case Stage::controller:
    case Stage::active_prepare:
      return {Q::stream_connection_unknown, {}};
    default:
      return {};
  }
}

auto LinkFailure::quits_without_reason() const noexcept -> bool {
  if (as<rp::RudpSwitchBarrierError>() || as<SwitchDrain>()) return true;
  const auto* init = as<rp::RemoteSessionInitFailure>();
  return init && init->kind == rp::RemoteSessionInitError::handshake_failed;
}

auto LinkFailure::explains_cancellation() const noexcept -> bool {
  if (const auto* route = as<RouteFailure>()) return route->drains_late_probe();
  return as<AudioRequestRefused>() ||
         as<internet::PsnRemoteRegistrationFailure>() || as<LaunchFailure>();
}

auto LinkFailure::describe() const noexcept -> std::string_view {
  const bool cancelled = this->cancelled();
  if (const auto* launch = as<LaunchFailure>())
    return describe(*launch, cancelled);
  if (cancelled && !explains_cancellation()) return "cancelled";
  const auto text = std::visit(
      [](const auto& cause) -> std::string_view {
        if constexpr (std::is_same_v<std::decay_t<decltype(cause)>,
                                     LaunchFailure>)
          return {};
        else
          return describe(cause);
      },
      cause_);
  return text.empty() ? result_text(result_, console_reason()) : text;
}

auto LinkFailure::sentence() const -> std::string {
  std::string text;
  if (route_) text = std::format("{}: ", route_->label());
  text += title_stage().value_or(LinkStages::name(stage_));
  text += std::format(" — {}.", describe());
  const auto received = received_bytes();
  if (result_ == Error::ctrl_truncated) {
    text += std::format(" Received {} response bytes.", received);
    if (const auto sent = sent_bytes())
      text += std::format(" Sent {} request bytes.", sent);
  } else if (result_ == Error::session_init_truncated ||
             ((as<internet::PsnRemoteRegistrationFailure>() ||
               as<rp::RemoteSessionInitFailure>() ||
               as<internet::remote_ctrl::ConnectFailure>()) &&
              received)) {
    text += std::format(" Received {} response bytes.", received);
  }
  text += cloud_detail();
  const auto quit = this->quit();
  if (!quit.text.empty())
    text += std::format(" Console reason: {}.", quit.text);
  if (const auto status = http_status())
    text += std::format(" HTTP {}.", status);
  if (const auto reason = console_reason())
    text += std::format(" Console reason 0x{:08x}.", reason);
  return text;
}

auto LinkFailure::cloud_detail() const -> std::string {
  const auto* launch = as<LaunchFailure>();
  if (!launch) return {};
  std::string text;
  if (const auto* refusal = launch->as<LaunchFailure::NotCleared>()) {
    text = std::format(" Detail {}.",
                       magic_enum::enum_name(refusal->refusal().kind()));
    if (const auto status = refusal->unanswered())
      text += std::format(" Check status {}.",
                          psn::graphql::CheckError::status_name(*status));
  } else if (const auto* title = launch->as<LaunchFailure::TitleRefused>()) {
    text = std::format(" Detail {}.",
                       magic_enum::enum_name(title->error().cause()));
  } else if (const auto* reservation =
                 launch->as<LaunchFailure::ReservationFailed>()) {
    const auto& error = reservation->error();
    text = std::format(" Detail {}.", magic_enum::enum_name(error.reason()));
    const auto best = error.best_rtt_ms();
    if (stage_ == Stage::cloud_datacenter &&
        error.detail() == static_cast<std::uint32_t>(
                              psn::gk::Error::DatacenterDetail::rejected) &&
        best && *best > 0)
      text += std::format(" Best measured RTT {} ms.", *best);
  }
  return text;
}

auto LinkFailure::title_stage() const noexcept
    -> std::optional<std::string_view> {
  const auto* launch = as<LaunchFailure>();
  const auto* title =
      launch ? launch->as<LaunchFailure::TitleRefused>() : nullptr;
  if (!title) return std::nullopt;
  return title->step();
}

auto LinkFailure::end_reason(std::optional<LaunchRefused> refused) const
    -> EndReason {
  EndReason::Facts facts;
  facts.step = LinkStages::step(stage_);
  facts.console_reason = console_reason();
  facts.http_status = http_status();
  facts.left = cancelled();
  facts.action_url = std::string{action_url()};
  facts.detail = std::string{message()};
  facts.launch_refused = std::move(refused);
  return EndReason{end_cause(), std::move(facts)};
}

auto LinkFailure::end_cause() const noexcept -> EndReason::Cause {
  if (const auto* reason = as<EndReason>()) return reason->cause();
  if (pairing_rejected()) return EndReason::Cause::pairing_rejected;
  if (const auto* active = as<rp::ActiveSessionFailure>();
      active != nullptr && active->cause == rp::ActiveSessionError::peer_silent)
    return EndReason::Cause::connection_lost;
  return LinkEvidence::cause(result_);
}

auto LinkFailure::pairing_rejected() const noexcept -> bool {
  if (const auto* reason = as<EndReason>())
    return reason->cause() == EndReason::Cause::pairing_rejected;
  const auto reason = console_reason();
  return result_ == Error::session_init_refused &&
         (reason == rp_reason_regist_failed ||
          reason == rp_reason_invalid_psn_id);
}

auto LinkFailure::stage_of(rp::ActiveHandshakeState state) noexcept -> Stage {
  using A = rp::ActiveHandshakeState;
  switch (state) {
    case A::awaiting_connection:
    case A::awaiting_profile:
      return Stage::active_connect;
    case A::awaiting_bang:
    case A::verifying_bang:
      return Stage::active_bang;
    case A::awaiting_crypto_install:
      return Stage::active_crypto;
    case A::awaiting_stream_info:
    case A::validating_stream_info:
      return Stage::stream_info;
    case A::ready:
      return Stage::media;
    case A::closed:
    case A::failed:
      break;
  }
  return Stage::unknown;
}

auto LinkFailure::stage_of(const LaunchFailure& failure) noexcept -> Stage {
  using G = psn::gk::Error::Stage;
  if (failure.as<LaunchFailure::Unmet>()) return Stage::cloud_provision;
  if (failure.as<LaunchFailure::NotCleared>()) return Stage::cloud_launch_check;
  if (failure.as<LaunchFailure::TitleRefused>()) return Stage::cloud_title;
  const auto& error = failure.as<LaunchFailure::ReservationFailed>()->error();
  switch (error.stage()) {
    case G::identity:
      return Stage::cloud_identity;
    case G::client_ids:
      return Stage::cloud_client_ids;
    case G::config:
      return Stage::cloud_config;
    case G::start:
      return Stage::cloud_start;
    case G::oauth:
      return Stage::cloud_oauth;
    case G::authorize:
      return Stage::cloud_authorize;
    case G::lock:
      return error.is_cancelled() || error.is_invalid_request()
                 ? Stage::cloud_authorize
                 : Stage::cloud_lock;
    case G::datacenters:
    case G::ping:
    case G::select:
      return Stage::cloud_datacenter;
    case G::allocate:
      break;
  }
  return Stage::cloud_allocate;
}

auto LinkFailure::result_of(
    const rp::RemoteSessionInitFailure& failure) noexcept -> Error {
  using K = rp::RemoteSessionInitError;
  switch (failure.kind) {
    case K::invalid_attempt:
      return Error::invalid_state;
    case K::request_failed:
      return Error::invalid_configuration;
    case K::stopped:
      return Error::session_init_invalid_response;
    case K::handshake_failed:
      return failure.cause == static_cast<std::uint32_t>(
                                  net::transport::rudp::ConnectError::timeout)
                 ? Error::session_init_timeout
                 : Error::session_init_network_failure;
    case K::exchange_failed:
      return failure.cause == static_cast<std::uint32_t>(
                                  rp::RudpCtrlHttpExchangeError::timeout)
                 ? Error::session_init_timeout
                 : Error::session_init_network_failure;
    case K::response_failed:
      if (failure.cause ==
          static_cast<std::uint32_t>(rp::ResponseCodecError::incomplete))
        return Error::session_init_truncated;
      if (failure.cause ==
          static_cast<std::uint32_t>(rp::ResponseCodecError::too_large))
        return Error::session_init_too_large;
      return Error::session_init_invalid_response;
    case K::response_rejected:
      if (failure.cause == static_cast<std::uint32_t>(
                               rp::SessionRefusal::Kind::version_mismatch))
        return Error::session_init_version_mismatch;
      return failure.protocol_reason == rp_reason_in_use
                 ? Error::session_init_in_use
                 : Error::session_init_refused;
  }
  return Error::session_init_invalid_response;
}

auto LinkFailure::result_of(
    const internet::remote_ctrl::ConnectFailure& failure) noexcept -> Error {
  if (failure.startup) {
    const auto& startup = *failure.startup;
    using S = internet::remote_ctrl::StartupError;
    if (startup.kind == S::exchange_failed)
      return static_cast<rp::RudpCtrlHttpExchangeError>(startup.cause) ==
                     rp::RudpCtrlHttpExchangeError::timeout
                 ? Error::ctrl_timeout
                 : Error::ctrl_network_failure;
    if (startup.kind == S::finalize_failed) {
      using F = rp::RemoteCtrlHttpFinalizeError;
      switch (static_cast<F>(startup.cause)) {
        case F::nonaccepted:
        case F::prohibited:
          return Error::ctrl_refused;
        case F::incomplete:
          return Error::ctrl_truncated;
        case F::too_large:
          return Error::ctrl_too_large;
        case F::invalid:
          break;
      }
    }
  }
  using K = internet::remote_ctrl::ConnectError;
  switch (failure.kind) {
    case K::invalid_attempt:
      return Error::invalid_state;
    case K::invalid_transport:
    case K::invalid_host:
    case K::invalid_port:
      return Error::invalid_configuration;
    case K::connect_failed:
      return failure.cause == static_cast<std::uint32_t>(
                                  net::transport::rudp::ConnectError::timeout)
                 ? Error::ctrl_timeout
                 : Error::ctrl_network_failure;
    case K::trailing_too_large:
      return Error::ctrl_too_large;
    case K::trailing_malformed:
    case K::trailing_crypto_failed:
      return Error::ctrl_invalid_response;
    case K::stopped:
    case K::prepare_failed:
    case K::startup_failed:
    case K::session_failed:
    case K::trailing_counter_exhausted:
    case K::trailing_lifecycle_failed:
      break;
  }
  return Error::ctrl_failed;
}

auto LinkFailure::result_text(Error result,
                              std::uint32_t console_reason) noexcept
    -> std::string_view {
  switch (result) {
    case Error::ctrl_truncated:
    case Error::session_init_truncated:
      return "the peer closed an incomplete response";
    case Error::ctrl_timeout:
    case Error::session_init_timeout:
      return "the response timed out";
    case Error::ctrl_network_failure:
    case Error::session_init_network_failure:
      return "the network operation failed";
    case Error::ctrl_invalid_response:
    case Error::session_init_invalid_response:
      return "the peer response was invalid";
    case Error::ctrl_too_large:
    case Error::session_init_too_large:
      return "the peer response exceeded the allowed size";
    case Error::ctrl_refused:
    case Error::session_init_refused:
      if (console_reason == rp_reason_crash)
        return "the console reported that Remote Play crashed";
      return "the console refused the request";
    case Error::session_init_in_use:
      return "Remote Play is already in use";
    case Error::session_init_connection_refused:
      return "the console refused the session request connection";
    case Error::ctrl_connection_refused:
      return "the console refused the Ctrl connection";
    case Error::resource_exhausted:
      return "the system ran out of file descriptors or sockets";
    case Error::session_init_version_mismatch:
      return "the console and client protocol versions disagree";
    case Error::ctrl_handoff_failure:
      return "the Ctrl transport handoff failed";
    case Error::provider_unavailable:
      return "a required platform provider is unavailable";
    case Error::invalid_configuration:
      return "the configuration is invalid";
    case Error::invalid_state:
      return "the operation arrived in an invalid state";
    case Error::controller_rejected:
      return "controller delivery failed";
    case Error::pin_rejected:
      return "the access code could not be submitted";
    case Error::shutdown_failed:
      return "resource cleanup failed";
    case Error::route_failed:
      return "route establishment failed";
    case Error::pipeline_failed:
      return "stream processing failed";
    case Error::ctrl_failed:
      return "the control operation failed";
    case Error::invalid_argument:
      return "an argument was invalid";
  }
  return "an unrecognized failure occurred";
}

auto LinkFailure::describe(std::monostate) noexcept -> std::string_view {
  return {};
}

auto LinkFailure::describe(const AudioRequestRefused& refused) noexcept
    -> std::string_view {
  return refused.describe();
}

auto LinkFailure::describe(ConsoleUnresolved) noexcept -> std::string_view {
  return {};
}

auto LinkFailure::describe(ConsoleRefusal) noexcept -> std::string_view {
  return {};
}

auto LinkFailure::describe(CtrlStartup) noexcept -> std::string_view {
  return {};
}

auto LinkFailure::describe(rp::CtrlReaderError error) noexcept
    -> std::string_view {
  using E = rp::CtrlReaderError;
  switch (error) {
    case E::invalid_argument:
      return "the reader arguments were invalid";
    case E::invalid_state:
      return "the reader was in an invalid state";
    case E::network_failure:
      return "the network read failed";
    case E::disconnected:
      return "the peer closed the control connection";
    case E::too_large:
      return "the control frame exceeded the allowed size";
    case E::crypto_failure:
      return "control frame decryption failed";
    case E::counter_exhausted:
      return "the control cipher counter was exhausted";
  }
  return {};
}

auto LinkFailure::describe(const rp::ActiveSessionFailure& failure) noexcept
    -> std::string_view {
  using E = rp::ActiveSessionError;
  switch (failure.cause) {
    case E::handshake_timeout:
      return "timed out waiting for the peer";
    case E::peer_disconnected:
      return "the peer ended the session";
    case E::peer_silent:
      return "the peer stopped sending data";
    case E::bang_authentication_failed:
      return "peer key authentication failed";
    case E::handshake_failed:
      return "the peer handshake or stream format was rejected";
    case E::post_failed:
      return "a protocol message could not be queued";
    default:
      return "the stream transport failed";
  }
}

auto LinkFailure::describe(net::protocol::senkusha::Error) noexcept
    -> std::string_view {
  return {};
}

auto LinkFailure::describe(net::protocol::takion::ClientError) noexcept
    -> std::string_view {
  return "the disconnect or transport close operation failed";
}

auto LinkFailure::describe(platform::PlatformError) noexcept
    -> std::string_view {
  return "the route worker could not start or complete";
}

auto LinkFailure::describe(const PsnFailure& failure) noexcept
    -> std::string_view {
  return failure.describe();
}

auto LinkFailure::describe(const WakeFailure& failure) noexcept
    -> std::string_view {
  return failure.describe();
}

auto LinkFailure::describe(const RouteFailure& failure) noexcept
    -> std::string_view {
  return failure.describe();
}

auto LinkFailure::describe(
    const internet::PsnRemoteRegistrationFailure& failure) noexcept
    -> std::string_view {
  using K = internet::PsnRemoteRegistrationError;
  switch (failure.kind) {
    case K::invalid_attempt:
      return "invalid registration attempt";
    case K::stopped:
      return "registration was cancelled";
    case K::timed_out:
      return "registration did not complete within 10 seconds";
    case K::request_failed:
      switch (static_cast<internet::PsnRequestError>(failure.cause)) {
        case internet::PsnRequestError::invalid_target:
          return "unsupported registration target";
        case internet::PsnRequestError::invalid_account_id:
          return "invalid account identifier length";
        case internet::PsnRequestError::invalid_psn_data:
          return "invalid PSN registration material length";
        case internet::PsnRequestError::invalid_host:
          return "invalid registration local address";
        case internet::PsnRequestError::random_failure:
          return "registration random generation failed";
        case internet::PsnRequestError::material_failure:
          return "registration key derivation failed";
        case internet::PsnRequestError::payload_failure:
          return "registration payload encoding failed";
      }
      break;
    case K::handshake_failed:
      if (failure.cause == static_cast<std::uint32_t>(
                               net::transport::rudp::ConnectError::timeout))
        return "RUDP handshake timed out before registration";
      return "RUDP handshake failed before registration";
    case K::exchange_failed:
      if (failure.cause ==
          static_cast<std::uint32_t>(rp::RudpCtrlHttpExchangeError::timeout))
        return "timed out waiting for the registration HTTP reply";
      return "registration HTTP transport failed";
    case K::response_failed:
      switch (static_cast<internet::PsnResponseError>(failure.cause)) {
        case internet::PsnResponseError::exchange_failed:
          return "registration reply transport failed";
        case internet::PsnResponseError::invalid_header:
          return "registration HTTP header is invalid";
        case internet::PsnResponseError::refused:
          return "console rejected registration";
        case internet::PsnResponseError::truncated:
          return "registration reply is incomplete";
        case internet::PsnResponseError::too_large:
          return "registration reply exceeds the size limit";
        case internet::PsnResponseError::crypto_failure:
          return "registration reply decryption failed";
        case internet::PsnResponseError::invalid_payload:
          return "decrypted registration payload is invalid";
      }
      break;
  }
  return "unrecognized registration failure";
}

auto LinkFailure::describe(const rp::RemoteSessionInitFailure&) noexcept
    -> std::string_view {
  return {};
}

auto LinkFailure::describe(
    const internet::remote_ctrl::ConnectFailure&) noexcept -> std::string_view {
  return {};
}

auto LinkFailure::describe(
    internet::remote_ctrl::OutboundStepError error) noexcept
    -> std::string_view {
  using K = internet::remote_ctrl::OutboundStepError;
  switch (error) {
    case K::unsupported_effect:
      return "the Ctrl send state rejected the operation";
    case K::stale_attempt:
      return "a Ctrl send belonged to an earlier attempt";
    case K::encode_failed:
      return "encoding or encrypting the Ctrl reply failed";
    case K::transport:
      return "sending the Ctrl reply failed";
    case K::short_send:
      return "the Ctrl reply was only partially sent";
    case K::invalid_pin:
      return "the supplied console PIN was invalid";
  }
  return {};
}

auto LinkFailure::describe(const RemoteCtrlRead& read) noexcept
    -> std::string_view {
  using K = internet::remote_ctrl::PumpError;
  switch (read.error) {
    case K::stopped:
      return "the control transport stopped";
    case K::step_failed:
      return "the RUDP Ctrl receive step failed";
    case K::inbox_rejected:
      return "the control state machine rejected the message";
    case K::stale_attempt:
      return "a stale control attempt was rejected";
    case K::timeout:
      return "the control response timed out";
    case K::receive_failed:
      return "receiving the RUDP datagram failed";
    case K::malformed:
      return "the RUDP datagram or embedded Ctrl frame is malformed";
    case K::crypto_failed:
      return "decrypting the RUDP Ctrl payload failed";
    case K::counter_exhausted:
      return "the RUDP Ctrl cipher counter was exhausted";
    case K::ack_encode_failed:
      return "encoding the RUDP acknowledgement failed";
    case K::ack_send_failed:
      return "sending the RUDP acknowledgement failed";
    case K::ack_short_send:
      return "the RUDP acknowledgement was only partially sent";
    case K::peer_closed:
      return "the console closed the RUDP control connection";
  }
  return {};
}

auto LinkFailure::describe(rp::RudpSwitchBarrierError error) noexcept
    -> std::string_view {
  using K = rp::RudpSwitchBarrierError;
  switch (error) {
    case K::invalid_attempt:
      return "the transport switch state rejected the attempt";
    case K::stopped:
      return "the transport switch was interrupted";
    case K::packet_failed:
      return "encoding the transport switch request failed";
    case K::send_failed:
      return "sending the transport switch request failed";
    case K::short_send:
      return "the transport switch request was only partially sent";
    case K::receive_failed:
      return "receiving the transport switch reply failed";
    case K::crypto_failed:
      return "decrypting Ctrl during the transport switch failed";
    case K::counter_exhausted:
      return "the Ctrl cipher counter was exhausted during transport switch";
    case K::ack_failed:
      return "acknowledging Ctrl during the transport switch failed";
    case K::timeout:
      return "the console did not acknowledge the transport switch before "
             "the deadline";
    case K::service_failed:
      return "servicing Ctrl during the transport switch failed";
    case K::malformed:
      return "the transport switch received a malformed Ctrl datagram";
  }
  return {};
}

auto LinkFailure::describe(SwitchDrain drain) noexcept -> std::string_view {
  return drain.error == net::transport::rudp::RemoteDatagramDrainError::poller
             ? "draining the switched route could not create a poller"
             : "draining stale datagrams from the switched route failed";
}

auto LinkFailure::describe(
    net::transport::rudp::RemoteDatagramDrainError) noexcept
    -> std::string_view {
  return {};
}

auto LinkFailure::describe(const LaunchFailure& failure,
                           bool cancelled) noexcept -> std::string_view {
  const auto text = cancelled && failure.as<LaunchFailure::Unmet>()
                        ? std::string_view{}
                        : failure.describe();
  if (!text.empty()) return text;
  if (cancelled) return "the cloud request was cancelled";
  return "the cloud service request failed or returned an invalid response";
}

auto LinkFailure::describe(const EndReason&) noexcept -> std::string_view {
  return {};
}

}  // namespace libchiaking::play
