#include "src/core/consoles/adapters/search/beacon.hpp"

#include "libchiaking/detail/secret_array.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace libchiaking::consoles {

Beacon::Beacon(platform::NetworkProvider& network,
               platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock, Watcher& watcher)
    : Beacon(network, threading, clock, watcher, Settings{}) {}

Beacon::Beacon(platform::NetworkProvider& network,
               platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock, Watcher& watcher,
               Settings settings)
    : network_(network),
      watcher_(watcher),
      settings_(std::move(settings)),
      broadcasts_(network.capabilities().broadcast),
      waiter_(threading, clock, network) {}

Beacon::~Beacon() {
  if (socket_) (void)socket_->close();
}

auto Beacon::accepts(const Settings& settings) noexcept -> bool {
  if (settings.targets.size() > target_limit ||
      settings.target_hosts.size() > target_limit - settings.targets.size())
    return false;
  return std::ranges::all_of(
      settings.target_hosts, [](const std::string& host) {
        return !host.empty() && host.size() <= host_name_limit &&
               host.find('\0') == std::string::npos;
      });
}

auto Beacon::search_datagram(std::string_view protocol_version)
    -> std::vector<std::byte> {
  constexpr std::string_view request =
      "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:";
  std::vector<std::byte> datagram;
  datagram.reserve(request.size() + protocol_version.size() + 2);
  const auto append = [&datagram](std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    datagram.insert(datagram.end(), bytes.begin(), bytes.end());
  };
  append(request);
  append(protocol_version);
  append("\n");
  datagram.push_back(std::byte{0});
  return datagram;
}

auto Beacon::wake_datagram(std::string_view protocol_version,
                           const WakeCredential& credential) -> SecretBytes {
  constexpr std::size_t longest_credential = 20;
  SecretArray<longest_credential, char> digits;
  const auto written = std::to_chars(
      digits.data(), digits.data() + digits.size(), credential.value());
  SecretBytes datagram;
  datagram.append(
      "WAKEUP * HTTP/1.1\n"
      "client-type:vr\n"
      "auth-type:R\n"
      "model:w\n"
      "app-type:r\n"
      "user-credential:");
  datagram.append(std::string_view{
      digits.data(), static_cast<std::size_t>(written.ptr - digits.data())});
  datagram.append("\ndevice-discovery-protocol-version:");
  datagram.append(protocol_version);
  datagram.append("\n");
  datagram.push_back(std::byte{0});
  return datagram;
}

auto Beacon::wake_destination(platform::DnsProvider& dns, std::string_view host)
    -> Expected<platform::IpAddress, Failure> {
  if (host.empty() || host.size() > host_name_limit ||
      host.find('\0') != std::string_view::npos)
    return unexpected(Failure::invalid_argument);
  const bool ipv6 = host.find(':') != std::string_view::npos;
  const auto resolved = dns.resolve(host, {});
  if (!resolved) return unexpected(Failure::network);
  for (const auto& address : resolved.value()) {
    if (address.is_ipv6() == ipv6) return address;
  }
  return unexpected(Failure::network);
}

auto Beacon::wake_host(platform::NetworkProvider& network,
                       platform::ThreadingProvider& threading,
                       const platform::ClockProvider& clock,
                       std::string_view host, std::uint16_t port,
                       Generation generation, const WakeCredential& credential)
    -> Expected<void, Failure> {
  const auto destination = wake_destination(network.dns(), host);
  if (!destination) return unexpected(destination.error());
  Settings settings;
  settings.bind_address = destination.value().is_ipv4()
                              ? platform::IpAddress::any_ipv4()
                              : platform::IpAddress::any_ipv6();
  if (port != 0) {
    settings.ps4_destination_port = port;
    settings.ps5_destination_port = port;
  }
  Unwatched unwatched;
  Beacon beacon{network, threading, clock, unwatched, std::move(settings)};
  const auto started = beacon.start();
  if (!started) return started;
  const auto sent =
      beacon.send_wakeup(destination.value(), generation, credential);
  (void)beacon.stop();
  return sent;
}

