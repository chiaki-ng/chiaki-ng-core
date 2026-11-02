#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"

#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

auto LocalCtrlHandoff::adopt(platform::CryptoProvider& crypto,
                             CtrlStartupConnection&& connection,
                             RegistrationTarget target, AuthMaterial&& material,
                             std::uint64_t attempt_id)
    -> Expected<LocalCtrlHandoff, LocalCtrlHandoffError> {
  auto reader = std::make_unique<CtrlFrameReader>(crypto);
  auto response =
      reader->adopt(std::move(connection), target, std::move(material));
  if (!response) return unexpected(LocalCtrlHandoffError::reader_failure);
  auto owner = std::make_unique<CtrlOwner>(std::move(reader));
  if (!owner->start(attempt_id))
    return unexpected(LocalCtrlHandoffError::owner_start_failure);
  return LocalCtrlHandoff{std::move(owner), std::move(response.value())};
}

auto LocalCtrlPreparation::validate(const RegisteredConsole& console,
                                    std::string_view host, std::uint16_t port,
                                    std::span<const std::byte> did,
                                    VideoCodec codec)
    -> Expected<void, LocalCtrlPrepareError> {
  if (!RpVersion::of(console.target))
    return unexpected(LocalCtrlPrepareError::invalid_target);
  if (!CtrlStartupRequest::Head::accepts_host(host))
    return unexpected(LocalCtrlPrepareError::invalid_host);
  if (port == 0) return unexpected(LocalCtrlPrepareError::invalid_port);
  if (did.size() != rp_did_size)
    return unexpected(LocalCtrlPrepareError::invalid_did);
  if (!CtrlCodec::supports(codec))
    return unexpected(LocalCtrlPrepareError::invalid_codec);
  return {};
}

auto LocalCtrlPreparation::build_from_nonce(
    platform::CryptoProvider& crypto, const RegisteredConsole& console,
    std::string_view host, std::uint16_t port, std::span<const std::byte> did,
    VideoCodec codec, std::span<const std::byte> nonce)
    -> Expected<PreparedLocalCtrl, LocalCtrlPrepareError> {
  PreparedLocalCtrl prepared;
  std::ranges::copy(nonce, prepared.nonce.begin());
  auto material = AuthMaterial::derive(
      console.target, {prepared.nonce.data(), prepared.nonce.size()},
      {console.key.data(), console.key.size()});
  if (!material)
    return unexpected(LocalCtrlPrepareError::auth_material_failure);
  prepared.material = std::move(material.value());

  auto request = CtrlCodec::encode_startup_request(
      crypto, console.target, prepared.material, host, port,
      {console.registration_key.data(), console.registration_key.size()}, did,
      codec);
  if (!request) return unexpected(LocalCtrlPrepareError::request_failure);
  prepared.request = std::move(request.value());
  return prepared;
}

auto LocalCtrlPreparation::prepare_from_nonce(
    platform::CryptoProvider& crypto, const RegisteredConsole& console,
    std::string_view host, std::uint16_t port, std::span<const std::byte> did,
    VideoCodec codec, std::span<const std::byte> nonce)
    -> Expected<PreparedLocalCtrl, LocalCtrlPrepareError> {
  if (nonce.size() != rp_nonce_size)
    return unexpected(LocalCtrlPrepareError::invalid_nonce);
  const auto valid = validate(console, host, port, did, codec);
  if (!valid) return unexpected(valid.error());
  return build_from_nonce(crypto, console, host, port, did, codec, nonce);
}

LocalCtrlSessionId::LocalCtrlSessionId(LocalCtrlStartupResult&& result,
                                       std::uint64_t attempt_id) noexcept
    : result_(std::move(result)),
      attempt_id_(attempt_id),
      nonce_(std::move(result_.nonce)) {}

