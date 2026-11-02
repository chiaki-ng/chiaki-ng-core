#include "src/core/consoles/adapters/pairing/pairing_knock.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

auto PairingKnock::datagram(Generation generation) noexcept
    -> std::optional<Datagram> {
  switch (generation) {
    case Generation::ps4:
      return ps4_datagram;
    case Generation::ps5:
      return ps5_datagram;
  }
  return std::nullopt;
}

auto PairingKnock::answers(Generation generation,
                           std::span<const std::byte> reply) noexcept -> bool {
  if (generation != Generation::ps4 && generation != Generation::ps5)
    return false;
  if (reply.size() < answer_size) return false;
  const auto& expected =
      generation == Generation::ps4 ? ps4_answer : ps5_answer;
  return std::ranges::equal(reply.first(answer_size), expected);
}

PairingKnock::PairingKnock(platform::NetworkProvider& network,
                           const platform::ClockProvider& clock) noexcept
    : network_(network), clock_(clock) {}

PairingKnock::~PairingKnock() { (void)close(); }

auto PairingKnock::start(Door door) -> Expected<void, Error> {
  if (socket_) return unexpected(Error::invalid_state);
  if (door.destination.port == 0 ||
      door.timeout <= std::chrono::microseconds::zero())
    return unexpected(Error::invalid_argument);

  const auto knock = datagram(door.generation);
  if (!knock) return unexpected(Error::invalid_argument);

  std::vector<platform::SocketEndpoint> candidates{door.destination};
  candidates.insert(candidates.end(), door.fallbacks.begin(),
                    door.fallbacks.end());
  const auto reached = std::ranges::find_if(
      candidates, [this, &door](const platform::SocketEndpoint& candidate) {
        return open(candidate, door.broadcast).has_value();
      });
  if (reached == candidates.end()) return unexpected(Error::network_failure);
  door.destination = *reached;

  const auto sent = door.broadcast ? socket_->send_to(*knock, door.destination)
                                   : socket_->send(*knock);
  if (!sent) {
    (void)close();
    return unexpected(Error::network_failure);
  }
  if (sent.value() != knock->size()) {
    (void)close();
    return unexpected(Error::short_send);
  }

  deadline_ =
      timing::Deadline::after(clock_, timing::Deadline::Duration{door.timeout});
  door_ = std::move(door);
  return {};
}

auto PairingKnock::pump_once()
    -> Expected<std::optional<platform::SocketEndpoint>, Error> {
  if (!socket_) return unexpected(Error::invalid_state);
  const bool expired = deadline_.expired(clock_);

  std::array<std::byte, answer_buffer_size> buffer{};
  const auto received = socket_->receive_from(buffer);
  if (!received) {
    if (received.error() == platform::NetError::would_block && !expired)
      return std::optional<platform::SocketEndpoint>{};
    (void)close();
    return unexpected(received.error() == platform::NetError::would_block
                          ? Error::timeout
                          : Error::network_failure);
  }
  if (received.value().size == 0 || received.value().size > buffer.size()) {
    (void)close();
    return unexpected(Error::network_failure);
  }
  const auto source = received.value().source;
  if ((!door_.broadcast && source != door_.destination) ||
      !answers(door_.generation, std::span<const std::byte>{
                                     buffer.data(), received.value().size})) {
    if (expired) {
      (void)close();
      return unexpected(Error::timeout);
    }
    return std::optional<platform::SocketEndpoint>{};
  }

  const auto closed = close();
  if (!closed) return unexpected(closed.error());
  return std::optional<platform::SocketEndpoint>{source};
}

auto PairingKnock::cancel() -> Expected<void, Error> { return close(); }

auto PairingKnock::open(const platform::SocketEndpoint& destination,
                        bool broadcast) -> Expected<void, Error> {
  if (destination.port == 0) return unexpected(Error::invalid_argument);
  auto created = network_.sockets().create_udp_socket(destination.address);
  if (!created) return unexpected(Error::network_failure);
  socket_ = std::move(created.value());
  if (broadcast) {
    const auto any = destination.address.is_ipv4()
                         ? platform::IpAddress::any_ipv4()
                         : platform::IpAddress::any_ipv6();
    if ((destination.address.is_ipv4() &&
         !socket_->set_boolean_option(platform::SocketOption::broadcast,
                                      true)) ||
        !socket_->bind({any, 0})) {
      (void)close();
      return unexpected(Error::network_failure);
    }
    return {};
  }
  if (!socket_->connect(destination)) {
    (void)close();
    return unexpected(Error::network_failure);
  }
  return {};
}

auto PairingKnock::close() -> Expected<void, Error> {
  if (!socket_) return {};
  auto socket = std::move(socket_);
  const auto closed = socket->close();
  if (!closed && closed.error() != platform::NetError::closed)
    return unexpected(Error::network_failure);
  return {};
}

}  // namespace libchiaking::consoles
