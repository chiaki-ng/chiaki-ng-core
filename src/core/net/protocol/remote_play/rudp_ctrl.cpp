#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto RudpCtrl::decode_ongoing(std::span<const std::byte> packet)
    -> Expected<std::vector<RudpOngoingCtrl>, rudp::FrameError> {
  auto frames = rudp::FrameCodec::decode(packet);
  if (!frames) return unexpected(frames.error());
  if (frames.value().front().payload.size() < 4)
    return unexpected(rudp::FrameError::malformed);
  const auto remote_counter = static_cast<std::uint16_t>(
      bytes::load_be<std::uint16_t>(frames.value().front().payload) + 1U);
  std::vector<RudpOngoingCtrl> result;
  result.reserve(frames.value().size());
  for (const auto& frame : frames.value()) {
    const auto subtype = rudp::FrameCodec::subtype_of(frame.type);
    const auto& payload = frame.payload;
    if (payload.size() < 2) return unexpected(rudp::FrameError::malformed);
    RudpOngoingCtrl out{};
    out.subtype = subtype;
    out.remote_counter = remote_counter;
    if (subtype == rudp::Subtype::ack ||
        subtype == rudp::Subtype::ack_ctrl_offset8 ||
        subtype == rudp::Subtype::ack_ctrl_offset6 ||
        subtype == rudp::Subtype::ack_ctrl_offset2) {
      if (payload.size() < 4) return unexpected(rudp::FrameError::malformed);
      out.ack_counter =
          bytes::load_be<std::uint16_t>(std::span{payload}.subspan(2));
    }
    if (subtype != rudp::Subtype::ack) {
      const std::size_t offset =
          subtype == rudp::Subtype::ack_ctrl_offset8   ? 8U
          : subtype == rudp::Subtype::ack_ctrl_offset6 ? 6U
          : subtype == rudp::Subtype::ctrl_message ||
                  subtype == rudp::Subtype::ack_ctrl_offset2
              ? 2U
              : 4U;
      if (payload.size() >= offset + ctrl_frame_header_size) {
        const auto tail = std::span<const std::byte>{payload}.subspan(offset);
        const auto declared = bytes::load_be<std::uint32_t>(tail);
        if (declared == tail.size() - ctrl_frame_header_size) {
          if (declared > ctrl_frame_max_payload)
            return unexpected(rudp::FrameError::too_large);
          out.ctrl_frame.assign(tail.begin(), tail.end());
        }
      }
    }
    result.push_back(std::move(out));
  }
  return result;
}

auto RudpCtrl::decode_message(platform::CryptoProvider& crypto,
                              RegistrationTarget target,
                              const AuthMaterial& material,
                              std::uint64_t remote_crypt_counter,
                              const RudpOngoingCtrl& incoming)
    -> Expected<RudpCtrlMessageResult, RudpCtrlMessageError> {
  auto frame = CtrlCodec::decode_frame(incoming.ctrl_frame);
  if (!frame || frame.value().consumed != incoming.ctrl_frame.size())
    return unexpected(RudpCtrlMessageError::malformed);
  SecretArray<ctrl_frame_max_payload> plain;
  const auto payload_size = frame.value().payload.size();
  if (payload_size > plain.size())
    return unexpected(RudpCtrlMessageError::malformed);
  if (payload_size != 0) {
    if (remote_crypt_counter == std::numeric_limits<std::uint64_t>::max())
      return unexpected(RudpCtrlMessageError::counter_exhausted);
    auto decrypted = material.transform(
        crypto, target, remote_crypt_counter, CipherDirection::decrypt,
        frame.value().payload, plain.span().first(payload_size));
    if (!decrypted) return unexpected(RudpCtrlMessageError::crypto_failure);
    ++remote_crypt_counter;
  }
  auto message = CtrlCodec::decode_message(frame.value().type,
                                           plain.span().first(payload_size));
  return RudpCtrlMessageResult{std::move(message), remote_crypt_counter,
                               incoming.remote_counter, incoming.ack_counter,
                               incoming.subtype};
}