void LocalCtrlSessionId::drain_effects() {
  while (auto effect = result_.handoff.owner->take_effect()) {
    auto* id = std::get_if<ctrl_lifecycle_detail::PublishSessionId>(&*effect);
    auto* notice = std::get_if<ctrl_lifecycle_detail::PublishNotice>(&*effect);
    if (id && !captured_ && id->attempt_id == attempt_id_) {
      session_id_.emplace(std::move(id->value));
      captured_ = true;
    } else if (notice) {
      if (notice->attempt_id == attempt_id_)
        notices_.push_back(std::move(notice->notice));
    } else if (const auto* repeat =
                   std::get_if<ctrl_lifecycle_detail::RepeatPostIdFeatures>(
                       &*effect)) {
      if (repeat->attempt_id == attempt_id_) ++post_id_repeats_;
    } else if (captured_ && !pin_prompt_open_ &&
               std::holds_alternative<ctrl_lifecycle_detail::RequestPin>(
                   *effect)) {
      continue;
    } else {
      effects_.push_back(std::move(*effect));
    }
  }
}

auto LocalCtrlSessionId::pump_once() -> Expected<bool, CtrlReaderError> {
  if (!result_.handoff.owner) return unexpected(CtrlReaderError::invalid_state);
  const auto progress = result_.handoff.owner->pump_once();
  drain_effects();
  return progress;
}

auto LocalCtrlSessionId::take_session_id() -> std::optional<SessionId> {
  if (!session_id_) return std::nullopt;
  auto result = std::move(session_id_);
  session_id_.reset();
  return result;
}

auto LocalCtrlSessionId::take_nonce()
    -> std::optional<SecretArray<rp_nonce_size>> {
  if (!nonce_) return std::nullopt;
  auto result = std::move(nonce_);
  nonce_.reset();
  return result;
}

auto LocalCtrlSessionId::take_ready_ticket()
    -> std::optional<LocalCtrlReadyTicket> {
  if (!captured_ || !session_id_ || !nonce_) return std::nullopt;
  LocalCtrlReadyTicket ticket{std::move(*session_id_), std::move(*nonce_),
                              attempt_id_};
  session_id_.reset();
  nonce_.reset();
  return ticket;
}

auto LocalCtrlSessionId::publish_fallback(SessionId&& fallback) -> bool {
  if (!result_.handoff.owner || captured_ ||
      result_.handoff.owner->state() !=
          ctrl_lifecycle_detail::State::waiting_id)
    return false;
  const bool accepted = result_.handoff.owner->on_message(
      attempt_id_, CtrlMessage{CtrlSessionId{std::move(fallback)}});
  drain_effects();
  return accepted && captured_;
}

auto LocalCtrlSessionId::take_effect()
    -> std::optional<ctrl_lifecycle_detail::Effect> {
  if (heartbeat_pending_ || pin_pending_ || effects_.empty())
    return std::nullopt;
  auto result = std::move(effects_.front());
  effects_.pop_front();
  return result;
}

auto LocalCtrlSessionId::take_notice() -> std::optional<CtrlNotice> {
  if (notices_.empty()) return std::nullopt;
  auto result = std::move(notices_.front());
  notices_.pop_front();
  return result;
}

auto LocalCtrlSessionId::take_post_id_repeat() noexcept -> bool {
  if (post_id_repeats_ == 0) return false;
  --post_id_repeats_;
  return true;
}

void LocalCtrlSessionId::promote_heartbeat_replies() {
  if (!result_.handoff.owner) return;
  drain_effects();
  std::stable_partition(
      effects_.begin(), effects_.end(),
      [](const ctrl_lifecycle_detail::Effect& effect) {
        return std::holds_alternative<
            ctrl_lifecycle_detail::SendHeartbeatReply>(effect);
      });
}

void LocalCtrlSessionId::coalesce_pin_requests() {
  if (!result_.handoff.owner) return;
  drain_effects();
  bool kept = false;
  std::erase_if(effects_, [&kept](const ctrl_lifecycle_detail::Effect& effect) {
    if (!std::holds_alternative<ctrl_lifecycle_detail::RequestPin>(effect))
      return false;
    return std::exchange(kept, true);
  });
}

