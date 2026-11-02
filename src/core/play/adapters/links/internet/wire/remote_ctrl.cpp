#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"

#include <algorithm>

namespace libchiaking::play::internet::remote_ctrl {

auto Connector::start_http(
    net::transport::rudp::HandshakeChannel& channel,
    const net::transport::rudp::HandshakeCompletion& completion,
    net::protocol::remote_play::PreparedLocalCtrl&& prepared,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    platform::ClockProvider& clock, platform::StopToken stop_token)
    -> Expected<StartupResult, StartupFailure> {
  const auto request = prepared.request.bytes();
  if (request.empty())
    return unexpected(StartupFailure{StartupError::invalid_request});
  net::protocol::remote_play::RudpCtrlHttpExchange exchange{
      completion.next_local_counter,
      completion.remote_counter,
      request,
      channel,
      clock,
      std::move(stop_token),
      net::protocol::remote_play::kReferenceRudpRetryPolicy};
  auto outcome = exchange.run();
  if (!outcome)
    return unexpected(
        StartupFailure{StartupError::exchange_failed,
                       static_cast<std::uint32_t>(outcome.error().error), 0,
                       outcome.error().received_bytes});
  auto final = net::protocol::remote_play::RudpCtrl::finalize_http(
      *outcome, crypto, target, prepared.material);
  if (!final) {
    const auto parsed = net::protocol::remote_play::CtrlCodec::parse_response(
        outcome->response.received);
    return unexpected(StartupFailure{StartupError::finalize_failed,
                                     static_cast<std::uint32_t>(final.error()),
                                     !parsed ? std::uint16_t{0}
                                     : parsed.value()
                                         ? net::HttpStatus::ok.code()
                                         : parsed.value().error().status(),
                                     outcome->received_bytes});
  }
  const auto local_crypt_counter =
      prepared.request.next_crypt_counter() +
      (target == net::protocol::remote_play::RegistrationTarget::ps5_1 ? 1U
                                                                       : 0U);
  return StartupResult{std::move(prepared), std::move(final.value()),
                       local_crypt_counter};
}

auto Connector::send_heartbeat(
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    const net::protocol::remote_play::AuthMaterial& material,
    net::transport::rudp::HandshakeChannel& channel, OutboundStepState state,
    const net::protocol::remote_play::ctrl_lifecycle_detail::Effect& effect)
    -> Expected<OutboundStepState, OutboundStepError> {
  const auto* heartbeat = std::get_if<
      net::protocol::remote_play::ctrl_lifecycle_detail::SendHeartbeatReply>(
      &effect);
  if (!heartbeat) return unexpected(OutboundStepError::unsupported_effect);
  if (heartbeat->attempt_id != state.attempt_id)
    return unexpected(OutboundStepError::stale_attempt);
  auto encoded = net::protocol::remote_play::RudpCtrl::encode_outbound(
      crypto, target, material,
      net::protocol::remote_play::ctrl_heartbeat_reply_type, {},
      state.crypt_counter, state.local_rudp_counter);
  if (!encoded) return unexpected(OutboundStepError::encode_failed);
  static_cast<void>(channel.send(encoded.value().bytes));
  state.crypt_counter = encoded.value().next_crypt_counter;
  state.local_rudp_counter = encoded.value().next_rudp_counter;
  return state;
}

auto Connector::send_pin(
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    const net::protocol::remote_play::AuthMaterial& material,
    net::transport::rudp::HandshakeChannel& channel, OutboundStepState state,
    const net::protocol::remote_play::ctrl_lifecycle_detail::Effect& effect,
    std::span<const std::byte> pin)
    -> Expected<OutboundStepState, OutboundStepError> {
  const auto* request = std::get_if<
      net::protocol::remote_play::ctrl_lifecycle_detail::RequestPin>(&effect);
  if (!request) return unexpected(OutboundStepError::unsupported_effect);
  if (request->attempt_id != state.attempt_id)
    return unexpected(OutboundStepError::stale_attempt);
  if (pin.empty() ||
      pin.size() > net::protocol::remote_play::ctrl_frame_max_payload)
    return unexpected(OutboundStepError::invalid_pin);
  auto encoded = net::protocol::remote_play::RudpCtrl::encode_outbound(
      crypto, target, material,
      net::protocol::remote_play::ctrl_login_pin_reply_type, pin,
      state.crypt_counter, state.local_rudp_counter);
  if (!encoded) return unexpected(OutboundStepError::encode_failed);
  auto sent = channel.send(encoded.value().bytes);
  if (!sent) return unexpected(OutboundStepError::transport);
  if (sent.value() != encoded.value().bytes.size())
    return unexpected(OutboundStepError::short_send);
  state.crypt_counter = encoded.value().next_crypt_counter;
  state.local_rudp_counter = encoded.value().next_rudp_counter;
  return state;
}

auto Inbox::post_id_repeat_pending() const noexcept -> bool {
  return post_id_repeats_ != 0;
}

auto Inbox::front_effect() const noexcept
    -> const net::protocol::remote_play::ctrl_lifecycle_detail::Effect* {
  return effects_.empty() ? nullptr : &effects_.front();
}

Inbox::Inbox(SecretArray<net::rudp_nonce_size> nonce,
             std::uint64_t attempt_id) noexcept
    : nonce_(std::move(nonce)), attempt_id_(attempt_id), lifecycle_(context_) {}

bool Inbox::start() {
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Start{attempt_id_});
  drain_effects();
  return accepted;
}

