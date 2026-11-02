#include "src/core/net/nat/stun/mapping_estimate.hpp"

#include "src/core/utils/bytes/endian.hpp"

#include <utility>

namespace libchiaking::net::nat::stun {

auto AllocationDiscovery::estimate(
    std::span<const platform::SocketEndpoint> observations)
    -> Expected<StunAllocationEstimate, StunAllocationError> {
  if (observations.empty() || observations.size() > observation_count)
    return unexpected(StunAllocationError::invalid_input);
  for (const auto& item : observations) {
    if (!item.address.is_ipv4() || item.address.is_unspecified() ||
        item.port == 0)
      return unexpected(StunAllocationError::invalid_input);
  }
  if (observations.size() == 1U)
    return StunAllocationEstimate{observations[0], 0, false};
  if (observations.size() == 3U) {
    const auto& a = observations[0];
    const auto& b = observations[1];
    const auto& c = observations[2];
    const auto p1 = static_cast<std::int32_t>(a.port);
    const auto p2 = static_cast<std::int32_t>(b.port);
    const auto p3 = static_cast<std::int32_t>(c.port);
    std::int32_t increment{};
    bool random_allocation{};
    if (a.address != b.address) {
      if (a.address == c.address)
        increment = (p3 - p1) / 2;
      else if (b.address == c.address)
        increment = p3 - p2;
    } else if (a.address != c.address) {
      increment = p2 - p1;
    } else {
      increment = p2 - p1;
      const auto increment1 = p3 - p2;
      if (increment1 != increment) {
        random_allocation = true;
        if (increment == 0) increment = increment1;
      }
    }
    return StunAllocationEstimate{
        {c.address, static_cast<std::uint16_t>(p3 + increment)},
        increment,
        random_allocation};
  }
  if (observations.size() == 4U) {
    const auto& a = observations[0];
    const auto& b = observations[1];
    const auto& c = observations[2];
    const auto& d = observations[3];
    if (a.address != b.address) return estimate_four_divergent(observations);
    const auto p1 = static_cast<std::int32_t>(a.port);
    const auto p2 = static_cast<std::int32_t>(b.port);
    const auto p3 = static_cast<std::int32_t>(c.port);
    const auto p4 = static_cast<std::int32_t>(d.port);
    std::int32_t increment{};
    bool random_allocation{};
    if (b.address != c.address) {
      increment = p2 - p1;
      if (b.address == d.address) {
        const auto increment1 = (p4 - p2) / 2;
        if (increment1 != increment) {
          random_allocation = true;
          if (increment == 0) increment = increment1;
        }
      }
    } else if (c.address != d.address) {
      increment = p2 - p1;
      const auto increment1 = p3 - p2;
      if (increment1 != increment) {
        random_allocation = true;
        if (increment == 0) increment = increment1;
      }
    } else {
      const auto inc0 = static_cast<std::uint16_t>(p2 - p1);
      const auto inc1 = static_cast<std::uint16_t>(p3 - p2);
      const auto inc2 = static_cast<std::uint16_t>(p4 - p3);
      if (inc0 == inc1 || inc0 == inc2)
        increment = inc0;
      else if (inc1 == inc2)
        increment = inc1;
      else {
        random_allocation = true;
        increment = inc0 != 0U ? inc0 : inc1;
      }
    }
    return StunAllocationEstimate{d, increment, random_allocation};
  }
  const auto& first = observations[0];
  const auto& second = observations[1];
  if (first.address != second.address)
    return StunAllocationEstimate{second, 0, false};
  const auto increment = static_cast<std::int32_t>(second.port) -
                         static_cast<std::int32_t>(first.port);
  const auto projected_port = static_cast<std::uint16_t>(
      static_cast<std::int32_t>(second.port) + 2 * increment);
  return StunAllocationEstimate{
      {second.address, projected_port}, increment, false};
}

auto AllocationDiscovery::estimate_four_divergent(
    std::span<const platform::SocketEndpoint> observations)
    -> Expected<StunAllocationEstimate, StunAllocationError> {
  if (observations.size() != 4U)
    return unexpected(StunAllocationError::invalid_input);
  for (const auto& item : observations) {
    if (!item.address.is_ipv4() || item.address.is_unspecified() ||
        item.port == 0)
      return unexpected(StunAllocationError::invalid_input);
  }
  const auto& a = observations[0];
  const auto& b = observations[1];
  const auto& c = observations[2];
  const auto& d = observations[3];
  if (a.address == b.address)
    return unexpected(StunAllocationError::invalid_input);
  const auto p1 = static_cast<std::int32_t>(a.port);
  const auto p2 = static_cast<std::int32_t>(b.port);
  const auto p3 = static_cast<std::int32_t>(c.port);
  const auto p4 = static_cast<std::int32_t>(d.port);
  std::int32_t increment{};
  bool random_allocation{};
  if (a.address == c.address || a.address == d.address) {
    if (a.address == d.address) {
      if (a.address == c.address) {
        increment = p4 - p3;
        const auto increment1 = (p3 - p1) / 2;
        if (increment1 != increment) {
          random_allocation = true;
          if (increment == 0) increment = increment1;
        }
      } else {
        increment = (p4 - p1) / 4;
      }
    } else {
      increment = (p3 - p1) / 2;
    }
  } else if (b.address == c.address || b.address == d.address) {
    if (b.address == d.address) {
      if (b.address == c.address) {
        increment = p4 - p3;
        const auto increment1 = p3 - p2;
        if (increment1 != increment) {
          random_allocation = true;
          if (increment == 0) increment = increment1;
        }
      } else {
        increment = (p4 - p1) / 4;
      }
    } else {
      increment = (p3 - p1) / 2;
    }
  } else if (c.address == d.address) {
    increment = p3 - p4;
  }
  return StunAllocationEstimate{d, increment, random_allocation};
}

auto AllocationDiscovery::discover(
    platform::UdpSocket& socket, platform::Poller& poller,
    const platform::ClockProvider& clock,
    platform::SecureRandomProvider& random, platform::HttpProvider& http,
    platform::DnsProvider& dns, const platform::StopToken& stop,
    std::chrono::nanoseconds per_server_timeout,
    const diagnostics::Trace* trace, StunDiscoveryState* cached,
    std::chrono::nanoseconds budget)
    -> Expected<StunAllocationEstimate, StunAllocationDiscoveryError> {
  if (stop.stop_requested())
    return unexpected(StunAllocationDiscoveryError::cancelled);
  StunDiscoveryState transient;
  auto& state = cached ? *cached : transient;
  const bool reuse = state.allocation.has_value();
  if (!reuse) {
    bool ipv6_follows{};
    auto hosts =
        diagnostics::observe(trace, diagnostics::Operation::stun_hosts, [&] {
          return PublicServers::fetch_host_list(http, stop, &ipv6_follows);
        });
    if (hosts)
      state.preferred = std::move(hosts.value());
    else if (hosts.error() == StunHostListFetchError::cancelled)
      return unexpected(StunAllocationDiscoveryError::cancelled);
    else if (trace)
      trace->decision(diagnostics::Trace::Decision::builtin_stun_servers,
                      static_cast<unsigned>(hosts.error()));
    if (ipv6_follows) {
      auto ipv6_hosts = diagnostics::observe(
          trace, diagnostics::Operation::stun_hosts_ipv6,
          [&] { return PublicServers::fetch_ipv6_host_list(http, stop); });
      if (ipv6_hosts)
        state.preferred_ipv6 = std::move(ipv6_hosts.value());
      else if (ipv6_hosts.error() == StunHostListFetchError::cancelled)
        return unexpected(StunAllocationDiscoveryError::cancelled);
    }
  }
  auto order = PublicServers::build_host_order(state.preferred);
  const auto fallback_start = order.size() - PublicServers::fixed_hosts.size();
  std::array<platform::SocketEndpoint, observation_count> observations{};
  std::size_t count{};
  bool resolved_any = false;
  const auto asked_from = clock.now_microseconds();
  const auto budget_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(budget).count());
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (stop.stop_requested())
      return unexpected(StunAllocationDiscoveryError::cancelled);
    if (clock.now_microseconds() - asked_from >= budget_us) break;
    if (i == fallback_start) {
      for (std::size_t j = PublicServers::fixed_hosts.size() - 1; j > 1; --j) {
        std::array<std::byte, sizeof(std::uint32_t)> drawn{};
        if (!random.fill_bytes(drawn))
          return unexpected(StunAllocationDiscoveryError::random_failure);
        const auto selected =
            1U + bytes::load_be<std::uint32_t>(drawn) % (j - 1U);
        std::swap(order[fallback_start + j], order[fallback_start + selected]);
      }
    }
    const StunServerHost host{order[i].host, order[i].port};
    auto resolved =
        diagnostics::observe(trace, diagnostics::Operation::stun_resolve, [&] {
          return PublicServers::resolve_server_endpoints(
              std::span{&host, 1}, platform::IpFamily::ipv4, dns, stop);
        });
    if (stop.stop_requested())
      return unexpected(StunAllocationDiscoveryError::cancelled);
    if (!resolved) continue;
    resolved_any = true;
    net::nat::stun::TransactionId transaction{};
    if (!random.fill_bytes(transaction))
      return unexpected(StunAllocationDiscoveryError::random_failure);
    auto mapped =
        diagnostics::observe(trace, diagnostics::Operation::stun_binding, [&] {
          return net::nat::stun::BindingClient{socket, poller, clock}.query(
              resolved.value().front().endpoint, transaction, stop,
              per_server_timeout);
        });
    if (stop.stop_requested())
      return unexpected(StunAllocationDiscoveryError::cancelled);
    if (!mapped) continue;
    if (reuse) {
      auto result = *state.allocation;
      result.projected = mapped.value();
      return result;
    }
    if (mapped.value().port == 0) continue;
    observations[count++] = mapped.value();
    if (count == observations.size()) break;
  }
  if (!count)
    return unexpected(resolved_any ? StunAllocationDiscoveryError::exhausted
                                   : StunAllocationDiscoveryError::no_servers);
  auto estimate =
      AllocationDiscovery::estimate(std::span{observations.data(), count});
  if (!estimate) return unexpected(StunAllocationDiscoveryError::exhausted);
  state.allocation = estimate.value();
  return estimate.value();
}

}  // namespace libchiaking::net::nat::stun
