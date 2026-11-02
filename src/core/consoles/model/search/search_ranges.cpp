#include "libchiaking/detail/consoles/network_search.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace libchiaking::consoles {

SearchRanges::SearchRanges(std::vector<Subnet> subnets, bool unicast,
                           std::size_t target_count)
    : subnets_(std::move(subnets)),
      networks_(subnets_.size()),
      unicast_(unicast),
      target_count_(target_count) {}

auto SearchRanges::create(std::vector<Subnet> subnets, bool unicast)
    -> Expected<SearchRanges, Invalid> {
  if (subnets.size() > subnet_limit)
    return unexpected(Invalid::too_many_subnets);
  std::vector<Subnet> distinct;
  std::set<std::array<std::uint8_t, 4>> targets;
  for (const auto& subnet : subnets) {
    if (std::ranges::find(distinct, subnet) != distinct.end()) continue;
    for (std::size_t index = 0; index < subnet.host_count(); ++index)
      targets.insert(subnet.host(index).ipv4_bytes());
    if (targets.size() > target_limit)
      return unexpected(Invalid::too_many_targets);
    distinct.push_back(subnet);
  }
  return SearchRanges{std::move(distinct), unicast, targets.size()};
}

auto SearchRanges::over(const Subnet& subnet, HomeNetwork network) const
    -> Expected<SearchRanges, Invalid> {
  const auto found = std::ranges::find(subnets_, subnet);
  if (found == subnets_.end()) return unexpected(Invalid::subnet);
  auto routed = *this;
  routed.networks_[static_cast<std::size_t>(found - subnets_.begin())] =
      std::move(network);
  return routed;
}

auto SearchRanges::network_of(const Subnet& subnet) const noexcept
    -> const HomeNetwork& {
  static const HomeNetwork platform_own;
  const auto found = std::ranges::find(subnets_, subnet);
  if (found == subnets_.end()) return platform_own;
  return networks_[static_cast<std::size_t>(found - subnets_.begin())];
}

auto SearchRanges::parse(std::span<const std::string> cidrs, bool unicast)
    -> Expected<SearchRanges, Invalid> {
  if (cidrs.size() > subnet_limit) return unexpected(Invalid::too_many_subnets);
  std::vector<Subnet> subnets;
  for (const auto& cidr : cidrs) {
    const auto subnet = Subnet::parse(cidr);
    if (!subnet) return unexpected(Invalid::subnet);
    subnets.push_back(*subnet);
  }
  return create(std::move(subnets), unicast);
}

}  // namespace libchiaking::consoles
