#include "libchiaking/detail/catalog/game_details.hpp"

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::catalog {

auto LayeredArt::of(std::optional<ImageRef> background,
                    std::optional<ImageRef> hero, std::optional<ImageRef> logo)
    -> std::optional<LayeredArt> {
  if (!background || !hero || !logo) return std::nullopt;
  return LayeredArt{std::move(*background), std::move(*hero), std::move(*logo)};
}

auto GameHubArt::of(std::optional<ImageRef> cover, std::optional<ImageRef> logo)
    -> std::optional<GameHubArt> {
  if (!cover) return std::nullopt;
  return GameHubArt{std::move(*cover), std::move(logo)};
}

KeyArt::KeyArt(LayeredArt layered) noexcept : choice_(std::move(layered)) {}

KeyArt::KeyArt(GameHubArt game_hub) noexcept : choice_(std::move(game_hub)) {}

auto KeyArt::preferring_layers(std::optional<LayeredArt> layered,
                               std::optional<GameHubArt> game_hub)
    -> std::optional<KeyArt> {
  if (layered) return KeyArt{std::move(*layered)};
  if (game_hub) return KeyArt{std::move(*game_hub)};
  return std::nullopt;
}

auto KeyArt::choice() const noexcept -> const Choice& { return choice_; }

auto KeyArt::images() const -> std::vector<ImageRef> {
  return std::visit(
      [](const auto& art) -> std::vector<ImageRef> {
        using Art = std::decay_t<decltype(art)>;
        if constexpr (std::is_same_v<Art, LayeredArt>)
          return {art.background, art.hero, art.logo};
        else if (art.logo)
          return {art.cover, *art.logo};
        else
          return {art.cover};
      },
      choice_);
}

auto GameDetails::is_fresh_at(Time now) const noexcept -> bool {
  return is_fresh(fetched_at, lifetime, now);
}

auto GameDetails::images() const -> std::vector<ImageRef> {
  auto all = screenshots;
  if (key_art) std::ranges::copy(key_art->images(), std::back_inserter(all));
  if (age_rating && age_rating->icon) all.push_back(*age_rating->icon);
  return all;
}

}  // namespace libchiaking::catalog