bool Inbox::accept(
    net::protocol::remote_play::RudpCtrlSwitchStepResult&& step) {
  auto& message = step.message.message;
  if (step.switch_notification !=
      std::holds_alternative<net::protocol::remote_play::CtrlStreamSwitchAck>(
          message))
    return false;
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Received{
          step.attempt_id, message});
  drain_effects();
  return accepted;
}

bool Inbox::publish_fallback(std::uint64_t attempt_id,
                             net::protocol::remote_play::SessionId&& value) {
  if (attempt_id != attempt_id_ ||
      lifecycle_.state() != net::protocol::remote_play::ctrl_lifecycle_detail::
                                State::waiting_id ||
      ticket_taken_ || session_id_)
    return false;
  net::protocol::remote_play::CtrlMessage message{
      net::protocol::remote_play::CtrlSessionId{std::move(value)}};
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Received{attempt_id,
                                                                  message});
  drain_effects();
  return accepted && session_id_.has_value();
}

bool Inbox::stop(std::uint64_t attempt_id) {
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Stop{attempt_id});
  drain_effects();
  return accepted;
}

bool Inbox::fail(
    std::uint64_t attempt_id,
    net::protocol::remote_play::ctrl_lifecycle_detail::FailureReason reason) {
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Failure{attempt_id,
                                                                 reason});
  drain_effects();
  return accepted;
}

bool Inbox::closed(std::uint64_t attempt_id, bool success) {
  const bool accepted = lifecycle_.process(
      net::protocol::remote_play::ctrl_lifecycle_detail::Closed{attempt_id,
                                                                success});
  drain_effects();
  return accepted;
}

auto Inbox::take_ready_ticket()
    -> std::optional<net::protocol::remote_play::LocalCtrlReadyTicket> {
  if (!session_id_ || !nonce_) return std::nullopt;
  net::protocol::remote_play::LocalCtrlReadyTicket ticket{
      std::move(*session_id_), std::move(*nonce_), attempt_id_};
  session_id_.reset();
  nonce_.reset();
  ticket_taken_ = true;
  return ticket;
}

auto Inbox::take_effect() -> std::optional<
    net::protocol::remote_play::ctrl_lifecycle_detail::Effect> {
  if (effects_.empty()) return std::nullopt;
  auto effect = std::move(effects_.front());
  effects_.pop_front();
  return effect;
}

