#include "libchiaking/detail/play/datacenter.hpp"

#include <algorithm>

namespace libchiaking::play {

auto DatacenterOffer::from(std::span<const Probe> probes,
                           std::chrono::milliseconds wait) -> DatacenterOffer {
  DatacenterOffer offer;
  offer.wait_ = std::max(wait, minimum_wait);
  for (const auto& probe : probes) {
    auto name = DatacenterName::parse(probe.name);
    if (!name || offer.find(*name) != nullptr) continue;
    const bool answered = probe.answered && probe.round_trip.count() > 0;
    offer.entries_.push_back(
        Entry{std::move(*name),
              answered ? std::optional{probe.round_trip} : std::nullopt});
  }
  std::ranges::stable_sort(
      offer.entries_, [](const Entry& left, const Entry& right) {
        if (left.answered() != right.answered()) return left.answered();
        return left.answered() && *left.round_trip() < *right.round_trip();
      });
  return offer;
}

auto DatacenterOffer::lowest() const noexcept -> const Entry* {
  const auto found = std::ranges::find_if(entries_, &Entry::answered);
  return found == entries_.end() ? nullptr : &*found;
}

auto DatacenterOffer::find(const DatacenterName& name) const noexcept
    -> const Entry* {
  const auto found = std::ranges::find(entries_, name, &Entry::name);
  return found == entries_.end() ? nullptr : &*found;
}

auto DatacenterOffer::is_lowest(const Entry& entry) const noexcept -> bool {
  const auto* first = lowest();
  return first != nullptr && first->name() == entry.name();
}

}  // namespace libchiaking::play
