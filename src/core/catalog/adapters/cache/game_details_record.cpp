#include "src/core/catalog/adapters/cache/game_details_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::catalog::cache {

GameDetailsRecord::GameDetailsRecord(GameDetails details) noexcept
    : details_(std::move(details)) {}

auto GameDetailsRecord::name_of(const Key& key)
    -> std::optional<platform::RecordName> {
  if (!key.concept_id && !key.product) return std::nullopt;
  auto leaf = key.concept_id
                  ? std::string{concept_prefix} + key.concept_id->text()
                  : key.product->text();
  std::ranges::transform(leaf, leaf.begin(), text::ascii_lower);
  const auto shelf = Cache::shared_record(kind, key.locale.lowercase());
  return shelf ? shelf->child(leaf) : std::nullopt;
}

auto GameDetailsRecord::key_of(const StoreLocale& locale, const Body& body)
    -> std::optional<Key> {
  auto product = ProductId::parse(body.product.value_or(""));
  auto concept_id = ConceptId::parse(body.concept_id.value_or(""));
  if (!product && !concept_id) return std::nullopt;
  return Key{locale, std::move(product), std::move(concept_id)};
}

auto GameDetailsRecord::stars(const StarRating& rating) -> Stars {
  Stars wire{rating.average, rating.average_text, rating.ratings, {}};
  for (const auto percent : rating.percent_by_stars)
    wire.percent_by_stars.push_back({percent});
  return wire;
}

auto GameDetailsRecord::age(const AgeRating& rating) -> Age {
  Age wire{rating.authority, rating.name, rating.description, std::nullopt,
           rating.descriptors};
  if (rating.icon) wire.icon = rating.icon->url();
  return wire;
}

auto GameDetailsRecord::body() const -> Body {
  Body body{.fetched_at_ms = milliseconds_of(details_.fetched_at),
            .screenshots = details_.screenshots |
                           std::views::transform(&ImageRef::url) |
                           std::ranges::to<std::vector<std::string>>(),
            .trailers = details_.trailers |
                        std::views::transform(&TrailerRef::url) |
                        std::ranges::to<std::vector<std::string>>(),
            .publisher = details_.publisher,
            .genres = details_.genres};
  if (details_.product) body.product = details_.product->text();
  if (details_.concept_id) body.concept_id = details_.concept_id->text();
  if (details_.key_art) show_key_art(body, *details_.key_art);
  if (details_.star_rating) body.star_rating = stars(*details_.star_rating);
  if (details_.age_rating) body.age_rating = age(*details_.age_rating);
  if (details_.released_at)
    body.released_at_ms = milliseconds_of(*details_.released_at);
  return body;
}

auto GameDetailsRecord::layered(const Layers& wire)
    -> std::optional<LayeredArt> {
  return LayeredArt::of(ImageRef::at(wire.background), ImageRef::at(wire.hero),
                        ImageRef::at(wire.logo));
}

auto GameDetailsRecord::game_hub(const GameHub& wire)
    -> std::optional<GameHubArt> {
  auto logo = stated<ImageRef>(wire.logo, &ImageRef::at);
  if (!logo) return std::nullopt;
  return GameHubArt::of(ImageRef::at(wire.cover), std::move(*logo));
}

auto GameDetailsRecord::key_art(const Body& body)
    -> std::optional<std::optional<KeyArt>> {
  std::optional<LayeredArt> layers;
  if (body.layered_art && !(layers = layered(*body.layered_art)))
    return std::nullopt;
  std::optional<GameHubArt> cover;
  if (body.game_hub_art && !(cover = game_hub(*body.game_hub_art)))
    return std::nullopt;
  return KeyArt::preferring_layers(std::move(layers), std::move(cover));
}

void GameDetailsRecord::show_key_art(Body& body, const KeyArt& art) {
  std::visit(
      [&body](const auto& shown) {
        using Art = std::decay_t<decltype(shown)>;
        if constexpr (std::is_same_v<Art, LayeredArt>)
          body.layered_art = Layers{shown.background.url(), shown.hero.url(),
                                    shown.logo.url()};
        else
          body.game_hub_art = GameHub{
              shown.cover.url(),
              shown.logo ? std::optional{shown.logo->url()} : std::nullopt};
      },
      art.choice());
}

auto GameDetailsRecord::stars(const Stars& wire) -> std::optional<StarRating> {
  if (wire.percent_by_stars.size() != StarRating::most_stars)
    return std::nullopt;
  StarRating rating{wire.average, wire.average_text, wire.ratings, {}};
  for (std::size_t index = 0; index < StarRating::most_stars; ++index) {
    const auto percent = wire.percent_by_stars[index].percent;
    if (percent < 0 || percent > 100) return std::nullopt;
    rating.percent_by_stars[index] = static_cast<int>(percent);
  }
  return rating;
}

auto GameDetailsRecord::age(const Age& wire) -> std::optional<AgeRating> {
  auto icon = stated<ImageRef>(wire.icon, &ImageRef::at);
  if (!icon) return std::nullopt;
  return AgeRating{wire.authority, wire.name, wire.description,
                   std::move(*icon), wire.descriptors};
}

auto GameDetailsRecord::same_game(const Body& body, const Key& key) -> bool {
  if (key.concept_id) return body.concept_id == key.concept_id->text();
  return !body.concept_id && key.product && body.product == key.product->text();
}

auto GameDetailsRecord::from_body(const Body& body, const Key& key)
    -> std::optional<GameDetailsRecord> {
  auto screenshots = every<ImageRef>(body.screenshots, &ImageRef::at);
  auto trailers = every<TrailerRef>(body.trailers, &TrailerRef::at);
  auto art = key_art(body);
  if (!same_game(body, key) || !screenshots || !trailers || !art)
    return std::nullopt;
  GameDetails details{.product = key.product,
                      .concept_id = key.concept_id,
                      .locale = key.locale,
                      .fetched_at = time_of(body.fetched_at_ms),
                      .key_art = std::move(*art),
                      .screenshots = std::move(*screenshots),
                      .trailers = std::move(*trailers),
                      .publisher = body.publisher,
                      .released_at = time_of(body.released_at_ms),
                      .genres = body.genres};
  if (body.star_rating && !(details.star_rating = stars(*body.star_rating)))
    return std::nullopt;
  if (body.age_rating && !(details.age_rating = age(*body.age_rating)))
    return std::nullopt;
  return GameDetailsRecord{std::move(details)};
}

}  // namespace libchiaking::catalog::cache