void RudpCtrlByteStream::seed(WipingCtrlBytes bytes) {
  pending_ = std::move(bytes);
}

auto RudpCtrlByteStream::append(std::span<const std::byte> bytes)
    -> Expected<std::vector<WipingCtrlBytes>, CtrlFrameError> {
  if (pending_.size() > ctrl_frame_max_size ||
      bytes.size() > ctrl_frame_max_size - pending_.size())
    return unexpected(CtrlFrameError::too_large);
  pending_.insert(pending_.end(), bytes.begin(), bytes.end());
  std::vector<WipingCtrlBytes> result;
  std::size_t consumed{};
  while (consumed < pending_.size()) {
    const auto remaining =
        std::span<const std::byte>{pending_}.subspan(consumed);
    auto frame = CtrlCodec::decode_frame(remaining);
    if (!frame) {
      if (frame.error() == CtrlFrameError::incomplete) break;
      return unexpected(frame.error());
    }
    result.emplace_back(remaining.begin(),
                        remaining.begin() + static_cast<std::ptrdiff_t>(
                                                frame.value().consumed));
    consumed += frame.value().consumed;
  }
  WipingCtrlBytes suffix{
      pending_.begin() + static_cast<std::ptrdiff_t>(consumed), pending_.end()};
  pending_.swap(suffix);
  return result;
}

auto RudpCtrlByteStream::decode(const RudpOngoingCtrl& incoming,
                                platform::CryptoProvider& crypto,
                                RegistrationTarget target,
                                const AuthMaterial& material,
                                std::uint64_t crypt_counter)
    -> std::vector<RudpCtrlDecoded> {
  std::vector<RudpCtrlDecoded> decoded;
  auto frames = append(incoming.ctrl_frame);
  if (!frames) {
    pending_.clear();
    decoded.emplace_back(unexpected(RudpCtrlMessageError::malformed));
    return decoded;
  }
  for (const auto& bytes : frames.value()) {
    RudpOngoingCtrl complete;
    complete.remote_counter = incoming.remote_counter;
    complete.ack_counter = incoming.ack_counter;
    complete.subtype = incoming.subtype;
    complete.ctrl_frame.assign(bytes.begin(), bytes.end());
    auto message = RudpCtrl::decode_message(crypto, target, material,
                                            crypt_counter, complete);
    const bool failed = !message;
    if (message) crypt_counter = message.value().next_remote_crypt_counter;
    decoded.push_back(std::move(message));
    if (failed) break;
  }
  return decoded;
}

auto RudpCtrl::login_pin_cipher_counter(std::uint64_t crypt_counter) noexcept
    -> std::uint64_t {
  return static_cast<std::uint64_t>(
      static_cast<std::int64_t>(static_cast<std::uint16_t>(crypt_counter)) - 1);
}

auto RudpCtrl::encode_outbound(platform::CryptoProvider& crypto,
                               RegistrationTarget target,
                               const AuthMaterial& material, std::uint16_t type,
                               std::span<const std::byte> plaintext,
                               std::uint64_t crypt_counter,
                               std::uint16_t rudp_counter)
    -> Expected<RudpCtrlOutbound, RudpCtrlOutboundError> {
  if (plaintext.size() > rudp::FrameCodec::frame_max_payload -
                             sizeof(rudp_counter) - ctrl_frame_header_size)
    return unexpected(RudpCtrlOutboundError::too_large);
  auto header = CtrlCodec::encode_frame_header(type, plaintext.size());
  if (!header) return unexpected(RudpCtrlOutboundError::too_large);
  if (!plaintext.empty() &&
      crypt_counter == std::numeric_limits<std::uint64_t>::max())
    return unexpected(RudpCtrlOutboundError::counter_exhausted);
  std::vector<std::byte> payload;
  payload.reserve(sizeof(rudp_counter) + ctrl_frame_header_size +
                  plaintext.size());
  bytes::append_be<std::uint16_t>(payload, rudp_counter);
  payload.insert(payload.end(), header.value().begin(), header.value().end());
  const auto encrypted_at = payload.size();
  payload.resize(encrypted_at + plaintext.size());
  auto next_crypt = crypt_counter;
  if (!plaintext.empty()) {
    const auto cipher_counter = type == ctrl_login_pin_reply_type
                                    ? login_pin_cipher_counter(crypt_counter)
                                    : crypt_counter;
    auto encrypted = material.transform(
        crypto, target, cipher_counter, CipherDirection::encrypt, plaintext,
        std::span{payload}.subspan(encrypted_at));
    if (!encrypted) return unexpected(RudpCtrlOutboundError::crypto_failure);
    ++next_crypt;
  }
  const rudp::Frame frame{
      rudp::FrameCodec::type_value(rudp::FrameType::ctrl_message),
      std::move(payload)};
  auto encoded = rudp::FrameCodec::encode({&frame, 1});
  if (!encoded) return unexpected(RudpCtrlOutboundError::encode_failure);
  return RudpCtrlOutbound{std::move(encoded.value()), next_crypt,
                          static_cast<std::uint16_t>(rudp_counter + 1U)};
}

