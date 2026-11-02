#include "src/core/net/ntp/time_sync.hpp"

#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>

namespace libchiaking::net::ntp {

TimeSync::TimeSync(platform::NetworkProvider& network,
                   const platform::ClockProvider& monotonic,
                   platform::SecureRandomProvider& random) noexcept
    : network_(network), monotonic_(monotonic), random_(random) {}

auto TimeSync::sync(const Stop& stop, const ServerObserver& asking)
    -> std::optional<TrustedTime> {
  for (const auto& provider : providers) {
    if (stop.stop_requested()) return std::nullopt;
    if (auto synced = from(provider, stop, asking)) return synced;
  }
  return std::nullopt;
}

auto TimeSync::agreed(TimeSource source, std::span<const Sample> samples)
    -> std::optional<TrustedTime> {
  std::vector<Sample> distinct;
  for (const auto& sample : samples)
    if (std::ranges::none_of(distinct, [&](const Sample& kept) {
          return kept.server == sample.server;
        }))
      distinct.push_back(sample);
  if (distinct.size() < 2U) return std::nullopt;
  const auto reference_us =
      std::ranges::max(distinct, {}, &Sample::at_us).at_us;
  std::vector<Timestamp::Time> projected;
  projected.reserve(distinct.size());
  for (const auto& sample : distinct)
    projected.push_back(sample.time +
                        std::chrono::microseconds{static_cast<std::int64_t>(
                            reference_us - sample.at_us)});
  std::ranges::sort(projected);
  std::size_t best_first = 0;
  std::size_t best_size = 0;
  for (std::size_t first = 0, last = 0; first < projected.size(); ++first) {
    last = std::max(last, first);
    while (last + 1U < projected.size() &&
           projected[last + 1U] - projected[first] <= agreement)
      ++last;
    if (last - first + 1U > best_size) {
      best_first = first;
      best_size = last - first + 1U;
    }
  }
  if (best_size < 2U || best_size * 2U <= projected.size()) return std::nullopt;
  const auto low = projected[best_first + (best_size - 1U) / 2U];
  const auto high = projected[best_first + best_size / 2U];
  const auto median = low + (high - low) / 2;
  return TrustedTime{source, median, reference_us};
}

auto TimeSync::from(const Provider& provider, const Stop& stop,
                    const ServerObserver& asking)
    -> std::optional<TrustedTime> {
  std::string_view told = provider.hosts.front();
  if (asking) asking(told);
  std::vector<Sample> samples;
  for (const auto& server : servers_of(provider.hosts, stop)) {
    if (stop.stop_requested()) return std::nullopt;
    if (asking && server.host != told) {
      told = server.host;
      asking(told);
    }
    if (auto answered = sample(server.address, stop))
      samples.push_back(*answered);
  }
  return agreed(provider.source, samples);
}

auto TimeSync::servers_of(std::span<const std::string_view> hosts,
                          const Stop& stop) -> std::vector<Server> {
  std::vector<Server> servers;
  for (const auto host : hosts) {
    const auto resolved = network_.dns().resolve(host, service, stop);
    if (!resolved) continue;
    for (const auto& address : resolved.value())
      if (!address.is_unspecified() &&
          std::ranges::find(servers, address, &Server::address) ==
              servers.end())
        servers.push_back({host, address});
  }
  std::ranges::stable_partition(servers, [](const Server& server) {
    return server.address.family() == platform::IpFamily::ipv4;
  });
  if (servers.size() > max_addresses)
    servers.erase(servers.begin() + static_cast<std::ptrdiff_t>(max_addresses),
                  servers.end());
  return servers;
}

auto TimeSync::sample(const platform::IpAddress& server, const Stop& stop)
    -> std::optional<Sample> {
  const auto transmit = nonce();
  if (!transmit) return std::nullopt;
  auto socket = network_.sockets().create_udp_socket(server);
  if (!socket) return std::nullopt;
  const auto wildcard = server.family() == platform::IpFamily::ipv4
                            ? platform::IpAddress::any_ipv4()
                            : platform::IpAddress::any_ipv6();
  if (!socket.value()->bind({wildcard, 0})) return std::nullopt;
  auto poller = network_.pollers().create_poller();
  if (!poller) return std::nullopt;
  Query query{*socket.value(), *poller.value(), monotonic_};
  auto answered = query.ask({server, port}, *transmit, stop, query_timeout);
  if (!answered) return std::nullopt;
  return std::move(answered).value();
}

auto TimeSync::nonce() -> std::optional<Timestamp> {
  std::array<std::byte, sizeof(std::uint64_t)> drawn{};
  if (!random_.fill_bytes(drawn)) return std::nullopt;
  const auto wire = bytes::Reader{drawn}.u64_be();
  if (!wire || wire.value() == 0U) return std::nullopt;
  return Timestamp{wire.value()};
}

}  // namespace libchiaking::net::ntp