auto Inbox::take_notice()
    -> std::optional<net::protocol::remote_play::CtrlNotice> {
  if (notices_.empty()) return std::nullopt;
  auto notice = std::move(notices_.front());
  notices_.pop_front();
  return notice;
}

auto Inbox::take_post_id_repeat() noexcept -> bool {
  if (post_id_repeats_ == 0) return false;
  --post_id_repeats_;
  return true;
}

void Inbox::coalesce_pin_requests() {
  drain_effects();
  bool kept = false;
  std::erase_if(
      effects_,
      [&kept](const net::protocol::remote_play::ctrl_lifecycle_detail::Effect&
                  effect) {
        if (!std::holds_alternative<
                net::protocol::remote_play::ctrl_lifecycle_detail::RequestPin>(
                effect))
          return false;
        return std::exchange(kept, true);
      });
}

void Inbox::set_pin_prompt_open(bool open) noexcept { pin_prompt_open_ = open; }

void Inbox::close_pin_window() {
  pin_window_closed_ = true;
  drain_effects();
  if (!pin_prompt_open_)
    std::erase_if(
        effects_,
        [](const net::protocol::remote_play::ctrl_lifecycle_detail::Effect&
               effect) {
          return std::holds_alternative<
              net::protocol::remote_play::ctrl_lifecycle_detail::RequestPin>(
              effect);
        });
}

void Inbox::promote_heartbeat_replies() {
  std::stable_partition(
      effects_.begin(), effects_.end(),
      [](const net::protocol::remote_play::ctrl_lifecycle_detail::Effect&
             effect) {
        return std::holds_alternative<
            net::protocol::remote_play::ctrl_lifecycle_detail::
                SendHeartbeatReply>(effect);
      });
}

void Inbox::drain_effects() {
  while (!context_.effects.empty()) {
    auto effect = std::move(context_.effects.front());
    context_.effects.pop_front();
    if (auto* id =
            std::get_if<net::protocol::remote_play::ctrl_lifecycle_detail::
                            PublishSessionId>(&effect)) {
      if (!session_id_ && !ticket_taken_ && id->attempt_id == attempt_id_)
        session_id_.emplace(std::move(id->value));
    } else if (auto* notice =
                   std::get_if<net::protocol::remote_play::
                                   ctrl_lifecycle_detail::PublishNotice>(
                       &effect)) {
      if (notice->attempt_id == attempt_id_)
        notices_.push_back(std::move(notice->notice));
    } else if (const auto* repeat =
                   std::get_if<net::protocol::remote_play::
                                   ctrl_lifecycle_detail::RepeatPostIdFeatures>(
                       &effect)) {
      if (repeat->attempt_id == attempt_id_) ++post_id_repeats_;
    } else if ((session_id_ || ticket_taken_ || pin_window_closed_) &&
               !pin_prompt_open_ &&
               std::holds_alternative<net::protocol::remote_play::
                                          ctrl_lifecycle_detail::RequestPin>(
                   effect)) {
      continue;
    } else {
      effects_.push_back(std::move(effect));
    }
  }
}

Pump::Pump(net::transport::rudp::HandshakeChannel& channel, Inbox& inbox,
           platform::CryptoProvider& crypto,
           net::protocol::remote_play::RegistrationTarget target,
           const net::protocol::remote_play::AuthMaterial& material,
           PumpState state,
           net::protocol::remote_play::RudpCtrlByteStream* byte_stream) noexcept
    : channel_(channel),
      inbox_(inbox),
      crypto_(crypto),
      target_(target),
      material_(material),
      state_(state),
      byte_stream_(byte_stream) {}