auto Beacon::start() -> Expected<void, Failure> {
  if (socket_ || settings_.first_bind_port > settings_.last_bind_port)
    return unexpected(Failure::invalid_state);
  if (!accepts(settings_)) return unexpected(Failure::invalid_argument);

  auto candidate = network_.sockets().create_udp_socket(settings_.bind_address);
  if (!candidate) return unexpected(Failure::network);
  std::uint32_t port = settings_.first_bind_port;
  for (;;) {
    const auto bound = candidate.value()->bind(
        {settings_.bind_address, static_cast<std::uint16_t>(port)});
    if (bound) {
      socket_ = std::move(candidate.value());
      break;
    }
    if (port == 0) {
      (void)candidate.value()->close();
      return unexpected(Failure::network);
    }
    port = port == settings_.last_bind_port ? 0 : port + 1;
  }
  if (broadcasts_)
    (void)socket_->set_boolean_option(platform::SocketOption::broadcast, true);
  receive_buffer_.resize(SearchDatagram::receive_limit);
  pending_hosts_ = settings_.target_hosts;
  resolved_hosts_.clear();
  ps4_search_ = search_datagram(ps4_protocol_version);
  ps5_search_ = search_datagram(ps5_protocol_version);
  return {};
}

auto Beacon::search_targets()
    -> Expected<std::vector<platform::IpAddress>, Failure> {
  auto targets = settings_.targets;
  if (!targets.empty() || !settings_.target_hosts.empty() || !broadcasts_)
    return targets;
  if (!settings_.bind_address.is_ipv4()) {
    targets.push_back(ipv6_all_nodes);
    return targets;
  }
  targets.push_back(ipv4_limited_broadcast);
  if (!settings_.include_interface_broadcasts) return targets;
  const auto available = network_.interfaces().enumerate();
  if (!available) return unexpected(Failure::network);
  for (const auto& interface : available.value()) {
    if (interface.has_broadcast && !interface.loopback &&
        interface.address.is_ipv4() && interface.broadcast.is_ipv4() &&
        interface.broadcast != platform::IpAddress::any_ipv4())
      targets.push_back(interface.broadcast);
  }
  return targets;
}

auto Beacon::resolve_pending_hosts() -> Expected<void, Failure> {
  while (!pending_hosts_.empty()) {
    const auto addresses = network_.dns().resolve(pending_hosts_.front(), {});
    if (!addresses) return unexpected(Failure::network);
    std::optional<platform::IpAddress> chosen;
    for (const auto& address : addresses.value())
      if (address.is_ipv4() == settings_.bind_address.is_ipv4())
        chosen = address;
    if (!chosen) return unexpected(Failure::network);
    resolved_hosts_.push_back(*chosen);
    pending_hosts_.erase(pending_hosts_.begin());
  }
  return {};
}

auto Beacon::send_search() -> Expected<void, Failure> {
  if (!socket_) return unexpected(Failure::invalid_state);
  const auto resolved = resolve_pending_hosts();
  if (!resolved) return resolved;
  auto selected = search_targets();
  if (!selected) return unexpected(selected.error());
  selected.value().insert(selected.value().end(), resolved_hosts_.begin(),
                          resolved_hosts_.end());
  std::optional<ProbeFailure> last_failure;
  bool any_sent = false;
  for (const auto& target : selected.value()) {
    for (const auto& [datagram, port] :
         {std::pair{&ps4_search_, settings_.ps4_destination_port},
          std::pair{&ps5_search_, settings_.ps5_destination_port}}) {
      const auto failure = probe(*datagram, {target, port});
      if (failure)
        last_failure = failure;
      else
        any_sent = true;
    }
  }
  if (!any_sent && last_failure) return unexpected(reported(*last_failure));
  return {};
}

