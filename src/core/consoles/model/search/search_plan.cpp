#include "src/core/consoles/model/search/search_plan.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace libchiaking::consoles {

SearchPlan::SearchPlan(SearchRanges ranges, bool broadcasts) noexcept
    : ranges_(std::move(ranges)), broadcasts_(broadcasts) {}

auto SearchPlan::eligible(const platform::InterfaceInfo& interface) noexcept
    -> bool {
  return interface.administrative_up.value_or(interface.operational) &&
         !interface.loopback && interface.address.is_ipv4() &&
         !interface.address.is_unspecified() &&
         !interface.address.is_loopback();
}

auto SearchPlan::broadcasts(const platform::InterfaceInfo& interface) noexcept
    -> bool {
  return interface.has_broadcast && interface.broadcast.is_ipv4() &&
         !interface.broadcast.is_unspecified();
}

auto SearchPlan::ordering(const platform::InterfaceInfo& interface) noexcept
    -> std::tuple<std::uint32_t, std::string_view, std::uint32_t, bool,
                  std::optional<std::uint8_t>, std::uint32_t> {
  return std::tuple{interface.address.ipv4_number(),
                    std::string_view{interface.name},
                    interface.index,
                    interface.has_broadcast,
                    interface.ipv4_prefix_length,
                    interface.has_broadcast && interface.broadcast.is_ipv4()
                        ? interface.broadcast.ipv4_number()
                        : 0U};
}

auto SearchPlan::same(const platform::InterfaceInfo& left,
                      const platform::InterfaceInfo& right) noexcept -> bool {
  return left.name == right.name && left.index == right.index &&
         left.address == right.address && left.broadcast == right.broadcast &&
         left.has_broadcast == right.has_broadcast &&
         left.loopback == right.loopback &&
         left.operational == right.operational &&
         left.administrative_up == right.administrative_up &&
         left.ipv4_prefix_length == right.ipv4_prefix_length &&
         left.lan_adapter == right.lan_adapter && left.medium == right.medium;
}

auto SearchPlan::range_label(const platform::InterfaceInfo& interface)
    -> std::string {
  if (!interface.ipv4_prefix_length ||
      *interface.ipv4_prefix_length > address_bits)
    return platform::to_string(interface.broadcast);
  const auto prefix = *interface.ipv4_prefix_length;
  const auto mask = prefix == 0 ? 0U
                                : std::numeric_limits<std::uint32_t>::max()
                                      << (address_bits - prefix);
  return platform::to_string(platform::IpAddress::from_ipv4_number(
             interface.address.ipv4_number() & mask)) +
         "/" + std::to_string(prefix);
}

auto SearchPlan::update_interfaces(
    std::span<const platform::InterfaceInfo> interfaces)
    -> Expected<bool, Refusal> {
  std::vector<platform::InterfaceInfo> usable;
  for (const auto& interface : interfaces) {
    if (!eligible(interface)) continue;
    if (usable.size() >= interface_limit)
      return unexpected(Refusal::too_many_interfaces);
    usable.push_back(interface);
  }
  std::ranges::sort(usable, {}, ordering);
  if (std::ranges::equal(interfaces_, usable, same)) return false;
  std::set<std::uint32_t> targets;
  std::vector<Subnet> active;
  if (ranges_.unicast()) {
    for (const auto& subnet : ranges_.subnets()) {
      const auto local = [&subnet](const platform::InterfaceInfo& interface) {
        return subnet.contains(interface.address);
      };
      if (!ranges_.network_of(subnet).network() &&
          std::ranges::any_of(usable, local))
        continue;
      for (std::size_t index = 0; index < subnet.host_count(); ++index)
        targets.insert(subnet.host(index).ipv4_number());
      active.push_back(subnet);
    }
  }
  interfaces_ = std::move(usable);
  targets_.assign(targets.begin(), targets.end());
  active_subnets_ = std::move(active);
  replies_.clear();
  cursor_ = 0;
  next_broadcast_us_ = 0;
  next_sweep_us_ = 0;
  return true;
}