auto Pump::pump_once(std::uint64_t attempt_id,
                     std::chrono::microseconds timeout,
                     const platform::StopToken& stop)
    -> Expected<void, PumpError> {
  if (attempt_id != state_.attempt_id)
    return unexpected(PumpError::stale_attempt);
  net::protocol::remote_play::RudpCtrlByteStream isolated;
  net::protocol::remote_play::RudpCtrlReceiver receiver;
  auto result =
      receiver.step(channel_,
                    {crypto_, target_, material_, state_.remote_crypt_counter,
                     state_.local_counter, attempt_id,
                     byte_stream_ ? *byte_stream_ : isolated},
                    timeout, stop);
  state_.last_receive = receiver.evidence();
  if (!result) {
    switch (result.error()) {
      case net::protocol::remote_play::RudpCtrlSwitchStepError::stopped:
        return unexpected(PumpError::stopped);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::timeout:
        return unexpected(PumpError::timeout);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::receive_failed:
        return unexpected(PumpError::receive_failed);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::malformed:
        return unexpected(PumpError::malformed);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::crypto_failed:
        return unexpected(PumpError::crypto_failed);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::
          counter_exhausted:
        return unexpected(PumpError::counter_exhausted);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::
          ack_encode_failed:
        return unexpected(PumpError::ack_encode_failed);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::ack_send_failed:
        return unexpected(PumpError::ack_send_failed);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::ack_short_send:
        return unexpected(PumpError::ack_short_send);
      case net::protocol::remote_play::RudpCtrlSwitchStepError::peer_closed:
        return unexpected(PumpError::peer_closed);
    }
    return unexpected(PumpError::step_failed);
  }
  for (auto& step : result.value()) {
    if (step.delivery ==
        net::protocol::remote_play::RudpCtrlDelivery::acknowledgement)
      continue;
    const auto next_crypt = step.message.next_remote_crypt_counter;
    const auto remote_counter = step.message.remote_counter;
    state_.remote_crypt_counter = next_crypt;
    state_.remote_counter = remote_counter;
    if (step.delivery ==
            net::protocol::remote_play::RudpCtrlDelivery::control &&
        !inbox_.accept(std::move(step)))
      return unexpected(PumpError::inbox_rejected);
  }
  return {};
}

auto Session::create(StartupResult&& startup, std::uint64_t attempt_id)
    -> Expected<std::unique_ptr<Session>, SessionError> {
  if (attempt_id == 0) return unexpected(SessionError::invalid_attempt);
  const auto local_crypt = startup.local_crypt_counter.value_or(
      startup.prepared.request.next_crypt_counter());
  const auto remote_crypt = startup.final.response.next_remote_counter;
  const auto local_rudp = startup.final.next_local_counter;
  const auto remote_rudp = startup.final.remote_counter;
  auto owner = std::unique_ptr<Session>(new Session(
      std::move(startup.prepared.nonce), std::move(startup.prepared.material),
      {remote_crypt, local_rudp, remote_rudp, attempt_id},
      {local_crypt, local_rudp, attempt_id}, attempt_id));
  owner->rp_prohibited_ = startup.final.response.prohibited;
  owner->server_type_ = startup.final.response.known_server_type();
  if (!owner->inbox_.start()) return unexpected(SessionError::invalid_attempt);
  return owner;
}

auto Session::apply_switch_counters(std::uint64_t attempt_id,
                                    std::uint64_t remote_crypt_counter,
                                    std::uint16_t next_local_counter) noexcept
    -> bool {
  if (attempt_id != attempt_id_ ||
      remote_crypt_counter < pump_state_.remote_crypt_counter)
    return false;
  pump_state_.remote_crypt_counter = remote_crypt_counter;
  pump_state_.local_counter = next_local_counter;
  outbound_state_.local_rudp_counter = next_local_counter;
  return true;
}

auto Session::take_ready_ticket()
    -> std::optional<net::protocol::remote_play::LocalCtrlReadyTicket> {
  return inbox_.take_ready_ticket();
}

auto Session::accept_acked_switch_step(
    net::protocol::remote_play::RudpCtrlSwitchStepResult result) -> bool {
  if (result.attempt_id != attempt_id_ ||
      result.message.next_remote_crypt_counter <
          pump_state_.remote_crypt_counter)
    return false;
  pump_state_.remote_crypt_counter = result.message.next_remote_crypt_counter;
  pump_state_.remote_counter = result.message.remote_counter;
  return inbox_.accept(std::move(result));
}

