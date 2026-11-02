#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/limits.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"

#include "src/core/play/adapters/links/active_stream.hpp"
#include "src/core/play/adapters/links/home_link_progress.hpp"
#include "src/core/play/adapters/links/unconfirmed_session.hpp"

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "src/core/net/protocol/remote_play/tcp_exchange.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play {

namespace rp = net::protocol::remote_play;
namespace progress = home_link_progress;

class HomeLink final : public Link, private ActiveStream::Owner {
 public:
  HomeLink(HomeLinkConfiguration configuration,
           platform::PlatformFactory& platform,
           platform::CryptoProvider& crypto)
      : Link(Venue::home()),
        configuration_(rp::LocalStreamPreparation::apply_disabled_video(
            std::move(configuration))),
        platform_(platform),
        stream_(*this, ending(), platform, configuration_.stop,
                {.component = diagnostics::Component::local,
                 .console = playing_ps5()
                                ? Seats::Core::ConsoleKind::remote_play_ps5
                                : Seats::Core::ConsoleKind::remote_play_ps4,
                 .feedback = configuration_.feedback}),
        crypto_(crypto),
        clock_(platform.clock()),
        random_(platform.random()),
        takion_crypto_(crypto, platform.threading()),
        ctrl_(crypto_, platform.network().sockets(), platform.clock()),
        post_id_ctrl_(configuration_.enable_dualsense_ctrl,
                      configuration_.enable_keyboard) {}

  ~HomeLink() override {
    static_cast<void>(stream_.release_media());
    queued_pin_.reset();
    ready_ticket_.reset();
    if (session_init_) static_cast<void>(session_init_->cancel());
    static_cast<void>(ctrl_.cancel());
  }

