#include "src/core/catalog/model/library/owned_ps5_games.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

OwnedPs5Games::OwnedPs5Games(std::vector<Ps5Entitlement> records,
                             LibraryState state, Scan scan,
                             Time fetched_at) noexcept
    : records_(std::move(records)),
      state_(state),
      scan_(scan),
      fetched_at_(fetched_at) {}

auto OwnedPs5Games::unread(Time at) -> OwnedPs5Games {
  return OwnedPs5Games{{}, LibraryState::unavailable, {}, at};
}

auto OwnedPs5Games::games() const -> std::vector<OwnedTitle> {
  return Ps5Entitlement::titles_of(records_);
}

auto OwnedPs5Games::find(const OwnershipId& id) const noexcept
    -> const Ps5Entitlement* {
  const auto found = std::ranges::find(records_, id, &Ps5Entitlement::id);
  return found != records_.end() ? &*found : nullptr;
}

auto OwnedPs5Games::owned_and_active() const -> std::set<OwnershipId> {
  std::set<OwnershipId> held;
  for (const auto& record : records_)
    if (record.facts().active == true) held.insert(record.id());
  return held;
}

auto OwnedPs5Games::with(Ps5Entitlement record, Time checked_at) const
    -> OwnedPs5Games {
  auto changed = *this;
  const auto held =
      std::ranges::find(changed.records_, record.id(), &Ps5Entitlement::id);
  if (held != changed.records_.end())
    *held = std::move(record);
  else
    changed.records_.push_back(std::move(record));
  changed.fetched_at_ = checked_at;
  return changed;
}

}  // namespace libchiaking::catalog