auto Session::accept_acked_initial_frame(
    std::span<const std::byte> frame, platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target)
    -> Expected<void, InitialFrameError> {
  net::protocol::remote_play::RudpOngoingCtrl incoming{};
  incoming.ctrl_frame.assign(frame.begin(), frame.end());
  incoming.remote_counter = pump_state_.remote_counter;
  auto decoded = net::protocol::remote_play::RudpCtrl::decode_message(
      crypto, target, material_, pump_state_.remote_crypt_counter, incoming);
  if (!decoded) {
    switch (decoded.error()) {
      case net::protocol::remote_play::RudpCtrlMessageError::crypto_failure:
        return unexpected(InitialFrameError::crypto_failed);
      case net::protocol::remote_play::RudpCtrlMessageError::counter_exhausted:
        return unexpected(InitialFrameError::counter_exhausted);
      case net::protocol::remote_play::RudpCtrlMessageError::malformed:
        return unexpected(InitialFrameError::malformed);
    }
    return unexpected(InitialFrameError::malformed);
  }
  const bool switch_ack =
      std::holds_alternative<net::protocol::remote_play::CtrlStreamSwitchAck>(
          decoded.value().message);
  pump_state_.remote_crypt_counter = decoded.value().next_remote_crypt_counter;
  if (!inbox_.accept(net::protocol::remote_play::RudpCtrlSwitchStepResult{
          std::move(decoded.value()), attempt_id_, switch_ack}))
    return unexpected(InitialFrameError::inbox_rejected);
  return {};
}

auto Session::receive_once(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    std::chrono::microseconds timeout, const platform::StopToken& stop)
    -> Expected<void, PumpError> {
  Pump pump(channel, inbox_, crypto, target, material_, pump_state_,
            &byte_stream_);
  auto received = pump.pump_once(attempt_id_, timeout, stop);
  if (received || received.error() == PumpError::inbox_rejected)
    pump_state_ = pump.state();
  else
    pump_state_.last_receive = pump.state().last_receive;
  return received;
}

auto Session::send_keepalive(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target)
    -> Expected<OutboundStepState, OutboundStepError> {
  const net::protocol::remote_play::ctrl_lifecycle_detail::Effect reply{
      net::protocol::remote_play::ctrl_lifecycle_detail::SendHeartbeatReply{
          attempt_id_}};
  auto sent = Connector::send_heartbeat(crypto, target, material_, channel,
                                        outbound_state_, reply);
  if (!sent) return unexpected(sent.error());
  outbound_state_ = sent.value();
  pump_state_.local_counter = outbound_state_.local_rudp_counter;
  return outbound_state_;
}

auto Session::send_heartbeat_once(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target)
    -> Expected<OutboundStepState, OutboundStepError> {
  const auto* front = inbox_.front_effect();
  if (!front) return unexpected(OutboundStepError::unsupported_effect);
  auto sent = Connector::send_heartbeat(crypto, target, material_, channel,
                                        outbound_state_, *front);
  if (!sent) return unexpected(sent.error());
  outbound_state_ = sent.value();
  pump_state_.local_counter = outbound_state_.local_rudp_counter;
  (void)inbox_.take_effect();
  return outbound_state_;
}

auto Session::send_pin_once(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    std::span<const std::byte> pin)
    -> Expected<OutboundStepState, OutboundStepError> {
  const auto* front = inbox_.front_effect();
  if (!front) return unexpected(OutboundStepError::unsupported_effect);
  auto sent = Connector::send_pin(crypto, target, material_, channel,
                                  outbound_state_, *front, pin);
  if (!sent) return unexpected(sent.error());
  outbound_state_ = sent.value();
  pump_state_.local_counter = outbound_state_.local_rudp_counter;
  (void)inbox_.take_effect();
  return outbound_state_;
}