  [[nodiscard]] auto start() -> Result override {
    if (stream_.state() != rp::StreamState::idle)
      return unexpected(rp::StreamError::invalid_state);
    if (!stream_.has_sink() || configuration_.host.empty() ||
        configuration_.ctrl_endpoint.port == 0 ||
        configuration_.active_stream_port == 0)
      return unexpected(rp::StreamError::invalid_configuration);
    if (auto audio = LinkFailure::AudioRequestRefused::for_console(
            configuration_.console.target,
            configuration_.tuning.requested_audio_channels)) {
      ending().record_failure(
          LinkFailure{rp::FailureStage::unknown,
                      rp::StreamError::invalid_configuration, *audio});
      return unexpected(rp::StreamError::invalid_configuration);
    }
    if (configuration_.stop.stop_requested())
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::cancelled);
    const auto did = rp::LocalStreamPreparation::resolve_local_did(
        configuration_.did, random_);
    if (!did)
      return stream_.fail(rp::StreamError::ctrl_failed,
                          rp::StreamFailure::negotiation_failed,
                          LinkFailure{rp::FailureStage::ctrl_startup,
                                      rp::StreamError::ctrl_failed});
    configuration_.did = *did;
    if (auto resolved = resolve_console(); !resolved) return resolved;
    aim_route();
    stream_.at(rp::FailureStage::session_init);
    if (const auto started = start_session_exchange(); !started)
      return stream_.fail(started.error(),
                          rp::StreamFailure::negotiation_failed);
    progress_.process(progress::SessionRequested{});
    stream_.enter(rp::StreamState::establishing_route);
    return {};
  }

  [[nodiscard]] auto pump_once() -> Result override {
    if (!stream_.running()) return unexpected(rp::StreamError::invalid_state);
    if (configuration_.stop.stop_requested()) return stop();
    stream_.observe();
    switch (progress_.stage()) {
      case progress::Stage::requesting_session:
        return pump_session_init();
      case progress::Stage::awaiting_control:
        if (!ctrl_ready_.expired(platform_.clock())) return {};
        if (auto opened = open_ctrl(); !opened) return opened;
        break;
      default:
        break;
    }
    if (stream_.ctrl_open()) {
      if (auto serviced = service_ctrl(); !serviced) return serviced;
      if (!stream_.running()) return {};
    }
    switch (progress_.stage()) {
      case progress::Stage::awaiting_session_id:
        return await_session_id();
      case progress::Stage::session_id_known:
        if (progress_.asking_for_pin()) return {};
        stream_.at(rp::FailureStage::ctrl_session_id);
        return begin_calibration();
      case progress::Stage::calibrating:
      case progress::Stage::active:
        stream_.at(rp::FailureStage::ctrl_session_id);
        return pump_pipeline();
      default:
        return {};
    }
  }

  [[nodiscard]] auto stop() -> Result override {
    if (!stream_.stopping()) return {};
    return stream_.stopped(release_route());
  }

  [[nodiscard]] auto wait(std::chrono::milliseconds timeout)
      -> Result override {
    return stream_.wait(timeout,
                        {stream_.ctrl_open() ? ctrl_.pollable() : nullptr,
                         session_init_ ? session_init_->pollable() : nullptr});
  }

  [[nodiscard]] auto connect_microphone() -> Result override {
    return queue_runtime_ctrl(rp::CtrlCodec::microphone_connect_frame());
  }

  [[nodiscard]] auto set_microphone_muted(bool muted) -> Result override {
    return queue_runtime_ctrl(rp::CtrlCodec::microphone_mute_frame(muted));
  }

  [[nodiscard]] auto submit_pin(std::span<const std::byte> pin)
      -> Result override {
    if (stream_.ended() || pin.empty() ||
        pin.size() > rp::ctrl_frame_max_payload)
      return unexpected(rp::StreamError::pin_rejected);
    queued_pin_.reset();
    const bool establishing =
        stream_.state() == rp::StreamState::establishing_route;
    const bool asking = progress_.asking_for_pin();
    if (!stream_.ctrl_open() && asking && establishing) {
      progress_.process(progress::PinSent{});
      wait_for_ctrl_answer();
      return {};
    }
    queued_pin_.emplace(pin);
    auto* ctrl_session = ctrl_.session();
    if (!establishing || !asking || !ctrl_session) return {};
    return send_queued_pin(*ctrl_session);
  }

  auto takion_version() const noexcept
      -> std::optional<std::uint32_t> override {
    if (const auto committed = stream_.committed_takion_version())
      return committed;
    const auto profile = rp::LocalStreamPreparation::console_active_profile(
        configuration_.console.target,
        configuration_.tuning.takion_version_override);
    if (!profile) return std::nullopt;
    return profile->version();
  }

 private:
  class SenkushaSink final : public net::protocol::senkusha::EventSink {
   public:
    void on_event(net::protocol::senkusha::Event event) override {
      if (const auto* changed =
              std::get_if<net::protocol::senkusha::event::StateChanged>(
                  &event)) {
        if (changed->state != net::protocol::senkusha::State::failed)
          last_phase = changed->state;
      } else if (const auto* failed =
                     std::get_if<net::protocol::senkusha::event::Failed>(
                         &event)) {
        last_error = failed->error;
      }
    }

    std::optional<net::protocol::senkusha::State> last_phase;
    std::optional<net::protocol::senkusha::Error> last_error;
  };

  enum class Step : std::uint8_t { continuing, paused };

  static constexpr std::size_t ctrl_frames_per_pump = rp::queue_capacity;
  static constexpr std::size_t ctrl_effects_per_pump = rp::queue_capacity;
  static constexpr std::uint8_t max_session_retries = 2;

  static auto map_session_exchange_error(
      rp::ConsoleExchangeError error) noexcept -> rp::StreamError {
    switch (error) {
      case rp::ConsoleExchangeError::network_failure:
        return rp::StreamError::session_init_network_failure;
      case rp::ConsoleExchangeError::timeout:
        return rp::StreamError::session_init_timeout;
      case rp::ConsoleExchangeError::truncated:
        return rp::StreamError::session_init_truncated;
      case rp::ConsoleExchangeError::invalid_response:
        return rp::StreamError::session_init_invalid_response;
      case rp::ConsoleExchangeError::too_large:
        return rp::StreamError::session_init_too_large;
      case rp::ConsoleExchangeError::connection_refused:
        return rp::StreamError::session_init_connection_refused;
      case rp::ConsoleExchangeError::resource_exhausted:
        return rp::StreamError::resource_exhausted;
      case rp::ConsoleExchangeError::invalid_argument:
      case rp::ConsoleExchangeError::invalid_state:
      case rp::ConsoleExchangeError::refused:
        return rp::StreamError::ctrl_failed;
    }
    return rp::StreamError::ctrl_failed;
  }

  static auto map_ctrl_attempt_error(rp::LocalCtrlError error) noexcept
      -> rp::StreamError {
    using Exchange = rp::ConsoleExchangeError;
    using Fault = rp::LocalCtrlFault;
    using Stream = rp::StreamError;
    if (error == rp::LocalCtrlError{Fault::handoff_failure})
      return Stream::ctrl_handoff_failure;
    const auto* exchange = std::get_if<Exchange>(&error);
    if (!exchange) return Stream::ctrl_failed;
    switch (*exchange) {
      case Exchange::network_failure:
        return Stream::ctrl_network_failure;
      case Exchange::timeout:
        return Stream::ctrl_timeout;
      case Exchange::truncated:
        return Stream::ctrl_truncated;
      case Exchange::invalid_response:
        return Stream::ctrl_invalid_response;
      case Exchange::too_large:
        return Stream::ctrl_too_large;
      case Exchange::refused:
        return Stream::ctrl_refused;
      case Exchange::connection_refused:
        return Stream::ctrl_connection_refused;
      case Exchange::resource_exhausted:
        return Stream::resource_exhausted;
      case Exchange::invalid_argument:
      case Exchange::invalid_state:
        break;
    }
    return Stream::ctrl_failed;
  }

  static auto route_failure_text(rp::DirectRouteError error) noexcept
      -> std::string_view {
    switch (error) {
      case rp::DirectRouteError::invalid_configuration:
        return "local route: invalid UDP configuration";
      case rp::DirectRouteError::unsupported_capability:
        return "local route: unsupported UDP capability";
      case rp::DirectRouteError::transport_failure:
        return "local route: UDP transport failed";
    }
    return "local route: UDP route failed";
  }

  static auto calibration_failure_text(
      rp::LocalCalibrationStartError error) noexcept -> std::string_view {
    switch (error) {
      case rp::LocalCalibrationStartError::invalid_ticket:
        return "local route: invalid Ctrl ticket";
      case rp::LocalCalibrationStartError::invalid_route:
        return "local route: missing UDP transport";
      case rp::LocalCalibrationStartError::route_unavailable:
        return "local route: UDP route unavailable";
      case rp::LocalCalibrationStartError::senkusha_start_failed:
        return "local route: Senkusha start failed";
    }
    return "local route: calibration failed";
  }

  static auto reader_failure() -> LinkFailure {
    return LinkFailure{rp::FailureStage::ctrl_receive,
                       rp::StreamError::ctrl_failed};
  }

  static auto reader_failure(rp::CtrlReaderError cause) -> LinkFailure {
    return LinkFailure{rp::FailureStage::ctrl_receive,
                       rp::StreamError::ctrl_failed, cause};
  }

  auto playing_ps5() const noexcept -> bool {
    return configuration_.console.target == rp::RegistrationTarget::ps5_1;
  }

  auto active_stream() noexcept -> ActiveStream* override { return &stream_; }
  auto active_stream() const noexcept -> const ActiveStream* override {
    return &stream_;
  }

  [[nodiscard]] auto release_route() -> bool override {
    progress_.process(progress::RouteReleased{});
    ctrl_nonce_.reset();
    ready_ticket_.reset();
    queued_pin_.reset();
    runtime_ctrl_.clear();
    ctrl_ready_ = {};
    bool cleaned = stream_.release_media();
    if (session_init_) {
      if (!session_init_->cancel()) cleaned = false;
      session_init_.reset();
    }
    if (!ctrl_.cancel()) cleaned = false;
    return cleaned;
  }

  [[nodiscard]] auto resolve_console() -> Result {
    stream_.at(rp::FailureStage::resolve);
    auto ctrl_endpoint = configuration_.ctrl_endpoint;
    ctrl_candidates_.clear();
    if (ctrl_endpoint.address.is_unspecified()) {
      const auto resolved = platform_.network().dns().resolve(
          configuration_.host, std::to_string(ctrl_endpoint.port),
          configuration_.stop);
      if (!resolved && configuration_.stop.stop_requested())
        return stream_.fail(rp::StreamError::route_failed,
                            rp::StreamFailure::cancelled);
      if (!resolved) {
        return stream_.fail(
            rp::StreamError::ctrl_failed,
            rp::StreamFailure::route_establishment_failed,
            LinkFailure{rp::FailureStage::resolve, rp::StreamError::ctrl_failed,
                        LinkFailure::ConsoleUnresolved{resolved.error()}});
      }
      const bool ipv4 = ctrl_endpoint.address.is_ipv4() &&
                        configuration_.host.find(':') == std::string::npos;
      std::ranges::copy_if(
          resolved.value(), std::back_inserter(ctrl_candidates_),
          [ipv4](const auto& address) { return address.is_ipv4() == ipv4; });
      if (ctrl_candidates_.empty())
        return stream_.fail(rp::StreamError::ctrl_failed,
                            rp::StreamFailure::route_establishment_failed);
      ctrl_endpoint.address = ctrl_candidates_.front();
    } else {
      ctrl_candidates_.push_back(ctrl_endpoint.address);
    }
    stream_.trace().complete(rp::FailureStage::resolve);
    resolved_ctrl_endpoint_ = ctrl_endpoint;
    return {};
  }

  void aim_route() {
    auto& route = configuration_.route;
    route_follows_ctrl_ = route.remote.address.is_unspecified();
    if (route_follows_ctrl_)
      route.remote.address = resolved_ctrl_endpoint_.address;
    if (route.remote.port == 0) route.remote.port = senkusha_port;
    if (route.local.address.is_unspecified() &&
        route.local.address.family() != route.remote.address.family())
      route.local.address = route.remote.address.is_ipv4()
                                ? platform::IpAddress::any_ipv4()
                                : platform::IpAddress::any_ipv6();
  }

  [[nodiscard]] auto pump_session_init() -> Result {
    stream_.at(rp::FailureStage::session_init);
    const auto progress = session_init_->pump_once();
    if (!progress) {
      if (session_init_->failed_while_connecting() &&
          ctrl_candidate_ + 1 < ctrl_candidates_.size()) {
        ++ctrl_candidate_;
        if (const auto next = start_session_candidates(); !next)
          return stream_.fail(next.error(),
                              rp::StreamFailure::negotiation_failed);
        return {};
      }
      return stream_.fail(map_session_exchange_error(progress.error()),
                          rp::StreamFailure::negotiation_failed);
    }
    if (!progress.value()) return {};
    auto response = session_init_->take();
    if (!response)
      return stream_.fail(map_session_exchange_error(response.error()),
                          rp::StreamFailure::negotiation_failed);
    session_init_.reset();
    auto result = std::move(response.value());
    if (!result) {
      const auto refusal = session_retries_ >= max_session_retries
                               ? result.error().settled()
                               : result.error();
      if (refusal.kind() == rp::SessionRefusal::Kind::version_mismatch)
        return retry_session_with(refusal.retry_target());
      return refuse_session(refusal);
    }
    stream_.trace().complete(rp::FailureStage::session_init);
    ctrl_nonce_.emplace(result.value().take_nonce());
    progress_.process(progress::SessionAccepted{});
    log_stage("local stage: session-init accepted");
    ctrl_ready_ = timing::Deadline::after(
        platform_.clock(), timing::Deadline::Duration{rp::ctrl_ready_delay_us});
    return {};
  }

  [[nodiscard]] auto retry_session_with(
      std::optional<rp::RegistrationTarget> target) -> Result {
    if (!target || session_retries_ >= max_session_retries)
      return stream_.fail(rp::StreamError::session_init_version_mismatch,
                          rp::StreamFailure::negotiation_failed);
    ++session_retries_;
    stream_.trace().record(diagnostics::Operation::local_version_retry,
                           diagnostics::Outcome::retry, session_retries_);
    configuration_.console.target = *target;
    if (const auto retried = start_session_exchange(); !retried)
      return stream_.fail(retried.error(),
                          rp::StreamFailure::negotiation_failed);
    return {};
  }

  [[nodiscard]] auto refuse_session(const rp::SessionRefusal& result)
      -> Result {
    log(platform::LogSeverity::error,
        std::format("local session-init rejected: http={} reason={}",
                    static_cast<unsigned>(result.status()),
                    static_cast<unsigned>(result.application_reason())));
    const auto error = result.application_reason() == rp_reason_in_use
                           ? rp::StreamError::session_init_in_use
                           : rp::StreamError::session_init_refused;
    return stream_.fail(
        error, rp::StreamFailure::negotiation_failed,
        LinkFailure{rp::FailureStage::session_init, error,
                    LinkFailure::ConsoleRefusal{result.application_reason(),
                                                result.http_status()}});
  }

  [[nodiscard]] auto open_ctrl() -> Result {
    ctrl_ready_ = {};
    stream_.at(rp::FailureStage::ctrl_startup);
    if (const auto started =
            start_ctrl({ctrl_nonce_->data(), ctrl_nonce_->size()});
        !started)
      return stream_.fail(map_ctrl_attempt_error(started.error()),
                          rp::StreamFailure::negotiation_failed);
    progress_.process(progress::ControlOpened{});
    wait_for_ctrl_answer();
    return {};
  }

  [[nodiscard]] auto await_session_id() -> Result {
    if (progress_.asking_for_pin() ||
        !ctrl_answer_due_.expired(platform_.clock()))
      return {};
    return synthesize_fallback_id();
  }

  void wait_for_ctrl_answer() {
    ctrl_answer_due_ = timing::Deadline::after(
        platform_.clock(), timing::Deadline::Duration{rp::ctrl_start_wait_us});
  }

  auto waiting_for_session_id() const noexcept -> bool {
    return !progress_.knows_session_id();
  }

  void close_ctrl(std::string_view why) {
    stream_.close_ctrl();
    runtime_ctrl_.clear();
    log_stage(why);
  }

  [[nodiscard]] auto on_ctrl_failure(rp::StreamError error,
                                     std::optional<LinkFailure> detail,
                                     bool ctrl_closed) -> Result {
    if (ctrl_closed)
      close_ctrl("local stage: Ctrl failed; it is no longer serviced");
    progress_.process(progress::ControlFailed{{error, std::move(detail)}});
    return carry_out_progress();
  }

  [[nodiscard]] auto carry_out_progress() -> Result {
    while (auto effect = progress_.take_effect()) {
      if (auto done = carry_out(std::move(*effect)); !done) return done;
    }
    return {};
  }

  [[nodiscard]] auto carry_out(progress::Effect effect) -> Result {
    if (auto* ending = std::get_if<progress::EndStream>(&effect))
      return stream_.fail(ending->failure.error,
                          rp::StreamFailure::negotiation_failed,
                          std::move(ending->failure.detail));
    if (auto* earlier =
            std::get_if<progress::EndStreamForEarlierFailure>(&effect)) {
      log_ctrl_failure("local ctrl: failed before calibration completed");
      return stream_.fail(earlier->failure.error,
                          rp::StreamFailure::negotiation_failed,
                          std::move(earlier->failure.detail));
    }
    if (std::holds_alternative<progress::NoteDeferredFailure>(effect))
      log_stage("local stage: Ctrl failed; checked when calibration completes");
    return {};
  }

  [[nodiscard]] auto ctrl_startup_failure(rp::LocalCtrlError error) -> Result {
    const auto progress = ctrl_.last_failure_progress();
    if (error == rp::LocalCtrlError{rp::ConsoleExchangeError::truncated})
      log(platform::LogSeverity::error,
          std::format("local ctrl response closed: sent={} received={}",
                      progress ? progress->request_bytes_sent : 0,
                      progress ? progress->response_bytes_received : 0));
    const auto mapped = map_ctrl_attempt_error(error);
    return on_ctrl_failure(
        mapped,
        LinkFailure{rp::FailureStage::ctrl_startup, mapped,
                    LinkFailure::CtrlStartup{
                        error, progress ? progress->request_bytes_sent : 0,
                        progress ? progress->response_bytes_received : 0}},
        true);
  }

  [[nodiscard]] auto service_ctrl() -> Result {
    if (!stream_.has_media()) stream_.at(rp::FailureStage::ctrl_startup);
    if (!ctrl_.session()) {
      const auto ready = ctrl_.pump_once();
      if (!ready) return ctrl_startup_failure(ready.error());
      if (!ready.value()) return {};
    }
    auto* ctrl_session = ctrl_.session();
    if (!ctrl_session) {
      log_ctrl_failure("local ctrl: session missing");
      return on_ctrl_failure(rp::StreamError::ctrl_failed, std::nullopt, true);
    }
    if (!progress_.control_answered() && !announce_ctrl(*ctrl_session))
      return {};
    if (!stream_.has_media()) stream_.at(rp::FailureStage::ctrl_receive);
    if (stream_.take_watched_readable(0) || ctrl_backlog_) {
      if (auto received = receive_ctrl(*ctrl_session);
          !received || !stream_.ctrl_open())
        return received;
    }
    if (waiting_for_session_id())
      receive_session_id(ctrl_session->take_ready_ticket());
    if (ready_ticket_ && !std::exchange(ctrl_logged_, true))
      log_stage("local stage: Ctrl session ID ready");
    if (auto stepped = step_ctrl(*ctrl_session);
        !stepped || stepped.value() == Step::paused)
      return stepped ? Result{} : unexpected(stepped.error());
    drain_ctrl_notices(*ctrl_session);
    if (!stream_.running()) return {};
    return send_ctrl(*ctrl_session);
  }

  auto announce_ctrl(rp::LocalCtrlSessionId& ctrl_session) -> bool {
    progress_.process(progress::ControlAnswer{});
    stream_.trace().complete(rp::FailureStage::ctrl_startup);
    log_stage("local stage: Ctrl startup accepted; awaiting session ID");
    stream_.prohibit(ctrl_session.ctrl_response().prohibited);
    if (!stream_.running()) return false;
    if (progress_.take<progress::PublishLateSessionId>())
      if (auto late = make_fallback_id())
        static_cast<void>(ctrl_session.publish_fallback(std::move(*late)));
    return true;
  }

  void receive_session_id(std::optional<rp::LocalCtrlReadyTicket> ticket) {
    ready_ticket_ = std::move(ticket);
    if (ready_ticket_) progress_.process(progress::SessionIdReceived{});
  }

  [[nodiscard]] auto receive_ctrl(rp::LocalCtrlSessionId& ctrl_session)
      -> Result {
    for (std::size_t count = 0; count < ctrl_frames_per_pump; ++count) {
      const auto ctrl_progress = ctrl_session.pump_once();
      if (!ctrl_progress) {
        if (ctrl_progress.error() == rp::CtrlReaderError::disconnected) {
          close_ctrl("local stage: console closed Ctrl; continuing without it");
          return {};
        }
        log_ctrl_failure("local ctrl: session pump failed");
        return on_ctrl_failure(rp::StreamError::ctrl_failed,
                               reader_failure(ctrl_progress.error()), true);
      }
      if (!ctrl_progress.value()) {
        ctrl_backlog_ = false;
        return {};
      }
      note_ctrl_frame(ctrl_session);
    }
    ctrl_backlog_ = true;
    return {};
  }

  void note_ctrl_frame(rp::LocalCtrlSessionId& ctrl_session) {
    if (ctrl_frames_logged_ >= rp::ctrl_frames_logged_max) return;
    ++ctrl_frames_logged_;
    const auto* owner = ctrl_session.owner();
    if (owner && owner->last_frame_type() && owner->last_display_state())
      log_stage(std::format(
          "local stage: Ctrl frame {} type=0x{:04x} display={},{}",
          ctrl_frames_logged_, static_cast<unsigned>(*owner->last_frame_type()),
          static_cast<unsigned>((*owner->last_display_state())[0]),
          static_cast<unsigned>((*owner->last_display_state())[1])));
    else if (owner && owner->last_frame_type())
      log_stage(std::format("local stage: Ctrl frame {} type=0x{:04x}",
                            ctrl_frames_logged_,
                            static_cast<unsigned>(*owner->last_frame_type())));
    else
      log_stage("local stage: first Ctrl frame received");
  }

  [[nodiscard]] auto step_ctrl(rp::LocalCtrlSessionId& ctrl_session)
      -> Expected<Step, rp::StreamError> {
    namespace lifecycle = rp::ctrl_lifecycle_detail;
    const auto paused = [](Result result) -> Expected<Step, rp::StreamError> {
      if (!result) return unexpected(result.error());
      return Step::paused;
    };
    for (std::size_t steps = 0; steps < ctrl_effects_per_pump; ++steps) {
      if (progress_.asking_for_pin()) ctrl_session.promote_heartbeat_replies();
      const bool pin_was_pending = ctrl_session.pin_pending();
      auto effect = rp::LocalCtrlSessionId::step_effect(ctrl_session, 1);
      if (pin_was_pending && !ctrl_session.pin_pending())
        wait_for_ctrl_answer();
      if (!effect) {
        log_ctrl_failure("local ctrl: effect step failed");
        return paused(on_ctrl_failure(rp::StreamError::ctrl_failed,
                                      reader_failure(), true));
      }
      const auto progress = effect.value().progress;
      if (progress == rp::LocalCtrlEffectProgress::idle) break;
      if (progress == rp::LocalCtrlEffectProgress::pin_required)
        return paused(prompt_for_pin(ctrl_session));
      if (const auto& notification = effect.value().notification) {
        if (std::holds_alternative<lifecycle::PublishSessionIdFallback>(
                *notification)) {
          if (waiting_for_session_id())
            if (const auto synthesized = synthesize_fallback_id(); !synthesized)
              return unexpected(synthesized.error());
          continue;
        }
        if (std::holds_alternative<lifecycle::PublishFailed>(*notification)) {
          log_ctrl_failure("local ctrl: failed notification");
          return paused(on_ctrl_failure(rp::StreamError::ctrl_failed,
                                        reader_failure(), true));
        }
        if (std::holds_alternative<lifecycle::PublishStopped>(*notification)) {
          close_ctrl("local stage: Ctrl stopped; continuing without it");
          return Step::paused;
        }
        if (std::holds_alternative<lifecycle::PublishCtrlFailed>(
                *notification) &&
            UnconfirmedSession{synthesized_session_id_}.starts_over(
                stream_.state())) {
          log_ctrl_failure(
              "local ctrl: Login PIN requested after a made-up "
              "session ID; a fresh session will ask for it");
          return paused(
              unconfirmed_session_or(rp::StreamError::ctrl_failed,
                                     rp::StreamFailure::negotiation_failed));
        }
        if (std::holds_alternative<lifecycle::PublishCtrlFailed>(
                *notification)) {
          log_ctrl_failure(
              "local ctrl: Login PIN requested after the session ID");
          if (const auto flagged =
                  on_ctrl_failure(rp::StreamError::ctrl_failed,
                                  LinkFailure{rp::FailureStage::ctrl_receive,
                                              rp::StreamError::ctrl_failed},
                                  false);
              !flagged)
            return unexpected(flagged.error());
        }
        continue;
      }
      if (ctrl_session.heartbeat_pending() || ctrl_session.pin_pending()) break;
    }
    return Step::continuing;
  }

  [[nodiscard]] auto prompt_for_pin(rp::LocalCtrlSessionId& ctrl_session)
      -> Result {
    progress_.process(progress::PinRequested{});
    if (const auto prompt = progress_.take<progress::AskForPin>()) {
      ctrl_session.set_pin_prompt_open(true);
      ctrl_session.coalesce_pin_requests();
      stream_.emit(rp::event::PinRequired{prompt->retry});
      if (stream_.state() != rp::StreamState::establishing_route ||
          ctrl_.session() != &ctrl_session)
        return {};
    }
    if (queued_pin_) return send_queued_pin(ctrl_session);
    return {};
  }

  [[nodiscard]] auto send_ctrl(rp::LocalCtrlSessionId& ctrl_session) -> Result {
    auto* owner = ctrl_session.owner();
    if (!owner || owner->state() != rp::ctrl_lifecycle_detail::State::active)
      return {};
    const bool media = stream_.has_media();
    if (media && post_id_ctrl_.done() && !runtime_ctrl_.pending() &&
        !ctrl_session.heartbeat_pending() && !ctrl_session.pin_pending() &&
        ctrl_session.take_post_id_repeat())
      post_id_ctrl_.restart();
    if (ready_ticket_ || (media && !post_id_ctrl_.done())) {
      if (!media) stream_.at(rp::FailureStage::ctrl_features);
      if (const auto flushed = post_id_ctrl_.pump(*owner, 1); !flushed) {
        log_ctrl_failure("local ctrl: post-ID pump failed");
        return on_ctrl_failure(rp::StreamError::ctrl_failed, std::nullopt,
                               true);
      }
    }
    if (post_id_ctrl_.done()) {
      auto& features = stream_.features();
      while (const auto* frame = features.front_outbound()) {
        if (!runtime_ctrl_.queue(frame->type, frame->payload)) break;
        features.pop_outbound();
      }
    }
    if (media && post_id_ctrl_.done()) {
      if (const auto sent = runtime_ctrl_.pump(*owner, 1); !sent) {
        log_ctrl_failure("local ctrl: runtime send failed");
        return on_ctrl_failure(rp::StreamError::ctrl_failed, std::nullopt,
                               true);
      }
    }
    return {};
  }

  void drain_ctrl_notices(rp::LocalCtrlSessionId& ctrl_session) {
    stream_.deliver_notices([&](auto& events, auto& couch) {
      while (auto notice = ctrl_session.take_notice())
        stream_.features().on_notice(std::move(*notice), events, &couch);
    });
  }

  auto calibration_inputs(const rp::LocalVideoSelection& selection)
      -> rp::LocalCalibrationInputs {
    rp::LocalCalibrationInputs inputs{
        {clock_, platform_.threading(), random_, crypto_, takion_crypto_,
         configuration_.console, selection.codec, selection.video},
        senkusha_sink_,
        {}};
    inputs.pipeline.trace = &stream_.trace();
    inputs.pipeline.dualsense = configuration_.enable_dualsense_ctrl;
    inputs.pipeline.pad_count = stream_.couch().session_pad_count();
    inputs.pipeline.enable_idr_on_fec_failure =
        configuration_.enable_idr_on_fec_failure;
    inputs.pipeline.tuning = configuration_.tuning;
    inputs.pipeline.transition_route = [this](rp::EstablishedRoute calibrated) {
      return move_to_active_route(std::move(calibrated));
    };
    inputs.pipeline.fallback_route = [this]() { return open_fallback_route(); };
    inputs.senkusha_config.policy =
        UnconfirmedSession{synthesized_session_id_}.calibration(
            inputs.senkusha_config.policy);
    return inputs;
  }

  auto move_to_active_route(rp::EstablishedRoute calibrated)
      -> Expected<rp::EstablishedRoute, rp::LocalPipelineError> {
    if (!calibrated.transport || calibrated.kind != rp::RouteKind::local_direct)
      return unexpected(rp::LocalPipelineError::route_unavailable);
    stream_.route_through(nullptr);
    if (!calibrated.transport.close())
      return unexpected(rp::LocalPipelineError::route_unavailable);
    const auto active_config = rp::LocalStart::active_route_config(
        configuration_.route, calibrated, configuration_.active_stream_port);
    auto active_route =
        rp::DirectRoute::connect(platform_.network(), active_config);
    if (!active_route)
      return unexpected(rp::LocalPipelineError::route_unavailable);
    return adopt(std::move(active_route.value()),
                 std::move(calibrated.calibration));
  }

  auto open_fallback_route()
      -> Expected<rp::EstablishedRoute, rp::LocalPipelineError> {
    stream_.route_through(nullptr);
    if (configuration_.stop.stop_requested())
      return unexpected(rp::LocalPipelineError::senkusha_failed);
    auto calibration =
        rp::LocalStreamPreparation::make_reference_fallback_calibration(
            configuration_.console.target,
            configuration_.tuning.takion_version_override);
    if (!calibration)
      return unexpected(rp::LocalPipelineError::route_unavailable);
    auto active_config = configuration_.route;
    active_config.remote.port = configuration_.active_stream_port;
    active_config.dont_fragment = rp::DontFragmentPolicy::disabled;
    log_stage("local stage: Senkusha fallback; opening active route");
    stream_.at(rp::FailureStage::active_connect);
    auto route = rp::DirectRoute::connect(platform_.network(), active_config);
    if (!route) return unexpected(rp::LocalPipelineError::route_unavailable);
    return adopt(std::move(route.value()), std::move(*calibration));
  }

  auto adopt(rp::RouteCandidate next,
             net::protocol::senkusha::Result calibration)
      -> rp::EstablishedRoute {
    stream_.route_through(next.transport ? next.transport->pollable()
                                         : nullptr);
    return rp::EstablishedRoute{
        {std::move(next.transport), next.local_endpoint, next.remote_endpoint,
         next.kind, next.dont_fragment},
        std::move(calibration)};
  }

  [[nodiscard]] auto begin_calibration() -> Result {
    if (!ready_ticket_) return {};
    stream_.trace().complete(rp::FailureStage::ctrl_session_id);
    const auto* ctrl_session = ctrl_.session();
    const auto selection =
        select_video(ctrl_session ? &ctrl_session->ctrl_response() : nullptr);
    launch_ = rp::LocalStart::effective_launch(selection);
    auto inputs = calibration_inputs(selection);
    stream_.at(rp::FailureStage::calibration);
    auto route =
        rp::DirectRoute::connect(platform_.network(), configuration_.route);
    if (!route) return calibrate_on_reference(route.error(), inputs);
    stream_.route_through(route.value().transport
                              ? route.value().transport->pollable()
                              : nullptr);
    auto started = rp::LocalStart::start_calibration(
        std::move(route.value()), std::move(*ready_ticket_), std::move(inputs));
    ready_ticket_.reset();
    if (!started) {
      stream_.route_through(nullptr);
      log(platform::LogSeverity::error,
          calibration_failure_text(started.error()));
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::route_establishment_failed);
    }
    stream_.calibrate(std::move(started.value()));
    progress_.process(progress::CalibrationStarted{});
    log_stage("local stage: Senkusha calibration started");
    stream_.enter(rp::StreamState::negotiating_stream);
    return {};
  }

  [[nodiscard]] auto calibrate_on_reference(
      rp::DirectRouteError error, const rp::LocalCalibrationInputs& inputs)
      -> Result {
    log(platform::LogSeverity::error, route_failure_text(error));
    if (configuration_.stop.stop_requested())
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::route_establishment_failed);
    auto fallback = rp::LocalCalibratedPipeline::start_reference_fallback(
        std::move(*ready_ticket_), inputs.pipeline);
    ready_ticket_.reset();
    if (!fallback)
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::route_establishment_failed);
    stream_.calibrate(std::move(fallback.value()));
    progress_.process(progress::CalibrationStarted{});
    stream_.enter(rp::StreamState::negotiating_stream);
    return {};
  }

  [[nodiscard]] auto pump_pipeline() -> Result {
    stream_.at(rp::FailureStage::calibration);
    const auto progress = stream_.pump_media();
    if (progress == ActiveStream::Progress::failed && stream_.pump_error()) {
      if (!peer_disconnected()) log_pipeline_failure(*stream_.pump_error());
      return unconfirmed_session_or(rp::StreamError::pipeline_failed,
                                    rp::StreamFailure::pipeline_failed);
    }
    if (stream_.active() && progress_.stage() == progress::Stage::calibrating) {
      progress_.process(progress::ActiveSessionStarted{});
      log_stage("local stage: active session started");
    }
    if (progress == ActiveStream::Progress::stopped) return stop();
    if (progress == ActiveStream::Progress::failed) {
      log_pipeline_failure(rp::LocalPipelineError::invalid_state);
      return unconfirmed_session_or(rp::StreamError::pipeline_failed,
                                    rp::StreamFailure::pipeline_failed);
    }
    if (auto reported = carry_out_progress(); !reported) return reported;
    if (const auto ready =
            stream_.stream_when_ready(launch_, "local stage: streaming ready");
        !ready)
      return stream_.fail(rp::StreamError::pipeline_failed,
                          rp::StreamFailure::pipeline_failed);
    stream_.keep_streaming();
    return {};
  }

  [[nodiscard]] auto unconfirmed_session_or(rp::StreamError error,
                                            rp::StreamFailure failure)
      -> Result {
    const UnconfirmedSession session{synthesized_session_id_};
    if (!session.starts_over(stream_.state()))
      return stream_.fail(error, failure);
    log_stage(
        "local stage: the console never confirmed the session; "
        "ending this attempt");
    return stream_.fail(session.failure(stream_.state(), error),
                        rp::StreamFailure::negotiation_failed);
  }

  auto peer_disconnected() noexcept -> bool {
    const auto* pipeline = stream_.pipeline();
    return pipeline && pipeline->active_failure() &&
           pipeline->active_failure()->cause ==
               rp::ActiveSessionError::peer_disconnected;
  }

  [[nodiscard]] auto queue_runtime_ctrl(const rp::CtrlMicrophoneFrame& frame)
      -> Result {
    if (!stream_.streaming() || !stream_.ctrl_open() || !ctrl_.session())
      return unexpected(rp::StreamError::invalid_state);
    if (!runtime_ctrl_.queue(frame.type, frame.bytes()))
      return unexpected(rp::StreamError::ctrl_failed);
    stream_.wake();
    return {};
  }

  [[nodiscard]] auto send_queued_pin(rp::LocalCtrlSessionId& ctrl_session)
      -> Result {
    if (ctrl_session.pin_pending() || ctrl_session.heartbeat_pending())
      return {};
    auto sent = ctrl_session.run_next_pin_reply(queued_pin_->bytes());
    if (!sent)
      return stream_.fail(rp::StreamError::ctrl_failed,
                          rp::StreamFailure::negotiation_failed);
    if (!sent.value()) return {};
    queued_pin_.reset();
    progress_.process(progress::PinSent{});
    ctrl_session.set_pin_prompt_open(false);
    wait_for_ctrl_answer();
    return {};
  }

  [[nodiscard]] auto make_fallback_id() -> std::optional<rp::SessionId> {
    SecretArray<rp::session_id_random_size> fallback_random;
    if (!random_.fill_bytes({fallback_random.data(), fallback_random.size()}))
      return std::nullopt;
    auto fallback = rp::SessionId::fallback(
        clock_.now_microseconds() / microseconds_per_second,
        {fallback_random.data(), fallback_random.size()});
    if (!fallback) return std::nullopt;
    return std::move(fallback.value());
  }

  [[nodiscard]] auto synthesize_fallback_id() -> Result {
    stream_.at(rp::FailureStage::ctrl_session_id);
    auto fallback = make_fallback_id();
    if (!fallback)
      return stream_.fail(rp::StreamError::ctrl_failed,
                          rp::StreamFailure::negotiation_failed);
    auto* ctrl_session = ctrl_.session();
    const auto* owner = ctrl_session ? ctrl_session->owner() : nullptr;
    if (owner &&
        owner->state() == rp::ctrl_lifecycle_detail::State::waiting_id) {
      if (!ctrl_session->publish_fallback(std::move(*fallback)))
        return stream_.fail(rp::StreamError::ctrl_failed,
                            rp::StreamFailure::negotiation_failed);
      receive_session_id(ctrl_session->take_ready_ticket());
    } else if (ctrl_nonce_) {
      rp::LocalCtrlReadyTicket ticket{std::move(*fallback), {}, 1};
      std::ranges::copy(*ctrl_nonce_, ticket.nonce.begin());
      receive_session_id(std::move(ticket));
      if (!ctrl_session) progress_.process(progress::SessionIdWithoutControl{});
    }
    if (!ready_ticket_)
      return stream_.fail(rp::StreamError::ctrl_failed,
                          rp::StreamFailure::negotiation_failed);
    synthesized_session_id_ = true;
    log_stage("local stage: Ctrl session ID fallback synthesized");
    stream_.trace().decision(
        diagnostics::Trace::Decision::synthetic_session_id);
    return {};
  }

  auto select_video(const rp::CtrlResponseFinal* ctrl_response)
      -> rp::LocalVideoSelection {
    const rp::LocalVideoSelection requested{
        configuration_.codec,
        {configuration_.video.width, configuration_.video.height,
         configuration_.video.max_fps, configuration_.video.bitrate_kbps}};
    const rp::CtrlResponseFinal unanswered{};
    const auto& response = ctrl_response ? *ctrl_response : unanswered;
    const auto selected = rp::LocalStart::apply_server_type(
        requested, response.known_server_type(),
        configuration_.video.auto_downgrade);
    if (response.prohibited)
      log_stage("local ctrl: console reports RP-Prohibit; continuing");
    if (!response.server_type &&
        response.server_type.error() == rp::ServerTypeError::undecryptable)
      log_stage("local ctrl: server type did not decrypt");
    else if (!response.server_type)
      log_stage("local ctrl: no valid server type");
    else
      log_stage(std::format(
          "local ctrl: server type {}; video {}x{}@{} {} kbps codec {}",
          *response.server_type, selected.video.width, selected.video.height,
          selected.video.max_fps, selected.video.bitrate_kbps,
          std::to_underlying(selected.codec)));
    return selected;
  }

  void log(platform::LogSeverity severity, std::string_view line) {
    if (platform_.supplies(platform::PlatformCapability::diagnostics))
      platform_.diagnostics().write({severity, line});
  }

  void log_stage(std::string_view stage) {
    log(platform::LogSeverity::info, stage);
  }

  void log_ctrl_failure(std::string_view stage) {
    log(platform::LogSeverity::error, stage);
  }

  void log_pipeline_failure(rp::LocalPipelineError error) {
    const auto phase = senkusha_sink_.last_phase
                           ? magic_enum::enum_name(*senkusha_sink_.last_phase)
                           : std::string_view{"none"};
    const auto cause = senkusha_sink_.last_error
                           ? magic_enum::enum_name(*senkusha_sink_.last_error)
                           : std::string_view{"none"};
    log(platform::LogSeverity::error,
        std::format("local pipeline: {}; Senkusha phase={}; Senkusha error={}",
                    magic_enum::enum_name(error), phase, cause));
  }

  [[nodiscard]] auto start_session_exchange() -> Result {
    ctrl_candidate_ = 0;
    return start_session_candidates();
  }

  [[nodiscard]] auto start_session_candidates() -> Result {
    for (;;) {
      const auto started = start_session_candidate();
      if (started || !session_connect_failed_ ||
          ctrl_candidate_ + 1 >= ctrl_candidates_.size())
        return started;
      ++ctrl_candidate_;
    }
  }

  [[nodiscard]] auto start_session_candidate() -> Result {
    session_connect_failed_ = false;
    if (ctrl_candidate_ < ctrl_candidates_.size()) {
      resolved_ctrl_endpoint_.address = ctrl_candidates_[ctrl_candidate_];
      if (route_follows_ctrl_)
        configuration_.route.remote.address = resolved_ctrl_endpoint_.address;
    }
    const auto host =
        platform::to_scoped_string(resolved_ctrl_endpoint_.address);
    auto request = rp::SessionCodec::encode_request(
        configuration_.console.target, host, resolved_ctrl_endpoint_.port,
        {configuration_.console.registration_key.data(),
         configuration_.console.registration_key.size()});
    if (!request) return unexpected(rp::StreamError::ctrl_failed);
    session_init_ = std::make_unique<rp::SessionTcpExchange>(
        platform_.network().sockets(), platform_.clock());
    const auto started = session_init_->begin(
        resolved_ctrl_endpoint_, std::move(request.value()),
        rp::SessionReply{configuration_.console.target});
    if (!started) {
      session_connect_failed_ = session_init_->failed_while_connecting();
      session_init_.reset();
      return unexpected(map_session_exchange_error(started.error()));
    }
    return {};
  }

  [[nodiscard]] auto start_ctrl(std::span<const std::byte> nonce)
      -> Expected<void, rp::LocalCtrlError> {
    const auto host =
        platform::to_scoped_string(resolved_ctrl_endpoint_.address);
    const rp::LocalCtrlAttempt::Config config{
        configuration_.console,       host,
        resolved_ctrl_endpoint_.port, configuration_.did,
        configuration_.codec,         nonce,
        resolved_ctrl_endpoint_,      1};
    return ctrl_.start(config);
  }

  HomeLinkConfiguration configuration_;
  platform::PlatformFactory& platform_;
  ActiveStream stream_;
  platform::CryptoProvider& crypto_;
  platform::ClockProvider& clock_;
  platform::SecureRandomProvider& random_;
  net::protocol::takion::PlatformTakionCryptoProvider takion_crypto_;
  rp::LocalCtrlAttempt ctrl_;
  rp::LocalPostIdCtrl post_id_ctrl_;
  std::optional<rp::event::EffectiveLaunch> launch_;
  rp::LocalRuntimeCtrl runtime_ctrl_;
  std::optional<rp::LocalCtrlReadyTicket> ready_ticket_;
  std::unique_ptr<rp::SessionTcpExchange> session_init_;
  std::optional<SecretArray<rp_nonce_size>> ctrl_nonce_;
  progress::Progress progress_;
  timing::Deadline ctrl_ready_;
  timing::Deadline ctrl_answer_due_;
  std::uint8_t session_retries_{};
  platform::SocketEndpoint resolved_ctrl_endpoint_{};
  std::vector<platform::IpAddress> ctrl_candidates_;
  std::size_t ctrl_candidate_{};
  bool route_follows_ctrl_{};
  bool session_connect_failed_{};
  bool synthesized_session_id_{};
  SenkushaSink senkusha_sink_;
  std::optional<SecretBytes> queued_pin_;
  unsigned ctrl_frames_logged_{};
  bool ctrl_logged_{};
  bool ctrl_backlog_{};
};

auto HomeLinkConfiguration::link(HomeLinkConfiguration configuration,
                                 platform::PlatformFactory& platform)
    -> Expected<std::unique_ptr<Link>, rp::StreamError> {
  if (!platform.supplies(platform::PlatformCapability::crypto))
    return unexpected(rp::StreamError::provider_unavailable);
  return std::unique_ptr<Link>{
      new HomeLink(std::move(configuration), platform, platform.crypto())};
}

}  // namespace libchiaking::play
