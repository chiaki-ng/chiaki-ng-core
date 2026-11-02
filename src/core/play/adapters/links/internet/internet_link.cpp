#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/remote_play/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"

#include "src/core/play/adapters/links/active_stream.hpp"

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl_effects.hpp"
#include "src/core/play/model/pin_lifecycle.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::play {

namespace rp = net::protocol::remote_play;

class InternetLink final : public Link,
                           private ActiveStream::Owner,
                           private play::InternetStart::RouteWatcher {
 public:
  InternetLink(InternetLinkConfiguration configuration,
               platform::PlatformFactory& platform, bool pairing_only,
               play::InternetStart::PairingListener* pairings)
      : Link(Venue::internet()),
        pairing_only_(pairing_only),
        configuration_(rp::LocalStreamPreparation::apply_disabled_video(
            std::move(configuration))),
        platform_(platform),
        stream_(*this, ending(), platform, configuration_.stop,
                {.component = diagnostics::Component::remote,
                 .console = playing_ps5()
                                ? Seats::Core::ConsoleKind::remote_play_ps5
                                : Seats::Core::ConsoleKind::remote_play_ps4,
                 .feedback = configuration_.feedback}),
        crypto_(platform.crypto()),
        clock_(platform.clock()),
        random_(platform.random()),
        takion_crypto_(crypto_, platform.threading()),
        internet_(
            platform, family_,
            {configuration_.enable_ipv6, configuration_.force_port_guessing,
             configuration_.port_guess_count,
             configuration_.nat_probe_socket_count,
             configuration_.start_timeout_us},
            configuration_.stop, stream_.trace(), pairings, this) {}

  ~InternetLink() override { static_cast<void>(release(false)); }

  [[nodiscard]] auto start() -> Result override {
    if (stream_.state() != rp::StreamState::idle)
      return unexpected(rp::StreamError::invalid_state);
    if (!stream_.has_sink() || !configuration_.user ||
        !reachable_configuration())
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
      return fail(rp::StreamError::route_failed, rp::StreamFailure::cancelled);
    const auto did = rp::LocalStreamPreparation::resolve_local_did(
        configuration_.did, random_);
    if (!did)
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed,
                  LinkFailure{rp::FailureStage::ctrl_startup,
                              rp::StreamError::ctrl_failed});
    configuration_.did = *did;
    if (!internet_.begin()) return unexpected(rp::StreamError::invalid_state);
    if (internet_.find_gateway() && configuration_.stop.stop_requested())
      return fail(rp::StreamError::route_failed, rp::StreamFailure::cancelled);
    if (!internet_.draw_secrets()) {
      log_start_failure("remote start: attempt material generation failed");
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed);
    }
    if (auto opened = open_account_session(); !opened) return opened;
    if (auto woken = wake_console(); !woken) return woken;
    trace_stage(rp::FailureStage::control_holepunch);
    auto ctrl_route = internet_.control_route();
    if (!ctrl_route) return fail_control_route(ctrl_route.error());
    return pair_and_connect(std::move(ctrl_route.value()));
  }

  [[nodiscard]] auto pump_once() -> Result override {
    if (!stream_.running()) return unexpected(rp::StreamError::invalid_state);
    if (configuration_.stop.stop_requested()) return stop();
    if (auto watched = watch_signalling(); !watched) return watched;
    stream_.observe();
    if (!stream_.ctrl_open()) return pump_pipeline();
    if (!ctrl_connected()) {
      log_start_failure("remote ctrl pump: missing connected owner");
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed);
    }
    CtrlChannel channel{*this};
    if (auto settled = apply_effects(channel.reliable(), "pre-receive"))
      return *settled;
    if (auto advanced = advance_after_ctrl()) return *advanced;
    trace_stage(rp::FailureStage::ctrl_receive);
    if (auto settled = receive_ctrl(channel.reliable())) return *settled;
    if (auto settled = apply_effects(channel.reliable(), "post-receive"))
      return *settled;
    if (auto kept = keep_ctrl_alive(channel.reliable())) return *kept;
    stream_.deliver_notices([&](auto& notices, auto& couch) {
      play::internet::remote_ctrl::Effects::drain_notices(
          *ctrl_->session, stream_.features(), notices, &couch);
    });
    if (!stream_.running()) return {};
    if (auto flushed = play::internet::remote_ctrl::Effects::flush_outbound(
            *ctrl_->session, channel.reliable(), crypto_,
            configuration_.console.target, stream_.features(),
            configuration_.enable_dualsense_ctrl,
            configuration_.enable_keyboard);
        !flushed) {
      if (stream_.streaming()) return tolerate_ctrl_trouble("feature send");
      log_start_failure("remote ctrl: feature send failed");
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed,
                  ctrl_outbound_failure(flushed.error(),
                                        rp::FailureStage::ctrl_receive));
    }
    ctrl_health_ = CtrlHealth::steady;
    if (auto advanced = advance_after_ctrl()) return *advanced;
    if (!stream_.has_media()) return {};
    if (stream_.streaming() && !display_devices_resent_)
      resend_display_devices();
    return pump_pipeline();
  }

  void resend_display_devices() {
    display_devices_resent_ = true;
    if (!stream_.ctrl_open() || !ctrl_connected()) return;
    CtrlChannel channel{*this};
    constexpr std::array<std::byte, 4> payload{};
    const auto sent = ctrl_->session->send_ctrl_frame(
        channel.reliable(), crypto_, configuration_.console.target,
        rp::CtrlCodec::message_type_id(rp::CtrlMessageType::display_devices),
        payload);
    log(platform::LogSeverity::debug,
        std::format("remote ctrl: display devices sent again once streaming: "
                    "{}",
                    sent ? "sent" : "failed"));
  }

  [[nodiscard]] auto stop() -> Result override { return stop_with(false); }

  [[nodiscard]] auto stop_for_sleep() -> Result override {
    return stop_with(true);
  }

  [[nodiscard]] auto wait(std::chrono::milliseconds timeout)
      -> Result override {
    const platform::Pollable* ctrl_pollable = nullptr;
    if (stream_.ctrl_open() && ctrl_ && ctrl_->connected.transport)
      ctrl_pollable = ctrl_->connected.transport->pollable();
    return stream_.wait(timeout, {ctrl_pollable});
  }

  [[nodiscard]] auto connect_microphone() -> Result override {
    return send_runtime_ctrl(rp::CtrlCodec::microphone_connect_frame());
  }

  [[nodiscard]] auto set_microphone_muted(bool muted) -> Result override {
    return send_runtime_ctrl(rp::CtrlCodec::microphone_mute_frame(muted));
  }

  [[nodiscard]] auto submit_pin(std::span<const std::byte> pin)
      -> Result override {
    if (stream_.ended() || pin.empty() ||
        pin.size() > rp::ctrl_frame_max_payload)
      return unexpected(rp::StreamError::pin_rejected);
    queued_pin_.reset();
    queued_pin_.emplace(pin);
    if (stream_.state() != rp::StreamState::establishing_route ||
        !pin_.asked() || !ctrl_connected())
      return {};
    CtrlChannel channel{*this};
    return send_queued_pin(channel.reliable());
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
  class CtrlChannel {
   public:
    explicit CtrlChannel(InternetLink& link)
        : transport_{*link.ctrl_->connected.transport},
          reliable_{transport_, link.ctrl_->session->send_buffer(), link.clock_,
                    link.platform_.supplies(
                        platform::PlatformCapability::diagnostics)
                        ? &link.platform_.diagnostics()
                        : nullptr} {}

    auto reliable() noexcept -> net::transport::rudp::ReliableChannel& {
      return reliable_;
    }

   private:
    net::transport::rudp::ProviderRudpHandshakeChannel transport_;
    net::transport::rudp::ReliableChannel reliable_;
  };

  class SenkushaSink final : public net::protocol::senkusha::EventSink {
   public:
    void on_event(net::protocol::senkusha::Event) override {}
  };

  static constexpr std::uint64_t attempt_id_ =
      play::InternetStart::this_attempt;

  static auto ctrl_outbound_failure(
      play::internet::remote_ctrl::OutboundStepError error,
      rp::FailureStage stage) -> LinkFailure {
    return LinkFailure{stage, rp::StreamError::ctrl_failed, error};
  }

  auto playing_ps5() const noexcept -> bool {
    return configuration_.console.target == rp::RegistrationTarget::ps5_1;
  }

  auto reachable_configuration() const noexcept -> bool {
    const bool target =
        configuration_.console.target == rp::RegistrationTarget::ps5_1 ||
        configuration_.console.target == rp::RegistrationTarget::ps4_10;
    return target &&
           (configuration_.host.empty() || configuration_.ctrl_port != 0) &&
           configuration_.start_timeout_us != 0 &&
           configuration_.port_guess_count != 0 &&
           configuration_.port_guess_count <=
               net::nat::holepunch::CandidateGathering::max_port_guess_count &&
           configuration_.nat_probe_socket_count <= rp::max_nat_probe_sockets;
  }

  auto active_stream() noexcept -> ActiveStream* override { return &stream_; }
  auto diagnostics() const noexcept
      -> net::protocol::remote_play::StreamDiagnostics override {
    auto health = Link::diagnostics();
    if (last_ctrl_trouble_us_ &&
        (!health.network_trouble_us ||
         *health.network_trouble_us < *last_ctrl_trouble_us_))
      health.network_trouble_us = last_ctrl_trouble_us_;
    return health;
  }

  auto active_stream() const noexcept -> const ActiveStream* override {
    return &stream_;
  }

  auto ctrl_connected() const noexcept -> bool {
    return ctrl_ && ctrl_->session && ctrl_->connected.transport;
  }

  [[nodiscard]] auto open_account_session() -> Result {
    trace_stage(rp::FailureStage::psn_open,
                progress::InternetRoute{
                    progress::InternetRoute::Step::opening_account_session});
    if (auto access = internet_.take_access(*configuration_.user); !access) {
      log_start_failure("remote start: PSN open failed");
      return fail(
          rp::StreamError::route_failed,
          rp::StreamFailure::route_establishment_failed,
          LinkFailure{rp::FailureStage::psn_open, rp::StreamError::route_failed,
                      PsnFailure{access.error()}});
    }
    auto opened = stream_.trace().observe_stage(
        rp::FailureStage::psn_open,
        [&] { return internet_.open_account_session(); });
    if (!opened) return fail_account_session(opened.error());
    if (auto signalling = internet_.listen(); !signalling) {
      return fail(
          rp::StreamError::route_failed,
          rp::StreamFailure::route_establishment_failed,
          LinkFailure{rp::FailureStage::psn_open, rp::StreamError::route_failed,
                      signalling.error()});
    }
    return {};
  }

  [[nodiscard]] auto fail_account_session(
      const psn::remote_play::StartError& error) -> Result {
    log_start_failure("remote start: PSN open failed");
    using E = psn::remote_play::StartError;
    const auto* wait = error.as<E::WaitFailed>();
    const auto* create = error.as<E::CreateFailed>();
    if (wait)
      log(wait->deletion_confirmed ? platform::LogSeverity::info
                                   : platform::LogSeverity::warning,
          wait->release_skipped
              ? "remote start: PSN push closed; skipping membership DELETE"
          : wait->deletion_confirmed
              ? "remote start: PSN membership deletion observed"
              : "remote start: PSN deletion unconfirmed");
    const auto close_failure = wait     ? wait->close_failure
                               : create ? create->close_failure
                                        : std::nullopt;
    if (close_failure)
      ending().record_cleanup_failure(LinkFailure::signalling_lost(
          rp::FailureStage::shutdown,
          {psn::remote_play::PushErrorKind::websocket_failure,
           *close_failure}));
    if (wait && wait->release_failure)
      ending().record_cleanup_failure(LinkFailure{
          rp::FailureStage::shutdown, rp::StreamError::shutdown_failed,
          PsnFailure{*wait->release_failure}});
    return fail(rp::StreamError::route_failed,
                rp::StreamFailure::route_establishment_failed,
                LinkFailure{rp::FailureStage::psn_open,
                            rp::StreamError::route_failed, PsnFailure{error}});
  }

  [[nodiscard]] auto wake_console() -> Result {
    trace_stage(rp::FailureStage::control_holepunch,
                progress::InternetRoute{
                    progress::InternetRoute::Step::gathering_candidates});
    if (auto prepared = internet_.prepare_control_route(); !prepared)
      return fail_control_route(prepared.error());
    trace_stage(rp::FailureStage::psn_start,
                progress::InternetRoute{
                    progress::InternetRoute::Step::waiting_for_console});
    if (auto ready = internet_.wake(configuration_.device_uid); !ready) {
      const auto& error = ready.error();
      log_start_failure(
          std::format("remote start: PSN start failed: {} (http={})",
                      error.describe(), error.http_status()));
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed,
                  LinkFailure{rp::FailureStage::psn_start,
                              rp::StreamError::route_failed, error});
    }
    if (!internet_.control_route_is_next())
      return fail(rp::StreamError::invalid_state,
                  rp::StreamFailure::route_establishment_failed);
    return {};
  }

  [[nodiscard]] auto pair_and_connect(
      play::internet::holepunch::CtrlRudpHandoff handoff) -> Result {
    net::transport::rudp::ProviderRudpHandshakeChannel channel{
        *handoff.transport};
    trace_stage(rp::FailureStage::registration);
    auto registered =
        stream_.trace().observe_stage(rp::FailureStage::registration, [&] {
          return internet_.pair(channel, crypto_, clock_,
                                configuration_.console.target,
                                configuration_.user->account_id().bytes(),
                                handoff.registration_local_address);
        });
    if (!registered) return fail_registration(registered.error());
    if (!internet_.keep(registered.value().console,
                        configuration_.console.target))
      return fail_unpaired_console();
    configuration_.console = std::move(registered.value().console);
    if (pairing_only_) {
      if (!emit_registered()) return {};
      return stream_.stopped(release(false));
    }
    if (!emit_nickname()) return {};
    const bool own_host = configuration_.host.empty();
    const auto ctrl_host =
        own_host ? platform::to_string(handoff.peer_endpoint.address)
                 : configuration_.host;
    const auto ctrl_port =
        own_host ? handoff.peer_endpoint.port : configuration_.ctrl_port;
    trace_stage(rp::FailureStage::session_init);
    auto initialized =
        stream_.trace().observe_stage(rp::FailureStage::session_init, [&] {
          return rp::RemoteSessionInit::run(
              channel,
              {registered.value().next_local_counter,
               registered.value().local_header},
              clock_, configuration_.stop, attempt_id_, configuration_.console,
              ctrl_host, ctrl_port);
        });
    if (!initialized) {
      log_start_failure("remote start: session-init failed");
      auto refused = LinkFailure::refused(initialized.error());
      const auto error = refused.result();
      return fail(error, rp::StreamFailure::negotiation_failed,
                  std::move(refused));
    }
    if (initialized.value().target != configuration_.console.target) {
      stream_.trace().record(diagnostics::Operation::local_version_retry,
                             diagnostics::Outcome::retry,
                             static_cast<unsigned>(initialized.value().target));
      configuration_.console.target = initialized.value().target;
    }
    const auto& nonce = initialized.value().response.nonce();
    net::transport::rudp::Connected registered_rudp{
        std::move(handoff.transport), initialized.value().completion};
    stream_.idle(std::chrono::milliseconds{rp::wait_slice_ms});
    trace_stage(rp::FailureStage::ctrl_startup);
    auto connected =
        stream_.trace().observe_stage(rp::FailureStage::ctrl_startup, [&] {
          return play::internet::remote_ctrl::Connector::connect_registered(
              std::move(registered_rudp), platform_, crypto_,
              configuration_.console, ctrl_host, ctrl_port, configuration_.did,
              selected_codec(), {nonce.data(), nonce.size()},
              configuration_.stop, attempt_id_);
        });
    if (!connected) {
      log_start_failure("remote start: Ctrl connection failed");
      auto refused = LinkFailure::refused(connected.error());
      const auto error = refused.result();
      return fail(error, rp::StreamFailure::negotiation_failed,
                  std::move(refused));
    }
    ctrl_.emplace(std::move(connected.value()));
    restart_ctrl_wait();
    pin_.rearm();
    stream_.enter(rp::StreamState::establishing_route);
    if (stream_.state() == rp::StreamState::establishing_route &&
        ctrl_->session)
      stream_.prohibit(ctrl_->session->rp_prohibited());
    return {};
  }

  [[nodiscard]] auto fail_registration(
      const play::internet::PsnRemoteRegistrationFailure& error) -> Result {
    log_start_failure("remote start: RUDP registration failed");
    return fail(rp::StreamError::route_failed,
                rp::StreamFailure::route_establishment_failed,
                LinkFailure::unpaired(error));
  }

  [[nodiscard]] auto fail_unpaired_console() -> Result {
    log_start_failure(
        "remote start: PSN reached a PS4 the player has not paired");
    EndReason::Facts facts;
    facts.step = EndReason::Step::pairing;
    return fail(rp::StreamError::invalid_state,
                rp::StreamFailure::route_establishment_failed,
                LinkFailure::reported(
                    EndReason{EndReason::Cause::unpaired_console, facts}));
  }

  [[nodiscard]] auto watch_signalling() -> Result {
    const auto error = internet_.signalling_failure();
    if (!error || internet_.data_route_is_planned()) return {};
    if (!data_accepted_)
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed,
                  LinkFailure::signalling_lost(stream_.stage(), *error));
    if (!std::exchange(push_failure_logged_, true))
      log_start_failure(
          "remote: PSN push lost after the DATA accept; continuing");
    return {};
  }

  [[nodiscard]] auto apply_effects(
      net::transport::rudp::ReliableChannel& channel, std::string_view when)
      -> std::optional<Result> {
    using Progress = play::internet::remote_ctrl::EffectProgress;
    auto effects = play::internet::remote_ctrl::Effects::step(
        *ctrl_->session, channel, crypto_, configuration_.console.target,
        pin_.asked());
    if (effects && effects.value() == Progress::pin_required) pin_.ask();
    if (!effects) {
      if (stream_.streaming()) return tolerate_ctrl_trouble("effect step");
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed,
                  ctrl_outbound_failure(effects.error(),
                                        rp::FailureStage::ctrl_receive));
    }
    switch (effects.value()) {
      case Progress::failed:
        if (stream_.streaming()) return tolerate_ctrl_trouble(when);
        log_start_failure(
            std::format("remote ctrl pump: {} effect failed", when));
        return fail(rp::StreamError::ctrl_failed,
                    rp::StreamFailure::negotiation_failed);
      case Progress::ctrl_failed:
        if (auto tolerated = on_ctrl_failed_while_active(); !tolerated)
          return tolerated;
        break;
      case Progress::fallback_required:
        if (auto fallback = synthesize_fallback_id(); !fallback)
          return fallback;
        break;
      case Progress::pin_required:
        return prompt_for_pin(channel);
      case Progress::idle:
        break;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto advance_after_ctrl() -> std::optional<Result> {
    if (!ready_ticket_) ready_ticket_ = ctrl_->session->take_ready_ticket();
    if (stream_.has_media() || pin_.asked()) return std::nullopt;
    auto advanced = advance_startup();
    if (!advanced || stream_.has_media()) return advanced;
    return std::nullopt;
  }

  [[nodiscard]] auto receive_ctrl(
      net::transport::rudp::ReliableChannel& channel) -> std::optional<Result> {
    auto received = ctrl_->session->receive_once(
        channel, crypto_, configuration_.console.target,
        std::chrono::microseconds{rp::ctrl_receive_poll_us}, {});
    const auto& evidence = ctrl_->session->pump_state().last_receive;
    if (!received &&
        received.error() != play::internet::remote_ctrl::PumpError::timeout) {
      if (stream_.streaming() && ctrl_health_ == CtrlHealth::troubled &&
          !ends_ctrl(received.error()))
        return tolerate_ctrl_trouble("receive");
      const LinkFailure read{
          rp::FailureStage::ctrl_receive, rp::StreamError::ctrl_failed,
          LinkFailure::RemoteCtrlRead{received.error(), evidence.packet_bytes,
                                      evidence.framing_error}};
      log_start_failure(std::format("remote ctrl pump: {}", read.describe()));
      log_start_failure(std::format(
          "remote ctrl receive: bytes={} first_type=0x{:04x} "
          "frames={} framing_error={}",
          evidence.packet_bytes, static_cast<unsigned>(evidence.first_type),
          evidence.frame_count,
          evidence.framing_error ? static_cast<int>(*evidence.framing_error)
                                 : -1));
      if (stream_.streaming()) {
        if (received.error() ==
            play::internet::remote_ctrl::PumpError::inbox_rejected)
          return pump_pipeline();
        if (ends_ctrl(received.error())) return suspend_ctrl();
        return tolerate_ctrl_trouble("receive");
      }
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed, read);
    }
    if (received) log_ctrl_counters(evidence);
    if (received && evidence.transport_only_frames != 0)
      log(platform::LogSeverity::info,
          std::format(
              "remote ctrl: ACKed transport-only envelopes without consuming "
              "Ctrl cipher "
              "state; bytes={} first_type=0x{:04x} frames={} transport_only={}",
              evidence.packet_bytes, static_cast<unsigned>(evidence.first_type),
              evidence.frame_count, evidence.transport_only_frames));
    return std::nullopt;
  }

  [[nodiscard]] auto prompt_for_pin(
      net::transport::rudp::HandshakeChannel& channel) -> Result {
    stream_.emit(rp::event::PinRequired{pin_.prompt()});
    if (!queued_pin_ ||
        stream_.state() != rp::StreamState::establishing_route ||
        !pin_.asked() || !ctrl_ || !ctrl_->session)
      return {};
    return send_queued_pin(channel);
  }

  [[nodiscard]] auto send_queued_pin(
      net::transport::rudp::HandshakeChannel& channel) -> Result {
    auto sent = ctrl_->session->send_pin_once(
        channel, crypto_, configuration_.console.target, queued_pin_->bytes());
    queued_pin_.reset();
    if (!sent) {
      if (sent.error() ==
              play::internet::remote_ctrl::OutboundStepError::invalid_pin ||
          sent.error() == play::internet::remote_ctrl::OutboundStepError::
                              unsupported_effect)
        return unexpected(rp::StreamError::pin_rejected);
      return fail(
          rp::StreamError::ctrl_failed, rp::StreamFailure::negotiation_failed,
          ctrl_outbound_failure(sent.error(), rp::FailureStage::ctrl_receive));
    }
    pin_.withdraw();
    ctrl_->session->inbox().set_pin_prompt_open(false);
    restart_ctrl_wait();
    return {};
  }

  [[nodiscard]] auto keep_ctrl_alive(
      net::transport::rudp::ReliableChannel& channel) -> std::optional<Result> {
    if (!stream_.streaming()) return std::nullopt;
    const auto now = clock_.now_microseconds();
    if (!ctrl_keepalive_us_) {
      ctrl_keepalive_us_ = now;
      return std::nullopt;
    }
    if (now - *ctrl_keepalive_us_ < rp::ctrl_keepalive_interval_us)
      return std::nullopt;
    ctrl_keepalive_us_ = now;
    if (auto sent = ctrl_->session->send_keepalive(
            channel, crypto_, configuration_.console.target);
        !sent)
      return tolerate_ctrl_trouble("keepalive");
    return std::nullopt;
  }

  void log_ctrl_counters(
      const net::protocol::remote_play::RudpCtrlReceiveEvidence& evidence) {
    bool packet_compared = false;
    for (const auto& [subtype, counter] : evidence.frames_seen) {
      const bool control = subtype != net::transport::rudp::Subtype::ack;
      const bool repeated =
          control && !packet_compared && last_ctrl_counter_ &&
          static_cast<std::uint16_t>(counter - *last_ctrl_counter_) - 1U >=
              0x7fffU;
      if (control) packet_compared = true;
      log(platform::LogSeverity::debug,
          std::format("remote ctrl frame: subtype=0x{:02x} counter=0x{:04x}{}",
                      static_cast<unsigned>(subtype),
                      static_cast<unsigned>(counter),
                      repeated ? " not newer than the last" : ""));
      if (control) last_ctrl_counter_ = counter;
    }
  }

  [[nodiscard]] static auto ends_ctrl(
      play::internet::remote_ctrl::PumpError error) noexcept -> bool {
    using E = play::internet::remote_ctrl::PumpError;
    return error == E::stopped || error == E::peer_closed ||
           error == E::counter_exhausted || error == E::stale_attempt;
  }

  [[nodiscard]] auto tolerate_ctrl_trouble(std::string_view what) -> Result {
    last_ctrl_trouble_us_ = clock_.now_microseconds();
    if (ctrl_health_ == CtrlHealth::steady) {
      ctrl_health_ = CtrlHealth::troubled;
      log_start_failure(std::format(
          "remote ctrl: {} failed while streaming; keeping Ctrl open", what));
    }
    return pump_pipeline();
  }

  [[nodiscard]] auto suspend_ctrl() -> Result {
    stream_.close_ctrl();
    log_start_failure(
        "remote: Ctrl lost while streaming; continuing without Ctrl");
    return pump_pipeline();
  }

  [[nodiscard]] auto on_ctrl_failed_while_active() -> Result {
    if (stream_.streaming() || stream_.active()) return {};
    if (stream_.has_media()) {
      ctrl_failed_before_stream_ = true;
      return {};
    }
    return fail(rp::StreamError::ctrl_failed,
                rp::StreamFailure::negotiation_failed);
  }

  [[nodiscard]] auto pump_pipeline() -> Result {
    trace_stage(rp::FailureStage::calibration);
    const auto progress = stream_.pump_media();
    if (progress == ActiveStream::Progress::failed && stream_.pump_error())
      return fail(rp::StreamError::pipeline_failed,
                  rp::StreamFailure::pipeline_failed);
    if (ctrl_failed_before_stream_ && stream_.active()) {
      ctrl_failed_before_stream_ = false;
      log_start_failure(
          "remote ctrl: failed before the stream connection started");
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed,
                  LinkFailure{rp::FailureStage::ctrl_receive,
                              rp::StreamError::ctrl_failed});
    }
    if (progress == ActiveStream::Progress::stopped) return stop();
    if (progress == ActiveStream::Progress::failed)
      return fail(rp::StreamError::pipeline_failed,
                  rp::StreamFailure::pipeline_failed);
    const auto launch = rp::LocalStart::effective_launch(
        video_selection_.value_or(requested_video()));
    if (const auto ready = stream_.stream_when_ready(launch); !ready)
      return fail(rp::StreamError::pipeline_failed,
                  rp::StreamFailure::pipeline_failed);
    stream_.keep_streaming();
    return {};
  }

  [[nodiscard]] auto stop_with(bool close_push_first) -> Result {
    if (!stream_.stopping()) return {};
    return stream_.stopped(release(close_push_first));
  }

  [[nodiscard]] auto send_runtime_ctrl(const rp::CtrlMicrophoneFrame& frame)
      -> Result {
    if (!stream_.streaming() || !stream_.ctrl_open() || !ctrl_connected())
      return unexpected(rp::StreamError::invalid_state);
    CtrlChannel channel{*this};
    if (!ctrl_->session->send_ctrl_frame(channel.reliable(), crypto_,
                                         configuration_.console.target,
                                         frame.type, frame.bytes()))
      return unexpected(rp::StreamError::ctrl_failed);
    return {};
  }

  void trace_stage(
      rp::FailureStage stage,
      std::optional<progress::InternetRoute> route = std::nullopt) {
    stream_.at(stage);
    const auto elapsed = since_start();
    const auto bit = std::uint64_t{1} << std::to_underlying(stage);
    if ((traced_stages_ & bit) != 0) return;
    traced_stages_ |= bit;
    stream_.emit(rp::event::SetupProgress{stage, elapsed, {}, route});
  }

  void on_route(const progress::InternetRoute& route) override {
    using Stage = rp::FailureStage;
    const auto stage =
        route.step == progress::InternetRoute::Step::switching_control
            ? Stage::route_switch
        : route.purpose == progress::InternetRoute::Purpose::stream
            ? Stage::data_holepunch
            : Stage::control_holepunch;
    stream_.emit(rp::event::SetupProgress{stage, since_start(), {}, route});
  }

  auto since_start() -> std::uint64_t {
    const auto now = timing::Deadline::now(platform_.clock());
    if (!trace_started_) trace_started_ = now;
    return timing::elapsed(*trace_started_, now).count();
  }

  void log(platform::LogSeverity severity, std::string_view line) {
    if (platform_.supplies(platform::PlatformCapability::diagnostics))
      platform_.diagnostics().write({severity, line});
  }

  void log_start_failure(std::string_view stage) {
    log(platform::LogSeverity::error, stage);
  }

  auto selected_codec() const noexcept -> rp::VideoCodec {
    return family_ == psn::rest::ConsoleFamily::ps4 ? rp::VideoCodec::h264
                                                    : configuration_.codec;
  }

  auto requested_video() const noexcept -> rp::LocalVideoSelection {
    return {selected_codec(),
            {configuration_.video.width, configuration_.video.height,
             configuration_.video.max_fps, configuration_.video.bitrate_kbps}};
  }

  void restart_ctrl_wait() {
    ctrl_wait_started_ = timing::Deadline::now(platform_.clock());
  }

  auto waited_for_ctrl(std::uint64_t patience_us) const -> bool {
    return timing::Deadline::after(ctrl_wait_started_,
                                   timing::Deadline::Duration{patience_us})
        .expired(platform_.clock());
  }

  [[nodiscard]] auto synthesize_fallback_id() -> Result {
    if (ready_ticket_ || stream_.has_media()) return {};
    trace_stage(rp::FailureStage::ctrl_session_id);
    SecretArray<rp::session_id_random_size> fallback_random;
    if (!random_.fill_bytes({fallback_random.data(), fallback_random.size()}))
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed);
    auto fallback = rp::SessionId::fallback(
        clock_.now_microseconds() / microseconds_per_second,
        {fallback_random.data(), fallback_random.size()});
    if (!fallback || !ctrl_->session->inbox().publish_fallback(
                         attempt_id_, std::move(fallback.value())))
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed);
    ready_ticket_ = ctrl_->session->take_ready_ticket();
    if (!ready_ticket_)
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed);
    log(platform::LogSeverity::info,
        "remote ctrl: session ID fallback synthesized");
    stream_.trace().decision(
        diagnostics::Trace::Decision::synthetic_session_id);
    return {};
  }

  [[nodiscard]] auto advance_startup() -> Result {
    if (stream_.has_media() || pin_.asked()) return {};
    if (!pending_data_route_) {
      if (internet_.data_route_is_planned()) return finish_data_route();
      if (!ready_ticket_ && !waited_for_ctrl(rp::ctrl_start_wait_us)) return {};
      return start_data_route();
    }
    if (!ready_ticket_) {
      if (!waited_for_ctrl(rp::session_id_wait_us)) return {};
      if (auto fallback = synthesize_fallback_id(); !fallback) return fallback;
    }
    return start_data_pipeline();
  }

  [[nodiscard]] auto send_post_id_features() -> Result {
    CtrlChannel channel{*this};
    if (auto sent = ctrl_->session->send_post_id_features(
            channel.reliable(), crypto_, configuration_.console.target,
            configuration_.enable_dualsense_ctrl,
            configuration_.enable_keyboard);
        !sent)
      return fail(rp::StreamError::ctrl_failed,
                  rp::StreamFailure::negotiation_failed,
                  ctrl_outbound_failure(sent.error(), stream_.stage()));
    std::string types;
    for (const auto& frame :
         rp::CtrlCodec::post_id_frames(configuration_.enable_dualsense_ctrl,
                                       configuration_.enable_keyboard))
      types += std::format(" 0x{:04x}", static_cast<unsigned>(frame.type));
    log(platform::LogSeverity::debug,
        std::format("remote ctrl features sent:{} keyboard={}", types,
                    configuration_.enable_keyboard));
    return {};
  }

  [[nodiscard]] auto start_data_route() -> Result {
    if (ctrl_ && ctrl_->session) ctrl_->session->inbox().close_pin_window();
    if (!internet_.data_route_is_next())
      return fail(rp::StreamError::invalid_state,
                  rp::StreamFailure::route_establishment_failed);
    if (!ctrl_connected() || !internet_.is_open() || !internet_.has_secrets())
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed);
    if (ready_ticket_)
      if (auto sent = send_post_id_features(); !sent) return sent;
    trace_stage(rp::FailureStage::data_holepunch,
                progress::InternetRoute{
                    progress::InternetRoute::Step::gathering_candidates,
                    progress::InternetRoute::Purpose::stream});
    internet_.plan_data_route();
    stream_.emit(rp::event::Holepunch{false});
    if (stream_.ended()) {
      internet_.drop_data_route();
      return {};
    }
    if (auto launched = internet_.launch_data_route(); !launched) {
      log_start_failure("remote DATA route: worker creation failed");
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed,
                  LinkFailure{stream_.stage(), rp::StreamError::route_failed,
                              launched.error()});
    }
    return finish_data_route();
  }

  [[nodiscard]] auto finish_data_route() -> Result {
    auto completion = internet_.take_data_route();
    if (!completion) return {};
    auto candidate = std::move(completion->result);
    if (completion->worker_error) {
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed,
                  LinkFailure{rp::FailureStage::data_holepunch,
                              rp::StreamError::route_failed,
                              *completion->worker_error});
    }
    if (completion->attempt_id != attempt_id_) {
      if (candidate && candidate.value().transport)
        static_cast<void>(candidate.value().transport.close());
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed);
    }
    if (!candidate) return fail_data_route(candidate.error());
    data_accepted_ = true;
    stream_.emit(rp::event::Holepunch{true});
    if (!internet_.routes_are_ready(completion->attempt_id))
      return fail(rp::StreamError::invalid_state,
                  rp::StreamFailure::route_establishment_failed);
    pending_data_route_ = std::move(candidate.value());
    restart_ctrl_wait();
    return {};
  }

  [[nodiscard]] auto fail_data_route(const RouteFailure& error) -> Result {
    return fail(rp::StreamError::route_failed,
                rp::StreamFailure::route_establishment_failed,
                LinkFailure{rp::FailureStage::data_holepunch,
                            rp::StreamError::route_failed, error});
  }

  auto calibration_inputs() -> rp::LocalCalibrationInputs {
    rp::LocalCalibrationInputs inputs{
        {clock_, platform_.threading(), random_, crypto_, takion_crypto_,
         configuration_.console, video_selection_->codec,
         video_selection_->video},
        senkusha_sink_,
        {}};
    inputs.pipeline.trace = &stream_.trace();
    inputs.pipeline.dualsense = configuration_.enable_dualsense_ctrl;
    inputs.pipeline.pad_count = stream_.couch().session_pad_count();
    inputs.pipeline.enable_idr_on_fec_failure =
        configuration_.enable_idr_on_fec_failure;
    inputs.pipeline.tuning = configuration_.tuning;
    inputs.pipeline.retained_route_fallback =
        rp::LocalStreamPreparation::make_reference_fallback_calibration(
            configuration_.console.target,
            configuration_.tuning.takion_version_override);
    inputs.pipeline.transition_route = [this](rp::EstablishedRoute calibrated) {
      return switch_route(std::move(calibrated));
    };
    return inputs;
  }

  [[nodiscard]] auto start_data_pipeline() -> Result {
    if (!ready_ticket_ || !internet_.is_open() || !internet_.has_secrets() ||
        !ctrl_connected() || !pending_data_route_ ||
        !pending_data_route_->transport)
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed);
    if (auto sent = send_post_id_features(); !sent) return sent;
    video_selection_ = rp::LocalStart::apply_server_type(
        requested_video(), ctrl_->session->server_type(),
        configuration_.video.auto_downgrade);
    auto inputs = calibration_inputs();
    auto candidate = std::move(*pending_data_route_);
    pending_data_route_.reset();
    if (auto drained = net::transport::rudp::RemoteDatagramDrain::drain(
            *candidate.transport, platform_.network(), platform_.clock(),
            configuration_.stop);
        !drained) {
      static_cast<void>(candidate.transport.close());
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed,
                  LinkFailure{rp::FailureStage::data_holepunch,
                              rp::StreamError::route_failed, drained.error()});
    }
    stream_.route_through(candidate.transport ? candidate.transport->pollable()
                                              : nullptr);
    auto started = rp::LocalCalibratedPipeline::start_uncalibrated(
        std::move(candidate), std::move(*ready_ticket_),
        std::move(inputs.pipeline));
    ready_ticket_.reset();
    if (!started) {
      if (const auto& switch_failure = stream_.transition_failure())
        return fail(rp::StreamError::pipeline_failed,
                    rp::StreamFailure::route_establishment_failed,
                    *switch_failure);
      return fail(rp::StreamError::route_failed,
                  rp::StreamFailure::route_establishment_failed);
    }
    stream_.calibrate(std::move(started.value()));
    stream_.enter(rp::StreamState::negotiating_stream);
    return {};
  }

  auto switch_route(rp::EstablishedRoute calibrated)
      -> Expected<rp::EstablishedRoute, rp::LocalPipelineError> {
    if (!ctrl_connected() || !calibrated.transport ||
        calibrated.kind != rp::RouteKind::remote_rudp)
      return unexpected(rp::LocalPipelineError::route_unavailable);
    on_route({progress::InternetRoute::Step::switching_control,
              progress::InternetRoute::Purpose::stream});
    net::transport::rudp::ProviderRudpHandshakeChannel channel{
        *ctrl_->connected.transport};
    const auto& state = ctrl_->session->pump_state();
    rp::RudpCtrlSwitchStepInput input{crypto_,
                                      configuration_.console.target,
                                      ctrl_->session->material(),
                                      state.remote_crypt_counter,
                                      state.local_counter,
                                      attempt_id_,
                                      ctrl_->session->byte_stream()};
    const rp::RudpSwitchServices services{
        platform_.random(), clock_, configuration_.stop,
        ctrl_->session->send_buffer(),
        platform_.supplies(platform::PlatformCapability::diagnostics)
            ? &platform_.diagnostics()
            : nullptr};
    const rp::RudpSwitchHooks hooks{
        [this](rp::RudpCtrlSwitchStepResult&& message) {
          return ctrl_ && ctrl_->session &&
                 ctrl_->session->accept_acked_switch_step(std::move(message));
        },
        [this](net::transport::rudp::HandshakeChannel& active_channel,
               std::uint16_t next_counter) {
          return service_ctrl_during_switch(active_channel, next_counter);
        }};
    auto switched = rp::RudpDataSwitch::switch_route(
        std::move(calibrated), channel, input, services, hooks);
    if (!switched) {
      if (!stream_.transition_failure()) {
        stream_.note_transition_failure(
            LinkFailure{rp::FailureStage::route_switch,
                        rp::StreamError::pipeline_failed, switched.error()});
      }
      return unexpected(rp::LocalPipelineError::route_unavailable);
    }
    const auto& barrier = switched.value().barrier;
    if (!ctrl_->session->apply_switch_counters(
            barrier.attempt_id, barrier.next_remote_crypt_counter,
            barrier.next_local_counter))
      return unexpected(rp::LocalPipelineError::route_unavailable);
    auto& route = switched.value().route;
    if (route.transport) {
      if (auto drained = net::transport::rudp::RemoteDatagramDrain::drain(
              *route.transport, platform_.network(), platform_.clock(),
              configuration_.stop);
          !drained) {
        stream_.note_transition_failure(LinkFailure{
            rp::FailureStage::route_switch, rp::StreamError::pipeline_failed,
            LinkFailure::SwitchDrain{drained.error()}});
        static_cast<void>(route.transport.close());
        return unexpected(rp::LocalPipelineError::route_unavailable);
      }
    }
    stream_.route_through(route.transport ? route.transport->pollable()
                                          : nullptr);
    return std::move(route);
  }

  auto service_ctrl_during_switch(
      net::transport::rudp::HandshakeChannel& channel,
      std::uint16_t next_counter) -> std::optional<std::uint16_t> {
    using Progress = play::internet::remote_ctrl::EffectProgress;
    if (!ctrl_->session->apply_switch_counters(
            attempt_id_, ctrl_->session->pump_state().remote_crypt_counter,
            next_counter))
      return std::nullopt;
    auto effects = play::internet::remote_ctrl::Effects::step(
        *ctrl_->session, channel, crypto_, configuration_.console.target,
        pin_.asked());
    if (effects && effects.value() == Progress::pin_required) pin_.ask();
    if (!effects) {
      stream_.note_transition_failure(ctrl_outbound_failure(
          effects.error(), rp::FailureStage::route_switch));
      return std::nullopt;
    }
    if (effects.value() != Progress::idle &&
        effects.value() != Progress::ctrl_failed)
      return std::nullopt;
    return ctrl_->session->outbound_state().local_rudp_counter;
  }

  [[nodiscard]] auto fail_control_route(const RouteFailure& error) -> Result {
    log_start_failure(std::format(
        "remote start: control route hole punch failed: {}", error.describe()));
    return fail(rp::StreamError::route_failed,
                rp::StreamFailure::route_establishment_failed,
                LinkFailure{rp::FailureStage::control_holepunch,
                            rp::StreamError::route_failed, error});
  }

  [[nodiscard]] auto release_route() -> bool override { return release(false); }

  [[nodiscard]] auto release(bool close_push_first) -> bool {
    bool cleaned = push_closed(internet_.stop_signalling(close_push_first));
    cleaned = stream_.release_media() && cleaned;
    ready_ticket_.reset();
    pin_.withdraw();
    queued_pin_.reset();
    if (pending_data_route_ && pending_data_route_->transport &&
        !pending_data_route_->transport.close())
      cleaned = false;
    pending_data_route_.reset();
    ctrl_.reset();
    const auto left = internet_.leave();
    cleaned = push_closed(left.push_close_failure) && cleaned;
    return left.gateway_released && cleaned;
  }

  auto push_closed(const std::optional<platform::WebSocketError>& failure)
      -> bool {
    if (!failure || push_failure_logged_) return true;
    ending().record_cleanup_failure(LinkFailure::signalling_lost(
        rp::FailureStage::shutdown,
        {psn::remote_play::PushErrorKind::websocket_failure, *failure}));
    return false;
  }

  [[nodiscard]] auto fail(rp::StreamError error, rp::StreamFailure reason,
                          std::optional<LinkFailure> detail = {}) -> Result {
    if (configuration_.stop.stop_requested() &&
        !configuration_.stop.deadline_expired())
      reason = rp::StreamFailure::cancelled;
    if (reason != rp::StreamFailure::cancelled) internet_.route_failed();
    if (auto signalling = internet_.signalling_failure();
        signalling && !push_failure_logged_)
      detail = LinkFailure::signalling_lost(stream_.stage(), *signalling);
    return stream_.fail(error, reason, std::move(detail));
  }

  [[nodiscard]] auto emit_registered() -> bool {
    rp::event::Registered registered;
    const auto& source = configuration_.console;
    auto& copy = registered.console;
    copy.target = source.target;
    copy.ap_ssid = source.ap_ssid;
    copy.ap_bssid = source.ap_bssid;
    copy.ap_name = source.ap_name;
    copy.nickname = source.nickname;
    copy.ap_key = source.ap_key.clone();
    copy.mac = source.mac;
    copy.registration_key = source.registration_key.clone();
    copy.key = source.key.clone();
    copy.key_type = source.key_type;
    stream_.emit(std::move(registered));
    return stream_.state() == rp::StreamState::idle;
  }

  [[nodiscard]] auto emit_nickname() -> bool {
    if (playing_ps5()) return true;
    const auto& nickname = configuration_.console.nickname;
    const auto end = std::ranges::find(nickname, '\0');
    stream_.emit(
        rp::event::NicknameReceived{std::string(nickname.begin(), end)});
    return stream_.state() == rp::StreamState::idle;
  }

  bool ctrl_failed_before_stream_{};
  bool display_devices_resent_{};
  bool pairing_only_{};
  InternetLinkConfiguration configuration_;
  psn::rest::ConsoleFamily family_{configuration_.console.target ==
                                           rp::RegistrationTarget::ps4_10
                                       ? psn::rest::ConsoleFamily::ps4
                                       : psn::rest::ConsoleFamily::ps5};
  std::optional<rp::LocalVideoSelection> video_selection_;
  platform::PlatformFactory& platform_;
  ActiveStream stream_;
  platform::CryptoProvider& crypto_;
  platform::ClockProvider& clock_;
  platform::SecureRandomProvider& random_;
  net::protocol::takion::PlatformTakionCryptoProvider takion_crypto_;
  play::InternetStart internet_;
  std::optional<play::internet::remote_ctrl::Connected> ctrl_;
  std::optional<rp::LocalCtrlReadyTicket> ready_ticket_;
  std::optional<rp::RouteCandidate> pending_data_route_;
  timing::Deadline::Instant ctrl_wait_started_{};
  SenkushaSink senkusha_sink_;
  play::PinLifecycle pin_;
  std::optional<SecretBytes> queued_pin_;
  bool push_failure_logged_{};
  enum class CtrlHealth : std::uint8_t { steady, troubled };
  CtrlHealth ctrl_health_{CtrlHealth::steady};
  std::optional<std::uint64_t> last_ctrl_trouble_us_;
  std::optional<std::uint16_t> last_ctrl_counter_;
  std::optional<std::uint64_t> ctrl_keepalive_us_;
  bool data_accepted_{};
  std::uint64_t traced_stages_{};
  std::optional<timing::Deadline::Instant> trace_started_;
};

