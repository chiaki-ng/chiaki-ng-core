#include "src/core/play/adapters/nat/stun_probe.hpp"

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/nat/stun/binding_client.hpp"
#include "src/core/net/nat/stun/public_servers.hpp"

#include <algorithm>
#include <span>
#include <string>

namespace libchiaking::play {

StunProbe::StunProbe(platform::NetworkProvider& network,
                     const platform::ClockProvider& clock,
                     platform::SecureRandomProvider& random) noexcept
    : network_(network), clock_(clock), random_(random) {}

auto StunProbe::probe(const platform::StopToken& stop)
    -> Expected<NatMappings, NatTypeFailure> {
  using Reason = NatTypeFailure::Reason;
  auto socket =
      network_.sockets().create_udp_socket(platform::IpAddress::any_ipv4());
  if (!socket || !socket.value() ||
      !socket.value()->bind({platform::IpAddress::any_ipv4(), 0}))
    return unexpected(NatTypeFailure{Reason::network});
  auto poller = network_.pollers().create_poller();
  if (!poller || !poller.value())
    return unexpected(NatTypeFailure{Reason::network});

  const auto& party_voice = net::nat::stun::PublicServers::psn_party_voice;
  auto first = ask(*socket.value(), *poller.value(), party_voice.host,
                   party_voice.port, std::nullopt, stop);
  if (!first) return unexpected(first.error());
  if (!first.value()) return unexpected(NatTypeFailure{Reason::unanswered});
  const auto unanswered = [](const auto& asked) {
    return asked ? !asked.value() : asked.error().reason == Reason::unresolved;
  };
  Expected<Answer, NatTypeFailure> second = Answer{};
  const auto& hosts = net::nat::stun::PublicServers::fixed_hosts;
  for (const auto& host :
       std::span{hosts}.first(std::min(most_second_servers, hosts.size()))) {
    second = ask(*socket.value(), *poller.value(), host.host, host.port,
                 first.value()->second, stop);
    if (!unanswered(second)) break;
  }
  if (!second && second.error().reason != Reason::unresolved)
    return unexpected(second.error());

  NatMappings mappings{.first = first.value()->first};
  if (second && second.value()) mappings.second = second.value()->first;
  if (auto interfaces = network_.interfaces().enumerate())
    for (const auto& entry : interfaces.value())
      if (entry.address.is_ipv4()) mappings.local.push_back(entry.address);
  return mappings;
}

auto StunProbe::ask(platform::UdpSocket& socket, platform::Poller& poller,
                    std::string_view host, std::uint16_t port,
                    const std::optional<platform::IpAddress>& skip,
                    const platform::StopToken& stop)
    -> Expected<Answer, NatTypeFailure> {
  using Reason = NatTypeFailure::Reason;
  if (stop.stop_requested())
    return unexpected(NatTypeFailure{Reason::cancelled});
  auto addresses = network_.dns().resolve(host, std::to_string(port), stop);
  if (!addresses) {
    return unexpected(NatTypeFailure{
        stop.stop_requested() ? Reason::cancelled : Reason::unresolved});
  }
  net::nat::stun::BindingClient client{socket, poller, clock_};
  for (const auto& address : addresses.value()) {
    if (!address.is_ipv4() || (skip && *skip == address)) continue;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      net::nat::stun::TransactionId transaction{};
      if (!random_.fill_bytes(transaction))
        return unexpected(NatTypeFailure{Reason::network});
      auto mapped =
          client.query({address, port}, transaction, stop, reply_timeout);
      if (mapped) return Answer{std::pair{mapped.value(), address}};
      if (mapped.error() == net::nat::stun::BindingClient::Error::cancelled)
        return unexpected(NatTypeFailure{Reason::cancelled});
    }
  }
  return Answer{};
}

}  // namespace libchiaking::play