auto LocalCtrlSessionId::run_next_heartbeat()
    -> Expected<bool, LocalCtrlSessionIdError> {
  if (!result_.handoff.owner)
    return unexpected(LocalCtrlSessionIdError::invalid_state);
  drain_effects();
  if (heartbeat_pending_ || pin_pending_ || effects_.empty()) return false;
  const auto* heartbeat =
      std::get_if<ctrl_lifecycle_detail::SendHeartbeatReply>(&effects_.front());
  if (!heartbeat) return false;
  if (heartbeat->attempt_id != attempt_id_) return false;
  const auto attempt = heartbeat->attempt_id;
  effects_.pop_front();
  const auto sent = result_.handoff.owner->run_heartbeat_reply(attempt);
  drain_effects();
  if (!sent) return unexpected(LocalCtrlSessionIdError::reader_failure);
  heartbeat_pending_ = !sent.value();
  return true;
}

auto LocalCtrlSessionId::pump_heartbeat_outbound()
    -> Expected<bool, LocalCtrlSessionIdError> {
  if (!result_.handoff.owner || !heartbeat_pending_)
    return unexpected(LocalCtrlSessionIdError::invalid_state);
  const auto sent = result_.handoff.owner->pump_outbound();
  drain_effects();
  if (!sent) {
    heartbeat_pending_ = false;
    return unexpected(LocalCtrlSessionIdError::reader_failure);
  }
  heartbeat_pending_ = !sent.value();
  return sent.value();
}

auto LocalCtrlSessionId::run_next_pin_reply(std::span<const std::byte> pin)
    -> Expected<bool, LocalCtrlSessionIdError> {
  if (!result_.handoff.owner)
    return unexpected(LocalCtrlSessionIdError::invalid_state);
  drain_effects();
  if (pin_pending_ || heartbeat_pending_ || effects_.empty()) return false;
  const auto* request =
      std::get_if<ctrl_lifecycle_detail::RequestPin>(&effects_.front());
  if (!request || request->attempt_id != attempt_id_) return false;
  if (pin.size() > ctrl_frame_max_payload)
    return unexpected(LocalCtrlSessionIdError::pin_too_large);
  const auto attempt = request->attempt_id;
  effects_.pop_front();
  const auto sent = result_.handoff.owner->run_pin_reply(attempt, pin);
  drain_effects();
  if (!sent) return unexpected(LocalCtrlSessionIdError::reader_failure);
  pin_pending_ = !sent.value();
  return true;
}

auto LocalCtrlSessionId::pump_pin_outbound()
    -> Expected<bool, LocalCtrlSessionIdError> {
  if (!result_.handoff.owner || !pin_pending_)
    return unexpected(LocalCtrlSessionIdError::invalid_state);
  const auto sent = result_.handoff.owner->pump_outbound();
  drain_effects();
  if (!sent) {
    pin_pending_ = false;
    return unexpected(LocalCtrlSessionIdError::reader_failure);
  }
  pin_pending_ = !sent.value();
  return sent.value();
}

auto LocalCtrlSessionId::run_next_cleanup()
    -> Expected<bool, LocalCtrlSessionIdError> {
  if (!result_.handoff.owner)
    return unexpected(LocalCtrlSessionIdError::invalid_state);
  drain_effects();
  if (effects_.empty()) return false;
  const auto* cleanup =
      std::get_if<ctrl_lifecycle_detail::Cleanup>(&effects_.front());
  if (!cleanup || cleanup->attempt_id != attempt_id_) return false;
  effects_.pop_front();
  const auto closed = result_.handoff.owner->run_cleanup(attempt_id_);
  drain_effects();
  heartbeat_pending_ = false;
  pin_pending_ = false;
  if (!closed) return unexpected(LocalCtrlSessionIdError::reader_failure);
  return true;
}

LocalCtrlAttempt::LocalCtrlAttempt(
    platform::CryptoProvider& crypto, platform::SocketProvider& sockets,
    const platform::ClockProvider& clock) noexcept
    : crypto_(crypto), exchange_(sockets, clock) {}

