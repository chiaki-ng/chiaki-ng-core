#include "src/core/net/transport/datagram/udp_transport.hpp"

#include <algorithm>
#include <array>

namespace libchiaking::net::transport::datagram {

bool ProviderUdpTransport::peer_refusal(platform::NetError error) noexcept {
  return error == platform::NetError::connection_refused ||
         error == platform::NetError::connection_reset;
}

TransportError ProviderUdpTransport::map_error(
    platform::NetError error) noexcept {
  switch (error) {
    case platform::NetError::would_block:
      return TransportError::would_block;
    case platform::NetError::closed:
      return TransportError::closed;
    default:
      return TransportError::io_failure;
  }
}

auto ProviderUdpTransport::raise_receive_window(
    platform::UdpSocket& socket) noexcept -> std::optional<std::size_t> {
  const auto current = socket.receive_buffer_size();
  if ((!current || current.value() < socket_receive_buffer) &&
      !socket.set_receive_buffer_size(socket_receive_buffer))
    static_cast<void>(socket.set_receive_buffer_size(socket_receive_window));
  const auto raised = socket.receive_buffer_size();
  if (!raised) return std::nullopt;
  return raised.value();
}

ProviderUdpTransport::ProviderUdpTransport(
    platform::NetworkProvider& network,
    std::unique_ptr<platform::UdpSocket> socket,
    std::optional<platform::SocketEndpoint> peer) noexcept
    : network_(&network),
      socket_(std::move(socket)),
      peer_(peer),
      receive_buffer_(network.capabilities().costly_socket_calls
                          ? compact_receive_buffer_size
                          : default_receive_buffer_size) {}

ProviderUdpTransport::~ProviderUdpTransport() {
  if (!closed_) (void)socket_->close();
}

auto ProviderUdpTransport::create(platform::NetworkProvider& network,
                                  const UdpTransportConfig& config)
    -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError> {
  auto socket = network.sockets().create_udp_socket(config.local.address);
  if (!socket) return unexpected(map_error(socket.error()));
  if (config.dont_fragment) {
    const auto configured = socket.value()->set_boolean_option(
        platform::SocketOption::dont_fragment, *config.dont_fragment);
    if (!configured && *config.dont_fragment)
      return unexpected(map_error(configured.error()));
  }
  const auto bound = socket.value()->bind(config.local);
  if (!bound) return unexpected(map_error(bound.error()));
  bool connected = false;
  if (config.connect_peer && config.peer) {
    const auto attached = socket.value()->connect(*config.peer);
    if (!attached && attached.error() != platform::NetError::not_supported)
      return unexpected(map_error(attached.error()));
    connected = static_cast<bool>(attached);
  }
  auto transport = adopt(network, std::move(socket.value()), config.peer);
  if (transport) transport.value()->connected_ = connected;
  return transport;
}

auto ProviderUdpTransport::adopt(platform::NetworkProvider& network,
                                 std::unique_ptr<platform::UdpSocket> socket,
                                 std::optional<platform::SocketEndpoint> peer)
    -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError> {
  if (!socket) return unexpected(TransportError::io_failure);
  const auto window = raise_receive_window(*socket);
  std::unique_ptr<ProviderUdpTransport> transport{
      new ProviderUdpTransport{network, std::move(socket), peer}};
  transport->kernel_receive_buffer_ = window;
  return transport;
}

auto ProviderUdpTransport::adopt_connected(
    platform::NetworkProvider& network,
    std::unique_ptr<platform::UdpSocket> socket, platform::SocketEndpoint peer)
    -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError> {
  if (!socket) return unexpected(TransportError::io_failure);
  const auto attached = socket->connect(peer);
  if (!attached && attached.error() != platform::NetError::not_supported)
    return unexpected(map_error(attached.error()));
  auto transport = adopt(network, std::move(socket), peer);
  if (transport) {
    transport.value()->connected_ = static_cast<bool>(attached);
    transport.value()->ignore_peer_refusals_ = static_cast<bool>(attached);
  }
  return transport;
}

auto ProviderUdpTransport::send(std::span<const std::byte> datagram)
    -> Expected<std::size_t, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  if (!peer_) return unexpected(TransportError::io_failure);
  auto sent = connected_ ? socket_->send(datagram)
                         : socket_->send_to(datagram, peer_.value());
  for (unsigned attempt = 0;
       ignore_peer_refusals_ && !sent && peer_refusal(sent.error()) &&
       attempt < max_refusals_per_call;
       ++attempt)
    sent = socket_->send(datagram);
  if (!sent) return unexpected(map_error(sent.error()));
  if (sent.value() != datagram.size())
    return unexpected(TransportError::io_failure);
  return sent.value();
}