auto Beacon::send_search_to(platform::IpAddress destination)
    -> Expected<void, ProbeFailure> {
  if (!socket_) return unexpected(ProbeFailure::not_started);
  if (destination.is_ipv4() != settings_.bind_address.is_ipv4())
    return unexpected(ProbeFailure::address_family_mismatch);
  const auto ps4 =
      probe(ps4_search_, {destination, settings_.ps4_destination_port});
  if (ps4 == ProbeFailure::permission_denied) return unexpected(*ps4);
  const auto ps5 =
      probe(ps5_search_, {destination, settings_.ps5_destination_port});
  if (ps5 == ProbeFailure::permission_denied || (ps4 && ps5))
    return unexpected(*ps5);
  return {};
}

auto Beacon::send_wakeup(platform::IpAddress destination, Generation generation,
                         const WakeCredential& credential)
    -> Expected<void, Failure> {
  if (!socket_) return unexpected(Failure::invalid_state);
  const bool ps5 = generation == Generation::ps5;
  const auto datagram = wake_datagram(
      ps5 ? ps5_protocol_version : ps4_protocol_version, credential);
  if (const auto failure =
          transmit(datagram.bytes(),
                   {destination, ps5 ? settings_.ps5_destination_port
                                     : settings_.ps4_destination_port}))
    return unexpected(reported(*failure));
  return {};
}

auto Beacon::send_wakeup(std::string_view host, Generation generation,
                         const WakeCredential& credential)
    -> Expected<void, Failure> {
  if (!socket_) return unexpected(Failure::invalid_state);
  const auto destination = wake_destination(network_.dns(), host);
  if (!destination) return unexpected(destination.error());
  return send_wakeup(destination.value(), generation, credential);
}

auto Beacon::receive_once() -> Expected<bool, Failure> {
  if (!socket_) return unexpected(Failure::invalid_state);
  const auto received = socket_->receive_from(receive_buffer_);
  if (!received) {
    if (received.error() == platform::NetError::would_block) return false;
    return unexpected(Failure::network);
  }
  if (received.value().size > receive_buffer_.size())
    return unexpected(Failure::network);
  auto reply = SearchDatagram::admit(
      std::span<const std::byte>{receive_buffer_.data(), received.value().size},
      received.value().source.address);
  watcher_.on_datagram(reply.has_value());
  if (reply) watcher_.on_reply(std::move(reply.value()));
  return true;
}

auto Beacon::pump_once() -> Expected<void, Failure> {
  const auto received = receive_once();
  if (!received) return unexpected(received.error());
  return {};
}

auto Beacon::stop() -> Expected<void, Failure> {
  if (!socket_) return {};
  const auto closed = socket_->close();
  socket_.reset();
  if (!closed && closed.error() != platform::NetError::closed)
    return unexpected(Failure::network);
  return {};
}

auto Beacon::local_endpoint() const
    -> Expected<platform::SocketEndpoint, Failure> {
  if (!socket_) return unexpected(Failure::invalid_state);
  const auto endpoint = socket_->local_endpoint();
  if (!endpoint) return unexpected(Failure::network);
  return endpoint.value();
}

void Beacon::wait(std::chrono::milliseconds timeout) {
  waiter_.wait(socket_.get(), platform::PollEvent::readable, timeout);
}

void Beacon::wake() noexcept { waiter_.wake(); }

auto Beacon::transmit(std::span<const std::byte> datagram,
                      const platform::SocketEndpoint& destination)
    -> std::optional<ProbeFailure> {
  const auto sent = socket_->send_to(datagram, destination);
  if (!sent && destination.address.is_ipv6()) return std::nullopt;
  if (!sent && sent.error() == platform::NetError::permission_denied)
    return ProbeFailure::permission_denied;
  if (!sent) return ProbeFailure::network;
  if (sent.value() != datagram.size()) return ProbeFailure::short_send;
  return std::nullopt;
}

auto Beacon::probe(std::span<const std::byte> datagram,
                   const platform::SocketEndpoint& destination)
    -> std::optional<ProbeFailure> {
  const auto failure = transmit(datagram, destination);
  watcher_.on_probe(destination, failure);
  return failure;
}

}  // namespace libchiaking::consoles