RudpCtrlHttpExchange::RudpCtrlHttpExchange(
    std::uint16_t next_local_counter, std::uint16_t remote_counter,
    std::span<const std::byte> request, rudp::HandshakeChannel& channel,
    platform::ClockProvider& clock, platform::StopToken stop_token,
    RudpRetryPolicy policy, bool acknowledge_response)
    : next_local_counter_(next_local_counter),
      remote_counter_(remote_counter),
      request_(request),
      channel_(channel),
      clock_(clock),
      stop_token_(std::move(stop_token)),
      policy_(policy),
      acknowledge_response_(acknowledge_response) {}

auto RudpCtrlHttpExchange::carries_ctrl_bytes(std::uint16_t frame_type) noexcept
    -> bool {
  constexpr unsigned lane_mask = 0x0FU;
  const auto lane = [](rudp::Subtype subtype) noexcept {
    return std::to_underlying(subtype) & lane_mask;
  };
  const auto observed = lane(rudp::FrameCodec::subtype_of(frame_type));
  return observed == lane(rudp::Subtype::ctrl_message) ||
         observed == lane(rudp::Subtype::ack_ctrl_offset6);
}

auto RudpCtrlHttpExchange::received_ctrl_bytes(
    std::span<const std::byte> datagram) const -> std::optional<std::size_t> {
  auto frames = rudp::FrameCodec::decode(datagram);
  if (!frames) return std::nullopt;
  std::optional<std::size_t> received;
  for (const auto& frame : frames.value())
    if (carries_ctrl_bytes(frame.type) &&
        frame.payload.size() >= sizeof(std::uint16_t))
      received = frame.payload.size() - sizeof(std::uint16_t);
  return received;
}