void SearchPlan::prune(std::uint64_t now_us) {
  std::erase_if(replies_, [now_us](const Seen& entry) {
    return !alive(entry.seen_us, now_us);
  });
}

void SearchPlan::receive(SearchReply reply, std::uint64_t now_us) {
  prune(now_us);
  const auto existing = std::ranges::find(
      replies_, reply.hardware_id(),
      [](const Seen& entry) { return entry.reply.hardware_id(); });
  if (existing != replies_.end()) {
    existing->reply = std::move(reply);
    existing->seen_us = now_us;
    return;
  }
  if (replies_.size() < reply_limit)
    replies_.push_back({std::move(reply), now_us});
}

void SearchPlan::keep_probing(platform::IpAddress address) {
  if (address.is_ipv4() &&
      std::ranges::find(pinned_, address.ipv4_number()) == pinned_.end())
    pinned_.push_back(address.ipv4_number());
}

auto SearchPlan::destinations(std::uint64_t now_us)
    -> std::vector<platform::IpAddress> {
  prune(now_us);
  if (interfaces_.empty() && broadcasts_) return {};
  std::set<std::uint32_t> selected;
  if (now_us >= next_broadcast_us_) {
    for (const auto& interface : interfaces_)
      if (broadcasts_ && broadcasts(interface))
        selected.insert(interface.broadcast.ipv4_number());
    for (const auto& entry : replies_)
      if (entry.reply.address().is_ipv4())
        selected.insert(entry.reply.address().ipv4_number());
    selected.insert(pinned_.begin(), pinned_.end());
    next_broadcast_us_ =
        timing::Deadline::after_us(now_us, interval_us(broadcast_interval));
  }
  if (now_us >= next_sweep_us_ && !targets_.empty()) {
    const auto end = std::min(cursor_ + batch_size, targets_.size());
    selected.insert(targets_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                    targets_.begin() + static_cast<std::ptrdiff_t>(end));
    cursor_ = end == targets_.size() ? 0 : end;
    next_sweep_us_ = timing::Deadline::after_us(
        now_us, interval_us(cursor_ == 0 ? cycle_interval : batch_interval));
  }
  std::vector<platform::IpAddress> result;
  result.reserve(selected.size());
  std::ranges::transform(selected, std::back_inserter(result),
                         platform::IpAddress::from_ipv4_number);
  return result;
}

auto SearchPlan::replies(std::uint64_t now_us) const
    -> std::vector<SearchReply> {
  std::vector<SearchReply> result;
  for (const auto& entry : replies_)
    if (alive(entry.seen_us, now_us)) result.push_back(entry.reply);
  std::ranges::sort(result, {}, &SearchReply::hardware_id);
  return result;
}

auto SearchPlan::progress() const -> Progress {
  Progress result;
  result.targets = targets_.size();
  result.visited = cursor_;
  result.network_available = !interfaces_.empty();
  std::set<std::uint32_t> addresses;
  std::set<std::string> ranges;
  for (const auto& interface : interfaces_) {
    if (!broadcasts_ || !broadcasts(interface)) continue;
    addresses.insert(interface.broadcast.ipv4_number());
    ranges.insert(range_label(interface));
  }
  std::ranges::transform(addresses,
                         std::back_inserter(result.broadcast_addresses),
                         platform::IpAddress::from_ipv4_number);
  result.broadcast_ranges.assign(ranges.begin(), ranges.end());
  if (result.network_available) result.unicast_ranges = active_subnets_;
  return result;
}

auto SearchPlan::next_deadline_us(std::uint64_t now_us) const
    -> std::optional<std::uint64_t> {
  if (interfaces_.empty() && broadcasts_) return std::nullopt;
  auto next = std::max(now_us, next_broadcast_us_);
  if (!targets_.empty())
    next = std::min(next, std::max(now_us, next_sweep_us_));
  for (const auto& entry : replies_)
    if (alive(entry.seen_us, now_us))
      next = std::min(next, timing::Deadline::after_us(
                                entry.seen_us, interval_us(reply_lifetime)));
  return next;
}

}  // namespace libchiaking::consoles
