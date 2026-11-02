#include "src/core/consoles/adapters/pairing/pairing_exchange.hpp"

#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "src/core/net/transport/http/status.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

PairingExchange::PairingExchange(platform::SocketProvider& sockets,
                                 platform::CryptoProvider& crypto,
                                 const platform::ClockProvider& clock) noexcept
    : sockets_(sockets), crypto_(crypto), clock_(clock) {}

PairingExchange::~PairingExchange() { (void)cancel(); }

auto PairingExchange::known(
    net::protocol::remote_play::RegistrationTarget target) noexcept -> bool {
  switch (target) {
    case net::protocol::remote_play::RegistrationTarget::ps4_unknown:
    case net::protocol::remote_play::RegistrationTarget::ps4_8:
    case net::protocol::remote_play::RegistrationTarget::ps4_9:
    case net::protocol::remote_play::RegistrationTarget::ps4_10:
    case net::protocol::remote_play::RegistrationTarget::ps5_1:
      return true;
  }
  return false;
}

auto PairingExchange::stage(
    std::span<const std::byte> header, std::span<const std::byte> payload,
    net::protocol::remote_play::RegistrationTarget target)
    -> Expected<void, Error> {
  if (socket_) return unexpected(Error::invalid_state);
  if (header.empty() || header.size() > header_limit || payload.empty() ||
      payload.size() > payload_limit || !known(target))
    return unexpected(Error::invalid_argument);
  std::ranges::copy(header, header_.begin());
  std::ranges::copy(payload, payload_.begin());
  header_size_ = header.size();
  payload_size_ = payload.size();
  target_ = target;
  return {};
}

void PairingExchange::unstage() noexcept {
  if (!socket_) forget_request();
}

auto PairingExchange::begin(
    platform::SocketEndpoint console,
    net::protocol::remote_play::RegistrationMaterial&& material)
    -> Expected<Progress, Error> {
  if (socket_) return unexpected(Error::invalid_state);
  if (console.port == 0 || header_size_ == 0 || payload_size_ == 0)
    return unexpected(Error::invalid_argument);
  material_ = std::move(material);

  auto created = sockets_.create_tcp_socket(console.address);
  if (!created) {
    (void)cancel();
    return unexpected(Error::network_failure);
  }
  socket_ = std::move(created.value());
  const auto connected = socket_->connect(console);
  if (!connected && connected.error() != platform::NetError::would_block) {
    (void)cancel();
    return unexpected(Error::network_failure);
  }
  reset_deadline();
  connect_deadline_ = deadline_;
  if (connected) return Connected{};
  return Waiting{};
}

auto PairingExchange::await_connection() -> Expected<Progress, Error> {
  if (!socket_) return unexpected(Error::invalid_state);
  if (deadline_.expired(clock_)) return fail(Error::timeout);
  const auto connected = socket_->complete_connect();
  if (!connected) {
    if (connected.error() == platform::NetError::would_block) return Waiting{};
    return fail(Error::network_failure);
  }
  const auto peer = socket_->peer_endpoint();
  if (!peer) {
    if (peer.error() == platform::NetError::not_connected ||
        peer.error() == platform::NetError::would_block)
      return Waiting{};
    return fail(Error::network_failure);
  }
  reset_deadline();
  return Connected{};
}

auto PairingExchange::send_header() -> Expected<Progress, Error> {
  return send_part({header_.data(), header_size_})
      .and_then([this](Delivery delivery) -> Expected<Progress, Error> {
        switch (delivery) {
          case Delivery::partial:
            return Waiting{};
          case Delivery::not_connected:
            deadline_ = connect_deadline_;
            return ConnectionPending{};
          case Delivery::complete:
            break;
        }
        header_.clear();
        reset_deadline();
        return HeaderSent{};
      });
}

auto PairingExchange::send_payload() -> Expected<Progress, Error> {
  return send_part({payload_.data(), payload_size_})
      .and_then([this](Delivery delivery) -> Expected<Progress, Error> {
        switch (delivery) {
          case Delivery::partial:
            return Waiting{};
          case Delivery::not_connected:
            return fail(Error::network_failure);
          case Delivery::complete:
            break;
        }
        payload_.clear();
        reset_deadline();
        return RequestSent{};
      });
}

auto PairingExchange::send_part(std::span<const std::byte> part)
    -> Expected<Delivery, Error> {
  if (!socket_) return unexpected(Error::invalid_state);
  if (deadline_.expired(clock_)) return fail(Error::timeout);
  const auto written = socket_->send(part.subspan(sent_));
  if (!written) {
    if (written.error() == platform::NetError::would_block)
      return Delivery::partial;
    if (written.error() == platform::NetError::not_connected)
      return Delivery::not_connected;
    return fail(Error::network_failure);
  }
  if (written.value() == 0 || written.value() > part.size() - sent_)
    return fail(Error::network_failure);
  sent_ += written.value();
  if (sent_ < part.size()) return Delivery::partial;
  sent_ = 0;
  return Delivery::complete;
}