auto InternetLinks::configured(
    const consoles::PairedPlay::InternetTarget& target,
    std::shared_ptr<users::PsUser> as, VideoRequest video,
    ConsolePreferences preferences, PortGuessing port_guessing,
    std::chrono::seconds start_timeout)
    -> Expected<InternetLinkConfiguration, ConfigurationError> {
  if (as == nullptr || !as->has_psn_account())
    return unexpected(ConfigurationError::invalid_credential);
  const auto wire_target = target.firmware.target();
  if (wire_target != rp::RegistrationTarget::ps4_10 &&
      wire_target != rp::RegistrationTarget::ps5_1)
    return unexpected(ConfigurationError::invalid_console);
  if (start_timeout.count() <= 0)
    return unexpected(ConfigurationError::rejected);
  InternetLinkConfiguration configuration;
  configuration.user = std::move(as);
  configuration.device_uid = target.device_uid.bytes();
  configuration.console.target = wire_target;
  configuration.video = video.settings();
  configuration.codec = video.codec_for(target.firmware.generation());
  configuration.enable_ipv6 = true;
  configuration.force_port_guessing = port_guessing.forced();
  configuration.port_guess_count = port_guessing.count();
  configuration.start_timeout_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(start_timeout)
          .count());
  configuration.feedback = preferences.feedback;
  configuration.enable_keyboard = preferences.keyboard;
  configuration.enable_idr_on_fec_failure = preferences.idr_on_fec_failure;
  configuration.tuning = preferences.tuning;
  if (!configuration.prefer_controller(preferences.controller))
    return unexpected(ConfigurationError::rejected);
  auto accepted = std::move(configuration).validated();
  if (!accepted) return unexpected(ConfigurationError::rejected);
  return std::move(accepted.value());
}

auto InternetLinks::playing(InternetLinkConfiguration configuration,
                            platform::PlatformFactory& platform,
                            play::InternetStart::PairingListener* pairings)
    -> Expected<std::unique_ptr<Link>, rp::StreamError> {
  if (!platform.supplies(platform::PlatformCapability::crypto |
                         platform::PlatformCapability::http |
                         platform::PlatformCapability::websocket))
    return unexpected(rp::StreamError::provider_unavailable);
  return std::unique_ptr<Link>{
      new InternetLink(std::move(configuration), platform, false, pairings)};
}

auto InternetLinks::pairing_only(InternetLinkConfiguration configuration,
                                 platform::PlatformFactory& platform)
    -> Expected<std::unique_ptr<Link>, rp::StreamError> {
  if (!platform.supplies(platform::PlatformCapability::crypto |
                         platform::PlatformCapability::http |
                         platform::PlatformCapability::websocket))
    return unexpected(rp::StreamError::provider_unavailable);
  return std::unique_ptr<Link>{
      new InternetLink(std::move(configuration), platform, true, nullptr)};
}

}  // namespace libchiaking::play