auto RudpCtrlHttpExchange::run()
    -> Expected<RudpCtrlHttpExchangeOutcome, RudpCtrlHttpExchangeFailure> {
  std::size_t received_bytes = 0;
  const auto failed = [this, &received_bytes](RudpCtrlHttpExchangeError error) {
    return unexpected(RudpCtrlHttpExchangeFailure{error, received_bytes,
                                                  next_local_counter_});
  };
  for (int sends = 0; sends < rudp_max_sends; ++sends) {
    if (stop_token_.stop_requested())
      return failed(RudpCtrlHttpExchangeError::stopped);
    auto request = rudp::FrameCodec::encode_session_message(
        next_local_counter_, remote_counter_, request_);
    if (!request) return failed(RudpCtrlHttpExchangeError::codec);
    auto sent = channel_.send(request.value());
    if (!sent) return failed(RudpCtrlHttpExchangeError::send_failed);
    if (sent.value() != request.value().size())
      return failed(RudpCtrlHttpExchangeError::short_send);
    next_local_counter_ = static_cast<std::uint16_t>(next_local_counter_ + 1U);
    const auto deadline = clock_.now_microseconds() + policy_.timeout_us;
    for (;;) {
      if (stop_token_.stop_requested())
        return failed(RudpCtrlHttpExchangeError::stopped);
      const auto now = clock_.now_microseconds();
      if (now >= deadline) break;
      auto received = channel_.receive_for(
          std::chrono::microseconds(deadline - now), stop_token_);
      if (!received)
        return failed(stop_token_.stop_requested()
                          ? RudpCtrlHttpExchangeError::stopped
                          : RudpCtrlHttpExchangeError::receive_failed);
      if (!received.value()) continue;
      const auto& datagram = *received.value();
      if (policy_.resend_on_unexpected &&
          datagram.size() <= rudp::FrameCodec::header_bytes)
        return failed(RudpCtrlHttpExchangeError::receive_failed);
      auto response = rudp::FrameCodec::decode_ctrl_http_response_packet(
          {datagram.data(), datagram.size()});
      if (!response) {
        if (const auto partial = received_ctrl_bytes(datagram))
          received_bytes = *partial;
        if (policy_.resend_on_unexpected) break;
        continue;
      }
      received_bytes = response.value().received.size();
      if (stop_token_.stop_requested())
        return failed(RudpCtrlHttpExchangeError::stopped);
      if (acknowledge_response_) {
        auto ack = rudp::FrameCodec::encode_ack_message(
            next_local_counter_, response.value().remote_counter);
        if (!ack) return failed(RudpCtrlHttpExchangeError::codec);
        auto ack_sent = channel_.send(ack.value());
        if (!ack_sent) return failed(RudpCtrlHttpExchangeError::send_failed);
        if (ack_sent.value() != ack.value().size())
          return failed(RudpCtrlHttpExchangeError::short_send);
      }
      remote_counter_ = response.value().remote_counter;
      return RudpCtrlHttpExchangeOutcome{std::move(response.value()),
                                         next_local_counter_, remote_counter_,
                                         received_bytes};
    }
  }
  return failed(RudpCtrlHttpExchangeError::timeout);
}

auto RudpCtrl::finalize_http(const RudpCtrlHttpExchangeOutcome& outcome,
                             platform::CryptoProvider& crypto,
                             RegistrationTarget target,
                             const AuthMaterial& material)
    -> Expected<RemoteCtrlHttpFinal, RemoteCtrlHttpFinalizeError> {
  const auto& received = outcome.response.received;
  auto parsed = CtrlCodec::parse_response(received);
  if (!parsed) {
    switch (parsed.error()) {
      case CtrlResponseError::incomplete:
        return unexpected(RemoteCtrlHttpFinalizeError::incomplete);
      case CtrlResponseError::too_large:
        return unexpected(RemoteCtrlHttpFinalizeError::too_large);
      case CtrlResponseError::invalid:
        break;
    }
    return unexpected(RemoteCtrlHttpFinalizeError::invalid);
  }
  if (!parsed.value())
    return unexpected(RemoteCtrlHttpFinalizeError::nonaccepted);
  const auto& grant = parsed.value().value();
  RemoteCtrlHttpFinal result{};
  result.response =
      CtrlCodec::finalize_response(grant, crypto, target, material);
  result.next_local_counter = outcome.next_local_counter;
  result.remote_counter = outcome.remote_counter;
  const auto header_size = grant.header_size();
  if (header_size > received.size())
    return unexpected(RemoteCtrlHttpFinalizeError::invalid);
  result.trailing_received.assign(
      received.begin() + static_cast<std::ptrdiff_t>(header_size),
      received.end());
  return result;
}

void RudpCtrlReceiver::record_packet(
    std::span<const std::byte> packet) noexcept {
  evidence_.packet_bytes = packet.size();
  if (packet.size() >= rudp::FrameCodec::header_bytes)
    evidence_.first_type =
        bytes::load_be<std::uint16_t>(packet.subspan(first_frame_type_offset));
}

auto RudpCtrlReceiver::acknowledge(rudp::HandshakeChannel& channel,
                                   std::uint16_t local_counter,
                                   std::uint16_t remote_counter)
    -> Expected<void, RudpCtrlSwitchStepError> {
  auto ack =
      rudp::FrameCodec::encode_ack_message(local_counter, remote_counter);
  if (!ack) return unexpected(RudpCtrlSwitchStepError::ack_encode_failed);
  auto sent = channel.send(ack.value());
  if (!sent || sent.value() != ack.value().size())
    ++evidence_.ack_send_failures;
  return {};
}