auto LocalCtrlAttempt::start(const Config& config)
    -> Expected<void, LocalCtrlError> {
  if (config.attempt_id == 0 ||
      !lifecycle_.process(local_ctrl_lifecycle::Start{}))
    return unexpected(LocalCtrlFault::invalid_state);
  auto prepared = LocalCtrlPreparation::prepare_from_nonce(
      crypto_, config.console, config.host, config.port, config.did,
      config.codec, config.nonce);
  if (!prepared) return fail(LocalCtrlFault::prepare_failed);
  prepared_ = std::move(prepared.value());
  target_ = config.console.target;
  attempt_id_ = config.attempt_id;
  const auto begun =
      exchange_.begin(config.endpoint, std::move(prepared_.request));
  if (!begun) {
    prepared_ = PreparedLocalCtrl{};
    return fail(begun.error());
  }
  return {};
}

auto LocalCtrlAttempt::pump_once() -> Expected<bool, LocalCtrlError> {
  switch (lifecycle_.state()) {
    case local_ctrl_lifecycle::State::ready:
      return true;
    case local_ctrl_lifecycle::State::exchanging:
      break;
    case local_ctrl_lifecycle::State::idle:
    case local_ctrl_lifecycle::State::finished:
      return unexpected(LocalCtrlFault::invalid_state);
  }
  const auto complete = exchange_.pump_once();
  if (!complete) return abandon(complete.error());
  if (!complete.value()) return false;
  auto connection = exchange_.take();
  if (!connection) return abandon(connection.error());
  auto handoff =
      LocalCtrlHandoff::adopt(crypto_, std::move(connection.value()), target_,
                              std::move(prepared_.material), attempt_id_);
  if (!handoff) return abandon(LocalCtrlFault::handoff_failure);
  session_ = std::make_unique<LocalCtrlSessionId>(
      LocalCtrlStartupResult{std::move(handoff.value()),
                             std::move(prepared_.nonce)},
      attempt_id_);
  static_cast<void>(lifecycle_.process(local_ctrl_lifecycle::HandedOff{}));
  return true;
}

auto LocalCtrlAttempt::cancel() -> Expected<void, LocalCtrlError> {
  static_cast<void>(lifecycle_.process(local_ctrl_lifecycle::Cancel{}));
  const auto effect = lifecycle_.take_effect();
  if (!effect) return {};
  if (std::holds_alternative<local_ctrl_lifecycle::CloseReady>(*effect))
    return close_session();
  if (!abandon_exchange()) return unexpected(LocalCtrlFault::cleanup_failed);
  return {};
}

auto LocalCtrlAttempt::fail(LocalCtrlError error)
    -> Unexpected<LocalCtrlError> {
  static_cast<void>(lifecycle_.process(local_ctrl_lifecycle::Failed{}));
  return unexpected(error);
}

auto LocalCtrlAttempt::abandon(LocalCtrlError error)
    -> Unexpected<LocalCtrlError> {
  static_cast<void>(abandon_exchange());
  return fail(error);
}

auto LocalCtrlAttempt::abandon_exchange()
    -> Expected<void, ConsoleExchangeError> {
  prepared_ = PreparedLocalCtrl{};
  return exchange_.cancel();
}

auto LocalCtrlAttempt::close_session() -> Expected<void, LocalCtrlError> {
  auto* owner = session_ ? session_->owner() : nullptr;
  bool cleanup_ok = owner != nullptr;
  if (owner) {
    const auto state = owner->state();
    if (state == ctrl_lifecycle_detail::State::waiting_id ||
        state == ctrl_lifecycle_detail::State::active)
      cleanup_ok = owner->stop(attempt_id_);
    if (owner->state() == ctrl_lifecycle_detail::State::closing &&
        !owner->run_cleanup(attempt_id_))
      cleanup_ok = false;
  }
  session_.reset();
  if (!cleanup_ok) return unexpected(LocalCtrlFault::cleanup_failed);
  return {};
}

auto LocalCtrlAttempt::pollable() noexcept -> const platform::Pollable* {
  switch (lifecycle_.state()) {
    case local_ctrl_lifecycle::State::exchanging:
      return exchange_.pollable();
    case local_ctrl_lifecycle::State::ready:
      return session_ && session_->owner() ? session_->owner()->pollable()
                                           : nullptr;
    case local_ctrl_lifecycle::State::idle:
    case local_ctrl_lifecycle::State::finished:
      break;
  }
  return nullptr;
}

