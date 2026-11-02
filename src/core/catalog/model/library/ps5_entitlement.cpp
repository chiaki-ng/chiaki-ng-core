#include "src/core/catalog/model/library/ps5_entitlement.hpp"

#include "src/core/catalog/model/list/title_name.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

Ps5Entitlement::Ps5Entitlement(Facts facts) noexcept
    : facts_(std::move(facts)) {}

auto Ps5Entitlement::titles_of(std::span<const Ps5Entitlement> owned)
    -> std::vector<OwnedTitle> {
  std::vector<OwnedTitle> titles;
  for (const auto& entitlement : owned) {
    auto title = entitlement.as_owned_title();
    if (title &&
        std::ranges::find(titles, title->id(), &OwnedTitle::id) == titles.end())
      titles.push_back(std::move(*title));
  }
  return titles;
}

auto Ps5Entitlement::why_not_a_game() const -> std::optional<NotAGame> {
  if (!facts_.product) return NotAGame::no_product;
  if (facts_.active != true) return NotAGame::inactive;
  if (const auto refused = feature_refusal()) return refused;
  if (facts_.entitlement_type != unified_entitlement)
    return NotAGame::not_unified;
  if (facts_.package_type != game_package) return NotAGame::other_package;
  if (const auto refused = streaming_refusal()) return refused;
  if (facts_.feature_type == play_first_trial_feature &&
      facts_.play_time_used_up == true)
    return NotAGame::play_first_trial;
  return std::nullopt;
}

auto Ps5Entitlement::feature_refusal() const noexcept
    -> std::optional<NotAGame> {
  if (facts_.feature_type == full_game_feature || is_trial())
    return std::nullopt;
  if (facts_.feature_type == disc_required_feature)
    return NotAGame::disc_required;
  return NotAGame::other_feature;
}

auto Ps5Entitlement::is_trial() const noexcept -> bool {
  return facts_.feature_type == trial_feature ||
         facts_.feature_type == play_first_trial_feature;
}

auto Ps5Entitlement::streaming_refusal() const noexcept
    -> std::optional<NotAGame> {
  if (!facts_.streaming_supported) return NotAGame::streaming_unknown;
  if (!*facts_.streaming_supported) return NotAGame::streaming_not_supported;
  return std::nullopt;
}

auto Ps5Entitlement::release() const -> ReleaseDate {
  if (!is_release_gated()) return ReleaseDate::unrestricted();
  const auto& playable =
      facts_.playable_from ? facts_.playable_from : facts_.active_date;
  if (!playable) return ReleaseDate::unannounced();
  const auto time = ReleaseDate::parse_time(*playable);
  if (!time || *time <= ReleaseDate::Time{}) return ReleaseDate::unannounced();
  return ReleaseDate::at(*time);
}

auto Ps5Entitlement::is_release_gated() const noexcept -> bool {
  return facts_.preorder_core || facts_.preorder_placeholder;
}

auto Ps5Entitlement::is_named_like_an_extra() const -> bool {
  const TitleName name{facts_.name};
  return name.is_demo() || name.is_trial() || name.is_digital_extra();
}

auto Ps5Entitlement::as_owned_title() const -> std::optional<OwnedTitle> {
  if (why_not_a_game()) return std::nullopt;
  return title(false);
}

auto Ps5Entitlement::as_ended_trial() const -> std::optional<OwnedTitle> {
  if (why_not_a_game() != NotAGame::play_first_trial) return std::nullopt;
  return title(true);
}

auto Ps5Entitlement::title(bool trial_ended) const
    -> std::optional<OwnedTitle> {
  OwnedTitle::Facts facts{.id = facts_.id};
  facts.name = facts_.name;
  facts.platform = Platform::ps5;
  facts.service = ServiceGeneration::ps5_cloud;
  facts.held = is_trial() ? HeldAs::trial_access : HeldAs::full_game;
  facts.release = release();
  facts.product = facts_.product;
  facts.concept_id = facts_.concept_id;
  facts.cover_url = facts_.cover_url;
  facts.streams = true;
  facts.trial_ended = trial_ended;
  facts.play_first_trial = facts_.feature_type == play_first_trial_feature;
  auto owned = OwnedTitle::from(std::move(facts));
  if (!owned) return std::nullopt;
  return std::move(owned).value();
}

}  // namespace libchiaking::catalog
