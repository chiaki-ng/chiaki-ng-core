#include "libchiaking/detail/catalog/owned_title.hpp"

#include "src/core/catalog/model/list/title_name.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

auto PrintableIdentifier::admits(std::string_view text) noexcept -> bool {
  const auto printable = [](char value) { return value > ' ' && value <= '~'; };
  return !text.empty() && text.size() <= limit &&
         std::ranges::all_of(text, printable);
}

auto OwnershipId::parse(std::string_view text) -> std::optional<OwnershipId> {
  auto admitted = Text::parse(text);
  if (!admitted) return std::nullopt;
  return OwnershipId{std::move(*admitted)};
}

auto OwnershipId::title_number() const -> std::optional<TitleNumber> {
  return TitleNumber::from(text());
}

auto OwnershipId::title_key() const -> std::optional<TitleKey> {
  return TitleKey::of(text());
}

auto OwnershipId::as_product() const -> std::optional<ProductId> {
  return ProductId::parse(text());
}

OwnedTitle::OwnedTitle(Facts facts) noexcept : facts_(std::move(facts)) {}

auto OwnedTitle::from(Facts facts) -> Expected<OwnedTitle, Invalid> {
  const TitleName name{facts.name};
  if (facts.service == ServiceGeneration::ps5_cloud)
    return OwnedTitle{std::move(facts)};
  if (name.is_demo()) return unexpected(Invalid::demo_name);
  if (name.is_trial()) return unexpected(Invalid::trial_name);
  return OwnedTitle{std::move(facts)};
}

auto OwnedTitle::title_number() const -> std::optional<TitleNumber> {
  if (facts_.product)
    if (auto number = facts_.product->title_number()) return number;
  return facts_.id.title_number();
}

auto OwnedTitle::is_held_under(const ProductId& product) const noexcept
    -> bool {
  return (facts_.product && *facts_.product == product) ||
         facts_.id == std::string_view{product.text()};
}

auto OwnedTitle::released_no_later_than(ReleaseDate other) const -> OwnedTitle {
  auto facts = facts_;
  facts.release = facts.release.earliest(other);
  return OwnedTitle{std::move(facts)};
}

auto OwnedTitle::as_ended_trial() const -> OwnedTitle {
  auto facts = facts_;
  facts.trial_ended = true;
  return OwnedTitle{std::move(facts)};
}

}  // namespace libchiaking::catalog
