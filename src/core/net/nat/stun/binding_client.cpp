#include "src/core/net/nat/stun/binding_client.hpp"

#include "src/core/utils/timing/deadline.hpp"

namespace libchiaking::net::nat::stun {

BindingClient::BindingClient(platform::UdpSocket& socket,
                             platform::Poller& poller,
                             const platform::ClockProvider& clock) noexcept
    : socket_(socket), poller_(poller), clock_(clock) {}

auto BindingClient::stopped(const platform::StopToken& stop_token) noexcept
    -> Error {
  return stop_token.deadline_expired() ? Error::timeout : Error::cancelled;
}

auto BindingClient::request(const TransactionId& transaction) noexcept
    -> std::array<std::byte, header_size> {
  return Header{Header::binding_type(MessageClass::request), 0, magic_cookie,
                transaction}
      .encode();
}

auto BindingClient::mapped_endpoint(std::span<const std::byte> response,
                                    const TransactionId& transaction)
    -> Expected<platform::SocketEndpoint, ResponseError> {
  const auto header = Header::read(response);
  if (!header) return unexpected(ResponseError::short_packet);
  if (header->type != Header::binding_type(MessageClass::success))
    return unexpected(ResponseError::invalid_type);
  if (header->body_size != response.size() - header_size)
    return unexpected(ResponseError::invalid_length);
  if (header->cookie != magic_cookie)
    return unexpected(ResponseError::invalid_cookie);
  if (header->transaction != transaction)
    return unexpected(ResponseError::invalid_transaction);

  AttributeReader reader{response};
  while (!reader.exhausted()) {
    const auto item = reader.next();
    if (!item) return unexpected(ResponseError::invalid_attribute);
    if (item->type != attribute::mapped_address &&
        item->type != attribute::xor_mapped_address)
      continue;
    const auto endpoint = item->endpoint(transaction);
    if (!endpoint) return unexpected(ResponseError::invalid_attribute);
    return *endpoint;
  }
  return unexpected(ResponseError::no_mapped_address);
}

auto BindingClient::answer(std::span<const std::byte> datagram,
                           const TransactionId& transaction)
    -> Expected<std::optional<platform::SocketEndpoint>, ResponseError> {
  const auto decoded = mapped_endpoint(datagram, transaction);
  if (!decoded) {
    if (decoded.error() == ResponseError::invalid_transaction)
      return std::optional<platform::SocketEndpoint>{};
    return unexpected(decoded.error());
  }
  return std::optional<platform::SocketEndpoint>{decoded.value()};
}

auto BindingClient::query(const platform::SocketEndpoint& server,
                          const TransactionId& transaction,
                          const platform::StopToken& stop_token,
                          std::chrono::nanoseconds timeout)
    -> Expected<platform::SocketEndpoint, Error> {
  using platform::PollEvent;
  using platform::PollRequest;
  if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
  if (timeout <= std::chrono::nanoseconds::zero())
    return unexpected(Error::timeout);
  if (&socket_.owner() != &poller_.owner() || server.port == 0 ||
      server.address.is_unspecified())
    return unexpected(Error::invalid_input);
  const auto local = socket_.local_endpoint();
  if (!local || local.value().port == 0 ||
      server.address.family() != local.value().address.family())
    return unexpected(Error::invalid_input);
  const auto datagram = request(transaction);
  const auto sent = socket_.send_to(datagram, server);
  if (!sent || sent.value() != datagram.size())
    return unexpected(Error::send_failed);

  const auto timeout_us =
      std::chrono::duration_cast<std::chrono::microseconds>(timeout);
  if (timeout_us.count() <= 0) return unexpected(Error::timeout);
  const auto deadline = timing::Deadline::after(
      clock_, timing::Deadline::Duration{
                  static_cast<std::uint64_t>(timeout_us.count())});
  const PollRequest requests[] = {
      {.pollable = &socket_, .tag = 1U, .interest = PollEvent::readable}};
  std::array<std::byte, receive_buffer_size> buffer{};
  for (;;) {
    if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
    const auto now = timing::Deadline::now(clock_);
    if (deadline.expired(now)) return unexpected(Error::timeout);
    const auto ready =
        poller_.wait(requests,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         *deadline.remaining(now)),
                     stop_token);
    if (!ready) return unexpected(Error::net_failed);
    if (ready.value().stop_requested) return unexpected(stopped(stop_token));
    for (const auto& event : ready.value().ready) {
      if (event.tag != 1U ||
          (event.ready & PollEvent::readable) == PollEvent::none)
        continue;
      const auto received = socket_.receive_from(buffer);
      if (!received) {
        if (received.error() == platform::NetError::would_block) continue;
        return unexpected(Error::net_failed);
      }
      if (received.value().size > buffer.size())
        return unexpected(Error::protocol);
      const auto filtered = answer(
          std::span<const std::byte>{buffer.data(), received.value().size},
          transaction);
      if (!filtered) return unexpected(Error::protocol);
      if (filtered.value()) return *filtered.value();
    }
  }
}

}  // namespace libchiaking::net::nat::stun
