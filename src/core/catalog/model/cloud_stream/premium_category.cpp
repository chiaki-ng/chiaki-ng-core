#include "src/core/catalog/model/cloud_stream/premium_category.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

PremiumEntitlement::PremiumEntitlement(Facts facts) noexcept
    : facts_(std::move(facts)) {}

auto PremiumEntitlement::feature_named(std::string_view name) noexcept
    -> Feature {
  for (const auto feature : {Feature::full_game, Feature::trial,
                             Feature::play_first_trial, Feature::disc_required})
    if (name == name_of(feature)) return feature;
  return Feature::unknown;
}

auto PremiumEntitlement::name_of(Feature feature) noexcept -> std::string_view {
  switch (feature) {
    case Feature::full_game:
      return "FULL_GAME";
    case Feature::trial:
      return "TRIAL";
    case Feature::play_first_trial:
      return "PLAY_FIRST_TRIAL";
    case Feature::disc_required:
      return "DISC_REQUIRED";
    case Feature::unknown:
      break;
  }
  return {};
}

auto PremiumEntitlement::is_a_streamable_full_game() const noexcept -> bool {
  return facts_.package_type == game_package &&
         facts_.feature == Feature::full_game &&
         facts_.streaming_supported == true;
}

auto PremiumEntitlement::is_a_streamable(Feature feature) const noexcept
    -> bool {
  return facts_.package_type == game_package && facts_.feature == feature &&
         facts_.streaming_supported == true;
}

auto PremiumEntitlement::is_a_game() const noexcept -> bool {
  return facts_.package_type == game_package;
}

auto PremiumEntitlement::names_an_id() const noexcept -> bool {
  return facts_.id.has_value() || facts_.unreadable_id;
}

auto PremiumEntitlement::is_among(const std::set<OwnershipId>& ids) const
    -> bool {
  return facts_.id && ids.contains(*facts_.id);
}

PremiumSku::PremiumSku(std::optional<SkuId> id, bool premium_stream_branded,
                       std::vector<PremiumEntitlement> entitlements) noexcept
    : id_(std::move(id)),
      premium_stream_branded_(premium_stream_branded),
      entitlements_(std::move(entitlements)) {}

auto PremiumSku::first_streamable_full_game() const
    -> std::optional<PremiumEntitlement> {
  const auto found = std::ranges::find_if(
      entitlements_, &PremiumEntitlement::is_a_streamable_full_game);
  return found != entitlements_.end() ? std::optional{*found} : std::nullopt;
}

auto PremiumSku::is_wholly_owned_given(
    const std::set<OwnershipId>& owned_and_active) const -> bool {
  if (std::ranges::none_of(entitlements_, &PremiumEntitlement::names_an_id))
    return true;
  auto games = entitlements_ |
               std::views::filter([](const PremiumEntitlement& entitlement) {
                 return entitlement.is_a_game() && entitlement.names_an_id();
               });
  return !std::ranges::empty(games) &&
         std::ranges::all_of(games, [&](const PremiumEntitlement& entitlement) {
           return entitlement.is_among(owned_and_active);
         });
}

PremiumProduct::PremiumProduct(Facts facts) noexcept
    : facts_(std::move(facts)) {}

auto PremiumProduct::included_offer() const -> std::optional<Offer> {
  const auto branded =
      std::ranges::find_if(facts_.skus, &PremiumSku::is_premium_stream_branded);
  if (branded == facts_.skus.end()) return std::nullopt;
  auto accepted = branded->first_streamable_full_game();
  if (!accepted) return std::nullopt;
  return Offer{branded->id(), std::move(*accepted)};
}

auto PremiumProduct::is_bundle() const noexcept -> bool {
  return streamable_children().size() > 1;
}

auto PremiumProduct::streamable_children() const -> std::vector<Offer> {
  std::vector<Offer> children;
  std::set<OwnershipId> seen;
  for (const auto& sku : facts_.skus)
    for (const auto& entitlement : sku.entitlements())
      if (entitlement.is_a_streamable_full_game() && entitlement.id() &&
          seen.insert(*entitlement.id()).second)
        children.push_back(Offer{sku.id(), entitlement});
  return children;
}

auto PremiumProduct::offer_for_sale() const -> std::optional<Offer> {
  auto children = streamable_children();
  if (children.empty()) return std::nullopt;
  return std::move(children.front());
}

auto PremiumProduct::is_owned_given(
    const std::set<OwnershipId>& owned_and_active) const -> bool {
  return std::ranges::any_of(facts_.skus, [&](const PremiumSku& sku) {
    return sku.is_wholly_owned_given(owned_and_active);
  });
}

auto PremiumProduct::offers(PremiumEntitlement::Feature feature,
                            const std::set<OwnershipId>& owned_and_active) const
    -> bool {
  return std::ranges::any_of(facts_.skus, [&](const PremiumSku& sku) {
    return std::ranges::any_of(
        sku.entitlements(), [&](const PremiumEntitlement& entitlement) {
          return entitlement.is_a_streamable(feature) && entitlement.id() &&
                 !entitlement.is_among(owned_and_active);
        });
  });
}

PremiumCategory::PremiumCategory(StoreLocale locale,
                                 std::vector<PremiumProduct> products,
                                 Scan scan, Time fetched_at) noexcept
    : locale_(std::move(locale)),
      products_(std::move(products)),
      scan_(scan),
      fetched_at_(fetched_at) {}

auto PremiumCategory::included_count() const -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(products_, [](const PremiumProduct& product) {
        return product.included_offer().has_value();
      }));
}

auto PremiumCategory::is_plausible() const -> bool {
  std::set<TitleNumber> included;
  for (const auto& product : products_)
    if (product.included_offer())
      if (const auto title = product.id().title_number())
        included.insert(*title);
  return is_complete() && included.size() >= fewest_plausible;
}

}  // namespace libchiaking::catalog::cloud_stream