auto Session::send_post_id_features(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    bool enable_dualsense, bool enable_keyboard)
    -> Expected<void, OutboundStepError> {
  if (inbox_.state() !=
      net::protocol::remote_play::ctrl_lifecycle_detail::State::active)
    return unexpected(OutboundStepError::unsupported_effect);
  const auto frames = net::protocol::remote_play::CtrlCodec::post_id_frames(
      enable_dualsense, enable_keyboard);
  while (post_id_index_ < frames.size()) {
    const auto& frame = frames[post_id_index_];
    auto sent =
        send_post_id_frame(channel, crypto, target, frame.type, frame.payload);
    if (!sent) return sent;
    ++post_id_index_;
  }
  post_id_complete_ = true;
  return {};
}

auto Session::repeat_post_id_features(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    bool enable_dualsense, bool enable_keyboard)
    -> Expected<void, OutboundStepError> {
  while (post_id_complete_ && inbox_.take_post_id_repeat()) {
    post_id_index_ = 0;
    post_id_complete_ = false;
    auto sent = send_post_id_features(channel, crypto, target, enable_dualsense,
                                      enable_keyboard);
    if (!sent) return sent;
  }
  return {};
}

auto Session::send_ctrl_frame(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target, std::uint16_t type,
    std::span<const std::byte> payload) -> Expected<void, OutboundStepError> {
  if (inbox_.state() !=
      net::protocol::remote_play::ctrl_lifecycle_detail::State::active)
    return unexpected(OutboundStepError::unsupported_effect);
  auto encoded = net::protocol::remote_play::RudpCtrl::encode_outbound(
      crypto, target, material_, type, payload, outbound_state_.crypt_counter,
      outbound_state_.local_rudp_counter);
  if (!encoded) return unexpected(OutboundStepError::encode_failed);
  outbound_state_.crypt_counter = encoded.value().next_crypt_counter;
  outbound_state_.local_rudp_counter = encoded.value().next_rudp_counter;
  pump_state_.local_counter = outbound_state_.local_rudp_counter;
  auto sent = channel.send(encoded.value().bytes);
  if (!sent) return unexpected(OutboundStepError::transport);
  if (sent.value() != encoded.value().bytes.size())
    return unexpected(OutboundStepError::short_send);
  return {};
}

auto Session::send_post_id_frame(
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target, std::uint16_t type,
    std::span<const std::byte> payload) -> Expected<void, OutboundStepError> {
  auto encoded = net::protocol::remote_play::RudpCtrl::encode_outbound(
      crypto, target, material_, type, payload, outbound_state_.crypt_counter,
      outbound_state_.local_rudp_counter);
  if (!encoded) return unexpected(OutboundStepError::encode_failed);
  static_cast<void>(channel.send(encoded.value().bytes));
  outbound_state_.crypt_counter = encoded.value().next_crypt_counter;
  outbound_state_.local_rudp_counter = encoded.value().next_rudp_counter;
  pump_state_.local_counter = outbound_state_.local_rudp_counter;
  return {};
}

Session::Session(SecretArray<net::rudp_nonce_size> nonce,
                 net::protocol::remote_play::AuthMaterial material,
                 PumpState pump, OutboundStepState outbound,
                 std::uint64_t attempt_id)
    : material_(std::move(material)),
      inbox_(std::move(nonce), attempt_id),
      pump_state_(pump),
      outbound_state_(outbound),
      attempt_id_(attempt_id) {}

