#include "ctrl_owner.hpp"
#include <libchiaking/platform/runtime/threading.hpp>

#include <utility>

#include "ctrl_codec.hpp"

namespace libchiaking::net::protocol::remote_play {

CtrlOwner::CtrlOwner(std::unique_ptr<CtrlFrameReader> reader) noexcept
    : reader_(std::move(reader)), lifecycle_(context_) {}

CtrlOwner::~CtrlOwner() { assert_owner(); }

void CtrlOwner::assert_owner() const noexcept {
#ifndef NDEBUG
  assert(platform::ThreadIdentity::current() == owner_);
#endif
}

bool CtrlOwner::start(std::uint64_t attempt_id) {
  assert_owner();
  if (!reader_ || !lifecycle_.process(ctrl_lifecycle_detail::Start{attempt_id}))
    return false;
  attempt_id_ = attempt_id;
  return true;
}

bool CtrlOwner::on_message(std::uint64_t attempt_id, CtrlMessage&& message) {
  assert_owner();
  return lifecycle_.process(
      ctrl_lifecycle_detail::Received{attempt_id, message});
}

auto CtrlOwner::pump_once() -> Expected<bool, CtrlReaderError> {
  assert_owner();
  const auto current = lifecycle_.state();
  if (!reader_ || (current != ctrl_lifecycle_detail::State::waiting_id &&
                   current != ctrl_lifecycle_detail::State::active)) {
    return unexpected(CtrlReaderError::invalid_state);
  }
  auto frame = reader_->pump_once();
  if (!frame) {
    (void)lifecycle_.process(ctrl_lifecycle_detail::Failure{
        attempt_id_, ctrl_lifecycle_detail::FailureReason::transport});
    return unexpected(frame.error());
  }
  if (!frame.value()) return false;
  last_frame_type_ = frame.value()->type();
  last_display_state_.reset();
  if (const auto payload = frame.value()->payload();
      (last_frame_type_ ==
           CtrlCodec::message_type_id(CtrlMessageType::displaya) ||
       last_frame_type_ ==
           CtrlCodec::message_type_id(CtrlMessageType::displayb)) &&
      !payload.empty())
    last_display_state_ = std::array<std::uint8_t, 2>{
        std::to_integer<std::uint8_t>(payload[0]),
        payload.size() > 1 ? std::to_integer<std::uint8_t>(payload[1])
                           : std::uint8_t{}};
  auto message = CtrlCodec::decode_message(frame.value()->type(),
                                           frame.value()->payload());
  if (!on_message(attempt_id_, std::move(message)))
    return unexpected(CtrlReaderError::invalid_state);
  return true;
}

bool CtrlOwner::stop(std::uint64_t attempt_id) {
  assert_owner();
  return lifecycle_.process(ctrl_lifecycle_detail::Stop{attempt_id});
}

auto CtrlOwner::run_cleanup(std::uint64_t attempt_id)
    -> Expected<void, CtrlReaderError> {
  assert_owner();
  if (!reader_ || lifecycle_.state() != ctrl_lifecycle_detail::State::closing ||
      !context_.active_attempt_id || *context_.active_attempt_id != attempt_id)
    return unexpected(CtrlReaderError::invalid_state);
  const auto result = reader_->cancel();
  outbound_pending_ = false;
  (void)lifecycle_.process(
      ctrl_lifecycle_detail::Closed{attempt_id, static_cast<bool>(result)});
  return result;
}

auto CtrlOwner::run_heartbeat_reply(std::uint64_t attempt_id)
    -> Expected<bool, CtrlReaderError> {
  assert_owner();
  const auto current = lifecycle_.state();
  if (!reader_ || attempt_id != attempt_id_ ||
      (current != ctrl_lifecycle_detail::State::waiting_id &&
       current != ctrl_lifecycle_detail::State::active))
    return unexpected(CtrlReaderError::invalid_state);
  return queue_and_pump(
      CtrlCodec::message_type_id(CtrlMessageType::heartbeat_rep), {});
}

auto CtrlOwner::run_pin_reply(std::uint64_t attempt_id,
                              std::span<const std::byte> pin)
    -> Expected<bool, CtrlReaderError> {
  assert_owner();
  const auto current = lifecycle_.state();
  if (!reader_ || attempt_id != attempt_id_ ||
      (current != ctrl_lifecycle_detail::State::waiting_id &&
       current != ctrl_lifecycle_detail::State::active) ||
      !context_.pin_requested)
    return unexpected(CtrlReaderError::invalid_state);
  if (pin.size() > ctrl_frame_max_payload)
    return unexpected(CtrlReaderError::too_large);
  return queue_and_pump(
      CtrlCodec::message_type_id(CtrlMessageType::login_pin_rep), pin);
}

auto CtrlOwner::run_control_frame(std::uint64_t attempt_id, std::uint16_t type,
                                  std::span<const std::byte> payload)
    -> Expected<bool, CtrlReaderError> {
  assert_owner();
  if (!reader_ || attempt_id != attempt_id_ ||
      lifecycle_.state() != ctrl_lifecycle_detail::State::active)
    return unexpected(CtrlReaderError::invalid_state);
  return queue_and_pump(type, payload);
}

auto CtrlOwner::queue_and_pump(std::uint16_t type,
                               std::span<const std::byte> payload)
    -> Expected<bool, CtrlReaderError> {
  const auto queued = reader_->queue_send(type, payload);
  if (!queued) {
    if (queued.error() == CtrlReaderError::invalid_state ||
        queued.error() == CtrlReaderError::too_large)
      return unexpected(queued.error());
    ++dropped_sends_;
    if (outbound_pending_) return pump_outbound();
    return true;
  }
  outbound_pending_ = true;
  return pump_outbound();
}

auto CtrlOwner::pump_outbound() -> Expected<bool, CtrlReaderError> {
  assert_owner();
  if (!reader_ || !outbound_pending_)
    return unexpected(CtrlReaderError::invalid_state);
  const auto sent = reader_->pump_send();
  if (!sent) {
    outbound_pending_ = false;
    if (sent.error() == CtrlReaderError::invalid_state)
      return unexpected(sent.error());
    ++dropped_sends_;
    return true;
  }
  if (sent.value()) outbound_pending_ = false;
  return sent.value();
}

auto CtrlOwner::take_effect() -> std::optional<ctrl_lifecycle_detail::Effect> {
  assert_owner();
  if (context_.effects.empty()) return std::nullopt;
  auto effect = std::move(context_.effects.front());
  context_.effects.pop_front();
  return effect;
}

auto CtrlOwner::state() const noexcept -> ctrl_lifecycle_detail::State {
  assert_owner();
  return lifecycle_.state();
}

auto CtrlOwner::last_frame_type() const noexcept
    -> std::optional<std::uint16_t> {
  assert_owner();
  return last_frame_type_;
}

}  // namespace libchiaking::net::protocol::remote_play
