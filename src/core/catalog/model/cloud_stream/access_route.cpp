#include "src/core/catalog/model/cloud_stream/access_route.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

AccessRoute::AccessRoute(Kind kind, Kind without_ownership,
                         std::vector<OwnershipId> rechecked_by) noexcept
    : kind_(kind),
      without_ownership_(without_ownership),
      rechecked_by_(std::move(rechecked_by)) {}

auto AccessRoute::of(const MyGame& game, const ProductId& asked)
    -> AccessRoute {
  const auto unknown = game.needs_ownership_check();
  if (const auto child = game.child_known_as(asked)) {
    const auto kind = child->owned ? Kind::owned
                      : unknown    ? Kind::unknown
                                   : Kind::premium;
    AccessRoute route{kind, Kind::premium, {child->entitlement}};
    if (child->owned) route.owned_ = child->entitlement;
    route.offer_ = game.library_offer();
    return route;
  }
  if (game.is_bundle())
    return AccessRoute{Kind::choose_a_game, Kind::choose_a_game, {}};
  const auto without_ownership =
      game.plays_through_subscription() ? Kind::premium : Kind::unowned;
  const auto kind = game.is_owned() ? Kind::owned
                    : unknown       ? Kind::unknown
                                    : without_ownership;
  AccessRoute route{kind, without_ownership, ids_of(game)};
  if (game.is_owned()) route.owned_ = game.ownership()->id();
  route.offer_ = game.library_offer();
  return route;
}

auto AccessRoute::ids_of(const MyGame& game) -> std::vector<OwnershipId> {
  std::vector<OwnershipId> ids;
  const auto ask = [&ids](std::optional<OwnershipId> id) {
    if (id && std::ranges::find(ids, *id) == ids.end())
      ids.push_back(std::move(*id));
  };
  if (game.ownership()) ask(game.ownership()->id());
  if (const auto& offer = game.library_offer())
    for (const auto& id : offer->entitlement_ids) ask(OwnershipId::parse(id));
  return ids;
}

auto AccessRoute::refusal() const -> std::optional<LaunchRefusal> {
  return refusal_as(kind_);
}

auto AccessRoute::refusal_when_not_owned() const -> LaunchRefusal {
  return *refusal_as(without_ownership_);
}

auto AccessRoute::refusal_as(Kind kind) const -> std::optional<LaunchRefusal> {
  switch (kind) {
    case Kind::owned:
      break;
    case Kind::premium:
      return LaunchRefusal::add_to_library(offer_);
    case Kind::unowned:
      return LaunchRefusal::buy_to_stream();
    case Kind::unknown:
      return LaunchRefusal::ownership_unknown();
    case Kind::choose_a_game:
      return LaunchRefusal::choose_a_game();
  }
  return std::nullopt;
}

}  // namespace libchiaking::catalog::cloud_stream