auto Connector::drain_initial_frames(
    Session& owner, net::protocol::remote_play::WipingCtrlBytes& trailing,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target)
    -> Expected<void, ConnectError> {
  if (trailing.size() > net::protocol::remote_play::ctrl_frame_max_size)
    return unexpected(ConnectError::trailing_too_large);
  std::size_t consumed = 0;
  while (consumed < trailing.size()) {
    const std::span<const std::byte> remaining{trailing.data() + consumed,
                                               trailing.size() - consumed};
    auto frame = net::protocol::remote_play::CtrlCodec::decode_frame(remaining);
    if (!frame) {
      if (frame.error() ==
          net::protocol::remote_play::CtrlFrameError::incomplete)
        break;
      return unexpected(ConnectError::trailing_too_large);
    }
    auto accepted = owner.accept_acked_initial_frame(
        remaining.first(frame.value().consumed), crypto, target);
    if (!accepted) {
      if (accepted.error() == InitialFrameError::crypto_failed)
        return unexpected(ConnectError::trailing_crypto_failed);
      if (accepted.error() == InitialFrameError::counter_exhausted)
        return unexpected(ConnectError::trailing_counter_exhausted);
      if (accepted.error() == InitialFrameError::malformed)
        return unexpected(ConnectError::trailing_malformed);
      return unexpected(ConnectError::trailing_lifecycle_failed);
    }
    consumed += frame.value().consumed;
  }
  net::protocol::remote_play::WipingCtrlBytes suffix{
      trailing.begin() + static_cast<std::ptrdiff_t>(consumed), trailing.end()};
  trailing.swap(suffix);
  return {};
}

auto Connector::connect_registered(
    net::transport::rudp::Connected connected,
    platform::PlatformFactory& platform, platform::CryptoProvider& crypto,
    const net::protocol::remote_play::RegisteredConsole& console,
    std::string_view host, std::uint16_t port, std::span<const std::byte> did,
    net::protocol::remote_play::VideoCodec codec,
    std::span<const std::byte> nonce, platform::StopToken stop,
    std::uint64_t attempt) -> Expected<Connected, ConnectFailure> {
  if (attempt == 0)
    return unexpected(ConnectFailure{ConnectError::invalid_attempt, 0, {}});
  if (!connected.transport)
    return unexpected(ConnectFailure{ConnectError::invalid_transport, 0, {}});
  if (host.empty())
    return unexpected(ConnectFailure{ConnectError::invalid_host, 0, {}});
  if (port == 0)
    return unexpected(ConnectFailure{ConnectError::invalid_port, 0, {}});
  if (stop.stop_requested())
    return unexpected(ConnectFailure{ConnectError::stopped, 0, {}});
  auto prepared =
      net::protocol::remote_play::LocalCtrlPreparation::prepare_from_nonce(
          crypto, console, host, port, did, codec, nonce);
  if (!prepared)
    return unexpected(
        ConnectFailure{ConnectError::prepare_failed,
                       static_cast<std::uint32_t>(prepared.error()),
                       {}});
  platform::ClockProvider& clock{platform.clock()};
  net::transport::rudp::ProviderRudpHandshakeChannel channel{
      *connected.transport};
  auto handshake = net::transport::rudp::Connector::connect_over_channel(
      channel,
      net::transport::rudp::HandshakeParameters{
          connected.completion.next_local_counter,
          connected.completion.local_header},
      clock, stop, attempt);
  if (!handshake)
    return unexpected(ConnectFailure{
        handshake.error() == net::transport::rudp::ConnectError::stopped
            ? ConnectError::stopped
            : ConnectError::connect_failed,
        static_cast<std::uint32_t>(handshake.error()),
        {}});
  connected.completion = handshake.value();
  auto startup =
      start_http(channel, connected.completion, std::move(prepared.value()),
                 crypto, console.target, clock, stop);
  if (!startup)
    return unexpected(
        ConnectFailure{ConnectError::startup_failed, 0, startup.error()});
  auto trailing = std::move(startup.value().final.trailing_received);
  auto session = Session::create(std::move(startup.value()), attempt);
  if (!session)
    return unexpected(
        ConnectFailure{ConnectError::session_failed,
                       static_cast<std::uint32_t>(session.error()),
                       {}});
  auto drained =
      drain_initial_frames(*session.value(), trailing, crypto, console.target);
  if (!drained) return unexpected(ConnectFailure{drained.error(), 0, {}});
  session.value()->byte_stream().seed(std::move(trailing));
  return Connected{std::move(connected), std::move(session.value())};
}

}  // namespace libchiaking::play::internet::remote_ctrl