auto LocalCtrlSessionId::step_effect(LocalCtrlSessionId& ctrl,
                                     std::uint64_t attempt_id)
    -> Expected<LocalCtrlEffectStep, LocalCtrlSessionIdError> {
  using namespace ctrl_lifecycle_detail;
  if (ctrl.heartbeat_pending()) {
    if (!ctrl.pump_heartbeat_outbound())
      return unexpected(LocalCtrlSessionIdError::reader_failure);
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::advanced, {}};
  }
  if (ctrl.pin_pending()) {
    if (!ctrl.pump_pin_outbound())
      return unexpected(LocalCtrlSessionIdError::reader_failure);
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::advanced, {}};
  }
  const auto* front = ctrl.front_effect();
  if (!front) return LocalCtrlEffectStep{};
  const auto effect_attempt =
      std::visit([](const auto& effect) { return effect.attempt_id; }, *front);
  if (effect_attempt != attempt_id) {
    (void)ctrl.take_effect();
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::stale, {}};
  }
  if (std::holds_alternative<SendHeartbeatReply>(*front)) {
    const auto ran = ctrl.run_next_heartbeat();
    if (!ran) return unexpected(LocalCtrlSessionIdError::reader_failure);
    if (!ran.value()) return unexpected(LocalCtrlSessionIdError::invalid_state);
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::advanced, {}};
  }
  if (std::holds_alternative<RequestPin>(*front))
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::pin_required, {}};
  if (std::holds_alternative<Cleanup>(*front)) {
    const auto ran = ctrl.run_next_cleanup();
    if (!ran) return unexpected(LocalCtrlSessionIdError::reader_failure);
    if (!ran.value()) return unexpected(LocalCtrlSessionIdError::invalid_state);
    return LocalCtrlEffectStep{LocalCtrlEffectProgress::advanced, {}};
  }
  auto notification = ctrl.take_effect();
  if (!notification) return unexpected(LocalCtrlSessionIdError::invalid_state);
  return LocalCtrlEffectStep{LocalCtrlEffectProgress::notification,
                             std::move(notification)};
}

LocalPostIdCtrl::LocalPostIdCtrl(bool enable_dualsense, bool enable_keyboard)
    : frames_(CtrlCodec::post_id_frames(enable_dualsense, enable_keyboard)) {}

auto LocalPostIdCtrl::pump(CtrlOwner& owner, std::uint64_t attempt_id)
    -> Expected<bool, CtrlReaderError> {
  while (index_ < frames_.size()) {
    const auto& frame = frames_[index_];
    auto step = pending_ ? owner.pump_outbound()
                         : owner.run_control_frame(attempt_id, frame.type,
                                                   frame.payload);
    if (!step) return unexpected(step.error());
    if (!step.value()) {
      pending_ = true;
      return false;
    }
    pending_ = false;
    ++index_;
  }
  return true;
}

void LocalPostIdCtrl::restart() noexcept {
  index_ = 0;
  pending_ = false;
}

auto LocalRuntimeCtrl::queue(std::uint16_t type,
                             std::span<const std::byte> payload) -> bool {
  if (frames_.size() >= capacity ||
      payload.size() > ctrl_frame_max_outbound_payload)
    return false;
  frames_.push_back({type, {payload.begin(), payload.end()}});
  return true;
}

auto LocalRuntimeCtrl::pump(CtrlOwner& owner, std::uint64_t attempt_id)
    -> Expected<bool, CtrlReaderError> {
  while (!frames_.empty()) {
    const auto& frame = frames_.front();
    auto step = pending_ ? owner.pump_outbound()
                         : owner.run_control_frame(attempt_id, frame.type,
                                                   frame.payload);
    if (!step) {
      pending_ = false;
      frames_.clear();
      return unexpected(step.error());
    }
    if (!step.value()) {
      pending_ = true;
      return false;
    }
    pending_ = false;
    frames_.pop_front();
  }
  return true;
}

void LocalRuntimeCtrl::clear() noexcept {
  frames_.clear();
  pending_ = false;
}

}  // namespace libchiaking::net::protocol::remote_play