auto PairingExchange::read_reply_header() -> Expected<Progress, Error> {
  const auto received = receive_up_to(reply_.size());
  if (!received) return unexpected(received.error());
  if (!received.value()) return Waiting{};
  return parse_header();
}

auto PairingExchange::read_reply_body() -> Expected<Progress, Error> {
  const auto received = receive_up_to(reply_size_);
  if (!received) return unexpected(received.error());
  if (filled_ < reply_size_) return Waiting{};
  return finish();
}

auto PairingExchange::receive_up_to(std::size_t limit)
    -> Expected<bool, Error> {
  if (!socket_) return unexpected(Error::invalid_state);
  if (filled_ >= limit) return fail(Error::too_large);
  const bool expired = deadline_.expired(clock_);
  const auto received = socket_->receive(
      std::span<std::byte>{reply_.data() + filled_, limit - filled_});
  if (!received) {
    if (received.error() == platform::NetError::would_block && !expired)
      return false;
    return fail(received.error() == platform::NetError::would_block
                    ? Error::timeout
                : received.error() == platform::NetError::closed
                    ? Error::truncated
                    : Error::network_failure);
  }
  if (received.value() == 0) return fail(Error::truncated);
  if (received.value() > limit - filled_) return fail(Error::network_failure);
  filled_ += received.value();
  return true;
}

auto PairingExchange::parse_header() -> Expected<Progress, Error> {
  const auto header =
      net::protocol::remote_play::RegistrationResponseCodec::parse_header(
          std::span<const std::byte>{reply_.data(), filled_});
  if (!header) {
    if (header.error() ==
        net::protocol::remote_play::ResponseHeaderError::incomplete)
      return Waiting{};
    return fail(
        header.error() ==
                net::protocol::remote_play::ResponseHeaderError::too_large
            ? Error::too_large
            : Error::invalid_header);
  }
  ps_reason_ = header.value().application_reason;
  http_status_ = header.value().status;
  if (net::HttpStatus{header.value().status} != net::HttpStatus::ok)
    return fail(Error::refused);
  reply_header_size_ = header.value().header_size;
  reply_size_ = reply_header_size_ + header.value().content_size;
  if (reply_size_ > reply_.size()) return fail(Error::too_large);
  reset_deadline();
  if (filled_ >= reply_size_) return finish();
  return ReplyHeaderAccepted{};
}

auto PairingExchange::finish() -> Expected<Progress, Error> {
  const auto body = std::span<std::byte>{reply_.data() + reply_header_size_,
                                         filled_ - reply_header_size_};
  const auto decrypted =
      net::protocol::remote_play::RegistrationCipher::transform(
          crypto_, target_, {material_.bright.data(), material_.bright.size()},
          {material_.ambassador.data(), material_.ambassador.size()}, 0,
          net::protocol::remote_play::CipherDirection::decrypt, body, body);
  if (!decrypted) return fail(Error::crypto_failure);
  auto parsed =
      net::protocol::remote_play::RegistrationResponseCodec::parse_payload(
          target_, body);
  if (!parsed) return fail(Error::invalid_payload);
  auto console = std::move(parsed.value());
  const auto closed = cancel();
  if (!closed) return unexpected(closed.error());
  return Progress{std::move(console)};
}

auto PairingExchange::cancel() -> Expected<void, Error> {
  const auto closed = close();
  forget_request();
  reply_.clear();
  material_ = net::protocol::remote_play::RegistrationMaterial{};
  sent_ = 0;
  filled_ = 0;
  reply_header_size_ = 0;
  reply_size_ = 0;
  return closed;
}

void PairingExchange::forget_request() noexcept {
  header_.clear();
  payload_.clear();
  header_size_ = 0;
  payload_size_ = 0;
}

void PairingExchange::reset_deadline() noexcept {
  deadline_ = timing::Deadline::after(
      clock_,
      timing::Deadline::Duration{net::protocol::remote_play::phase_timeout});
}

auto PairingExchange::fail(Error error) -> Unexpected<Error> {
  (void)cancel();
  return unexpected(error);
}

auto PairingExchange::close() -> Expected<void, Error> {
  if (!socket_) return {};
  auto socket = std::move(socket_);
  const auto closed = socket->close();
  if (!closed && closed.error() != platform::NetError::closed)
    return unexpected(Error::network_failure);
  return {};
}

}  // namespace libchiaking::consoles