auto ProviderUdpTransport::receive()
    -> Expected<std::optional<Datagram>, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  last_traffic_class_.reset();
  auto received = socket_->receive_from(receive_buffer_);
  for (unsigned attempt = 0;
       ignore_peer_refusals_ && !received && peer_refusal(received.error()) &&
       attempt < max_refusals_per_call;
       ++attempt)
    received = socket_->receive_from(receive_buffer_);
  if (!received) {
    if (ignore_peer_refusals_ && peer_refusal(received.error()))
      return Expected<std::optional<Datagram>, TransportError>{std::nullopt};
    const auto error = map_error(received.error());
    if (error == TransportError::would_block)
      return Expected<std::optional<Datagram>, TransportError>{std::nullopt};
    return unexpected(error);
  }
  if (received.value().size > receive_buffer_.size())
    return unexpected(TransportError::io_failure);
  if (peer_ && received.value().source != peer_.value())
    return Expected<std::optional<Datagram>, TransportError>{std::nullopt};
  last_traffic_class_ = received.value().traffic_class;
  return Expected<std::optional<Datagram>, TransportError>{
      Datagram{receive_buffer_}.first(received.value().size)};
}

auto ProviderUdpTransport::enable_traffic_class()
    -> Expected<void, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  if (auto enabled = socket_->receive_traffic_class(true); !enabled)
    return unexpected(map_error(enabled.error()));
  return Expected<void, TransportError>{};
}

auto ProviderUdpTransport::last_traffic_class() const noexcept
    -> std::optional<std::uint8_t> {
  return last_traffic_class_;
}

auto ProviderUdpTransport::close() -> Expected<void, TransportError> {
  if (closed_) return Expected<void, TransportError>{};
  closed_ = true;
  const auto closed = socket_->close();
  if (!closed && closed.error() != platform::NetError::closed)
    return unexpected(map_error(closed.error()));
  return Expected<void, TransportError>{};
}

auto ProviderUdpTransport::pollable() const noexcept
    -> const platform::Pollable* {
  if (closed_) return nullptr;
  return socket_.get();
}

auto ProviderUdpTransport::kernel_receive_buffer() const noexcept
    -> std::optional<std::size_t> {
  return kernel_receive_buffer_;
}

auto ProviderUdpTransport::set_dont_fragment(bool enabled)
    -> Expected<void, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  const auto configured = socket_->set_boolean_option(
      platform::SocketOption::dont_fragment, enabled);
  if (!configured) return unexpected(map_error(configured.error()));
  return Expected<void, TransportError>{};
}

auto ProviderUdpTransport::set_peer(platform::SocketEndpoint peer)
    -> Expected<void, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  if (connected_) {
    const auto attached = socket_->connect(peer);
    if (!attached) return unexpected(map_error(attached.error()));
  }
  peer_ = peer;
  return Expected<void, TransportError>{};
}

auto ProviderUdpTransport::local_endpoint() const
    -> Expected<platform::SocketEndpoint, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  const auto endpoint = socket_->local_endpoint();
  if (!endpoint) return unexpected(map_error(endpoint.error()));
  return endpoint.value();
}

auto ProviderUdpTransport::wait_readable(std::chrono::nanoseconds timeout,
                                         const platform::StopToken& stop_token)
    -> Expected<bool, TransportError> {
  return wait_for(platform::PollEvent::readable, timeout, stop_token);
}

auto ProviderUdpTransport::wait_writable(std::chrono::microseconds timeout)
    -> Expected<bool, TransportError> {
  return wait_for(platform::PollEvent::writable, timeout,
                  platform::StopToken{});
}

auto ProviderUdpTransport::wait_for(platform::PollEvent event,
                                    std::chrono::nanoseconds timeout,
                                    const platform::StopToken& stop_token)
    -> Expected<bool, TransportError> {
  if (closed_) return unexpected(TransportError::closed);
  if (!poller_) {
    auto poller = network_->pollers().create_poller();
    if (!poller) return unexpected(map_error(poller.error()));
    poller_ = std::move(poller.value());
  }
  const std::array<platform::PollRequest, 1> requests{
      {{socket_.get(), 1, event}}};
  const auto result = poller_->wait(requests, timeout, stop_token);
  if (!result) return unexpected(map_error(result.error()));
  return std::ranges::any_of(
      result.value().ready, [event](const auto& readiness) {
        return (readiness.ready & event) != platform::PollEvent::none;
      });
}

}  // namespace libchiaking::net::transport::datagram