auto RudpCtrlReceiver::transport_result(const RudpOngoingCtrl& frame,
                                        const RudpCtrlSwitchStepInput& input,
                                        RudpCtrlDelivery delivery)
    -> RudpCtrlSwitchStepResult {
  RudpCtrlSwitchStepResult result{};
  result.attempt_id = input.attempt_id;
  result.delivery = delivery;
  result.message.ack_counter = frame.ack_counter;
  result.message.next_remote_crypt_counter = input.remote_crypt_counter;
  result.message.remote_counter = frame.remote_counter;
  return result;
}

auto RudpCtrlReceiver::step(rudp::HandshakeChannel& channel,
                            RudpCtrlSwitchStepInput input,
                            std::chrono::microseconds timeout,
                            const platform::StopToken& stop)
    -> Expected<std::vector<RudpCtrlSwitchStepResult>,
                RudpCtrlSwitchStepError> {
  evidence_ = {};
  if (stop.stop_requested())
    return unexpected(RudpCtrlSwitchStepError::stopped);
  auto received = channel.receive_for(timeout, stop);
  if (!received)
    return unexpected(stop.stop_requested()
                          ? RudpCtrlSwitchStepError::stopped
                          : RudpCtrlSwitchStepError::receive_failed);
  if (!received.value())
    return unexpected(stop.stop_requested() ? RudpCtrlSwitchStepError::stopped
                                            : RudpCtrlSwitchStepError::timeout);
  const auto& packet = *received.value();
  record_packet(packet);
  if (rudp::FrameCodec::is_finish_packet(packet))
    return unexpected(RudpCtrlSwitchStepError::peer_closed);
  auto frames = RudpCtrl::decode_ongoing(packet);
  if (!frames) {
    evidence_.framing_error = frames.error();
    return unexpected(RudpCtrlSwitchStepError::malformed);
  }
  evidence_.frame_count = frames.value().size();
  std::vector<RudpCtrlSwitchStepResult> results;
  results.reserve(frames.value().size());
  for (const auto& frame : frames.value()) {
    if (stop.stop_requested())
      return unexpected(RudpCtrlSwitchStepError::stopped);
    evidence_.frames_seen.emplace_back(frame.subtype, frame.remote_counter);
    if (frame.subtype == rudp::Subtype::ack || frame.ctrl_frame.empty()) {
      const auto delivery = frame.subtype == rudp::Subtype::ack
                                ? RudpCtrlDelivery::acknowledgement
                                : RudpCtrlDelivery::transport;
      auto result = transport_result(frame, input, delivery);
      result.message.subtype = frame.subtype;
      if (delivery == RudpCtrlDelivery::transport) {
        if (auto acked =
                acknowledge(channel, input.local_counter, frame.remote_counter);
            !acked)
          return unexpected(acked.error());
        ++evidence_.transport_only_frames;
      }
      results.push_back(std::move(result));
      continue;
    }
    if (auto acked =
            acknowledge(channel, input.local_counter, frame.remote_counter);
        !acked)
      return unexpected(acked.error());
    auto decoded =
        input.byte_stream.decode(frame, input.crypto, input.target,
                                 input.material, input.remote_crypt_counter);
    std::size_t delivered = 0;
    for (auto& message : decoded) {
      if (!message) {
        if (message.error() == RudpCtrlMessageError::counter_exhausted)
          return unexpected(RudpCtrlSwitchStepError::counter_exhausted);
        ++evidence_.dropped_frames;
        evidence_.frame_error = message.error();
        continue;
      }
      const bool switched =
          std::holds_alternative<CtrlStreamSwitchAck>(message.value().message);
      input.remote_crypt_counter = message.value().next_remote_crypt_counter;
      results.push_back(
          {std::move(message.value()), input.attempt_id, switched});
      ++delivered;
    }
    if (delivered == 0)
      results.push_back(
          transport_result(frame, input, RudpCtrlDelivery::transport));
  }
  return results;
}

}  // namespace libchiaking::net::protocol::remote_play
