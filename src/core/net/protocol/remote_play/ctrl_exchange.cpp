#include "ctrl_exchange.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"

namespace libchiaking::net::protocol::remote_play {

auto CtrlStartupReply::parse(std::span<const std::byte> received)
    -> Expected<std::optional<CtrlGrant>, ConsoleExchangeError> {
  auto parsed = CtrlCodec::parse_response(received);
  if (!parsed) {
    switch (parsed.error()) {
      case CtrlResponseError::incomplete:
        return std::optional<CtrlGrant>{};
      case CtrlResponseError::too_large:
        return unexpected(ConsoleExchangeError::too_large);
      case CtrlResponseError::invalid:
        break;
    }
    return unexpected(ConsoleExchangeError::invalid_response);
  }
  if (!parsed.value()) return unexpected(ConsoleExchangeError::refused);
  return std::optional<CtrlGrant>{std::move(parsed.value().value())};
}

auto CtrlStartupReply::take(std::unique_ptr<platform::TcpSocket> socket,
                            CtrlGrant response) const -> CtrlStartupConnection {
  CtrlStartupConnection connection;
  connection.socket = std::move(socket);
  connection.response = std::move(response);
  connection.next_local_crypt_counter = next_local_counter_;
  return connection;
}

CtrlFrameReader::CtrlFrameReader(platform::CryptoProvider& crypto) noexcept
    : crypto_(crypto) {}

CtrlFrameReader::~CtrlFrameReader() { (void)cancel(); }

auto CtrlFrameReader::begin(std::unique_ptr<platform::TcpSocket> socket,
                            RegistrationTarget target, AuthMaterial&& material,
                            std::uint64_t initial_remote_counter,
                            std::uint64_t initial_local_counter)
    -> Expected<void, CtrlReaderError> {
  HeldTcpSocket held{std::move(socket)};
  if (started_ || terminal_) {
    static_cast<void>(held.close());
    return unexpected(CtrlReaderError::invalid_state);
  }
  if (!RpVersion::of(target)) {
    static_cast<void>(held.close());
    return unexpected(CtrlReaderError::invalid_argument);
  }
  if (!held) return unexpected(CtrlReaderError::invalid_argument);
  socket_ = std::move(held);
  target_ = target;
  material_ = std::move(material);
  remote_counter_ = initial_remote_counter;
  local_counter_ = initial_local_counter;
  started_ = true;
  return {};
}

auto CtrlFrameReader::adopt(CtrlStartupConnection&& connection,
                            RegistrationTarget target, AuthMaterial&& material)
    -> Expected<CtrlResponseFinal, CtrlReaderError> {
  if (started_ || terminal_ || !connection.socket) {
    static_cast<void>(HeldTcpSocket{std::move(connection.socket)}.close());
    connection.response = CtrlGrant{};
    material = AuthMaterial{};
    return unexpected(CtrlReaderError::invalid_state);
  }
  auto final = CtrlCodec::finalize_response(connection.response, crypto_,
                                            target, material);
  connection.response = CtrlGrant{};
  const auto began = begin(std::move(connection.socket), target,
                           std::move(material), final.next_remote_counter,
                           connection.next_local_crypt_counter);
  if (!began) {
    material = AuthMaterial{};
    (void)cancel();
    return unexpected(began.error());
  }
  return final;
}

auto CtrlFrameReader::pump_once()
    -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError> {
  if (!started_ || terminal_) return unexpected(CtrlReaderError::invalid_state);
  return parse_or_receive();
}

auto CtrlFrameReader::queue_send(std::uint16_t type,
                                 std::span<const std::byte> payload)
    -> Expected<void, CtrlReaderError> {
  if (!started_ || terminal_) return unexpected(CtrlReaderError::invalid_state);
  const auto header = CtrlCodec::encode_frame_header(type, payload.size());
  if (!header) return unexpected(CtrlReaderError::too_large);
  if (!payload.empty() &&
      local_counter_ == std::numeric_limits<std::uint64_t>::max())
    return unexpected(CtrlReaderError::counter_exhausted);
  const auto frame_start = outbound_.size();
  outbound_.resize(frame_start + ctrl_frame_header_size + payload.size());
  const auto frame = outbound_.span().subspan(frame_start);
  std::ranges::copy(header.value(), frame.begin());
  if (!payload.empty()) {
    const auto encrypted = material_.transform(
        crypto_, target_, local_counter_, CipherDirection::encrypt, payload,
        frame.subspan(ctrl_frame_header_size));
    if (!encrypted) {
      outbound_.resize(frame_start);
      return unexpected(CtrlReaderError::crypto_failure);
    }
    ++local_counter_;
  }
  outbound_size_ = outbound_.size();
  sending_ = true;
  return {};
}

auto CtrlFrameReader::pump_send() -> Expected<bool, CtrlReaderError> {
  if (!started_ || terminal_ || !sending_)
    return unexpected(CtrlReaderError::invalid_state);
  const auto sent = socket_->send(
      {outbound_.data() + outbound_sent_, outbound_size_ - outbound_sent_});
  if (!sent) {
    if (sent.error() == platform::NetError::would_block) return false;
    drop_outbound();
    return unexpected(CtrlReaderError::network_failure);
  }
  if (sent.value() == 0 || sent.value() > outbound_size_ - outbound_sent_) {
    drop_outbound();
    return unexpected(CtrlReaderError::network_failure);
  }
  outbound_sent_ += sent.value();
  if (outbound_sent_ != outbound_size_) return false;
  drop_outbound();
  return true;
}

void CtrlFrameReader::drop_outbound() noexcept {
  outbound_.clear();
  outbound_size_ = 0;
  outbound_sent_ = 0;
  sending_ = false;
}

auto CtrlFrameReader::parse_or_receive()
    -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError> {
  auto parsed = CtrlCodec::decode_frame({buffer_.data(), filled_});
  if (!parsed && parsed.error() == CtrlFrameError::too_large)
    return fail(CtrlReaderError::too_large);
  if (!parsed) {
    if (filled_ == buffer_.size()) return fail(CtrlReaderError::too_large);
    const auto received =
        socket_->receive({buffer_.data() + filled_, buffer_.size() - filled_});
    if (!received) {
      if (received.error() == platform::NetError::would_block)
        return std::optional<CtrlPlainFrame>{};
      return fail(received.error() == platform::NetError::closed
                      ? CtrlReaderError::disconnected
                      : CtrlReaderError::network_failure);
    }
    if (received.value() == 0) return fail(CtrlReaderError::disconnected);
    if (received.value() > buffer_.size() - filled_)
      return fail(CtrlReaderError::network_failure);
    filled_ += received.value();
    parsed = CtrlCodec::decode_frame({buffer_.data(), filled_});
    if (!parsed && parsed.error() == CtrlFrameError::too_large)
      return fail(CtrlReaderError::too_large);
    if (!parsed) return std::optional<CtrlPlainFrame>{};
  }

  CtrlPlainFrame frame;
  frame.type_ = parsed.value().type;
  frame.size_ = parsed.value().payload.size();
  if (parsed.value().consumed > filled_ || frame.size_ > ctrl_frame_max_payload)
    return fail(CtrlReaderError::too_large);
  if (frame.size_ != 0) {
    if (remote_counter_ == std::numeric_limits<std::uint64_t>::max())
      return fail(CtrlReaderError::counter_exhausted);
    const auto decrypted = material_.transform(
        crypto_, target_, remote_counter_, CipherDirection::decrypt,
        parsed.value().payload, frame.payload_.span().first(frame.size_));
    ++remote_counter_;
    if (!decrypted) {
      consume(parsed.value().consumed);
      ++dropped_frames_;
      return std::optional<CtrlPlainFrame>{};
    }
  }
  consume(parsed.value().consumed);
  return std::optional<CtrlPlainFrame>{std::move(frame)};
}

void CtrlFrameReader::consume(std::size_t consumed) noexcept {
  buffer_.drop_front(consumed, filled_);
  filled_ -= consumed;
}

auto CtrlFrameReader::cancel() -> Expected<void, CtrlReaderError> {
  const auto closed = close_socket();
  buffer_.clear();
  outbound_.clear();
  material_ = AuthMaterial{};
  filled_ = 0;
  remote_counter_ = 0;
  local_counter_ = 0;
  outbound_size_ = 0;
  outbound_sent_ = 0;
  sending_ = false;
  terminal_ = true;
  return closed;
}

auto CtrlFrameReader::fail(CtrlReaderError error)
    -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError> {
  (void)cancel();
  return unexpected(error);
}

auto CtrlFrameReader::close_socket() -> Expected<void, CtrlReaderError> {
  if (!socket_.close()) return unexpected(CtrlReaderError::network_failure);
  return {};
}

}  // namespace libchiaking::net::protocol::remote_play
