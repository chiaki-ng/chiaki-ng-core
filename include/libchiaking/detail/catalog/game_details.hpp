/// @file
/// @brief Declares GameDetails and the art and rating values a game's detail
/// page shows.
#pragma once

#include "libchiaking/detail/catalog/content_url.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_locale.hpp"
#include "libchiaking/detail/catalog/title.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::catalog {

/// @brief The three images PS layers to make a game's key art.
struct LayeredArt {
  /// @brief The backdrop layer.
  ImageRef background;
  /// @brief The hero image layer.
  ImageRef hero;
  /// @brief The game's logo layer.
  ImageRef logo;

  /// @brief Builds layered art when all three layers exist.
  ///
  /// @param background The backdrop, if the store has one.
  /// @param hero The hero image, if the store has one.
  /// @param logo The logo, if the store has one.
  /// @return The art, or nothing when any layer is missing.
  [[nodiscard]] static auto of(std::optional<ImageRef> background,
                               std::optional<ImageRef> hero,
                               std::optional<ImageRef> logo)
      -> std::optional<LayeredArt>;

  /// @brief Compares the three images.
  friend auto operator==(const LayeredArt&, const LayeredArt&)
      -> bool = default;
};

/// @brief The single flattened cover PS makes for a game's hub page, with
/// the game's logo when the store has one.
///
/// It is the fallback key art for a game whose store media lack one of the
/// three layers of LayeredArt. The cover is PS's `GAMEHUB_COVER_ART` image,
/// the concept's in preference to the product's.
struct GameHubArt {
  /// @brief The game hub cover, already composed into one picture.
  ImageRef cover;
  /// @brief The game's logo, to draw over the cover; empty when the store has
  /// none.
  std::optional<ImageRef> logo;

  /// @brief Builds game hub art when the cover exists.
  ///
  /// @param cover The game hub cover, if the store has one.
  /// @param logo The logo, if the store has one.
  /// @return The art, or nothing when the cover is missing; a missing logo
  ///     alone does not prevent it.
  [[nodiscard]] static auto of(std::optional<ImageRef> cover,
                               std::optional<ImageRef> logo)
      -> std::optional<GameHubArt>;

  /// @brief Compares the cover and the logo.
  friend auto operator==(const GameHubArt&, const GameHubArt&)
      -> bool = default;
};

/// @brief The art a detail page shows behind a game: either PS's three
/// layered images or, failing those, the game hub cover.
///
/// Always holds exactly one of the two. An immutable value; copies are
/// independent and safe to share between threads.
class KeyArt {
 public:
  /// @brief The art held: LayeredArt when all three layers exist, otherwise
  /// GameHubArt.
  using Choice = std::variant<LayeredArt, GameHubArt>;

  /// @brief Holds layered key art.
  ///
  /// @param layered The three layers.
  explicit KeyArt(LayeredArt layered) noexcept;
  /// @brief Holds the game hub cover as the key art.
  ///
  /// @param game_hub The cover and, when known, the logo.
  explicit KeyArt(GameHubArt game_hub) noexcept;

  /// @brief Chooses the key art for a game from what the store offers.
  ///
  /// Pure; makes no request.
  ///
  /// @param layered The layered art, if every layer exists.
  /// @param game_hub The game hub art, if the cover exists.
  /// @return The layered art when there is some, otherwise the game hub
  ///     art, or nothing when neither exists.
  [[nodiscard]] static auto preferring_layers(
      std::optional<LayeredArt> layered, std::optional<GameHubArt> game_hub)
      -> std::optional<KeyArt>;

  /// @brief Gives the art that was chosen, for a frontend to draw.
  ///
  /// @return The held art; valid while this KeyArt lives.
  auto choice() const noexcept -> const Choice&;
  /// @brief Lists every image the key art draws, for fetching or caching.
  ///
  /// @return The background, hero and logo for layered art, in that order;
  ///     the cover followed by the logo, when there is one, for game hub
  ///     art.
  auto images() const -> std::vector<ImageRef>;

  /// @brief Compares the art held.
  friend auto operator==(const KeyArt&, const KeyArt&) -> bool = default;

 private:
  Choice choice_;
};

/// @brief The store's player rating for a game.
struct StarRating {
  /// @brief The highest rating a player can give.
  static constexpr std::size_t most_stars = 5;

  /// @brief Mean rating, in stars.
  double average{};
  /// @brief The mean as the store formats it for display.
  std::string average_text;
  /// @brief How many players rated the game.
  std::int64_t ratings{};
  /// @brief Share of ratings for each star count, in percent; index 0 is one
  /// star.
  std::array<int, most_stars> percent_by_stars{};

  /// @brief Compares every field.
  friend auto operator==(const StarRating&, const StarRating&)
      -> bool = default;
};

/// @brief The age rating the store shows for a game in one locale.
struct AgeRating {
  /// @brief The body that issued the rating.
  std::string authority;
  /// @brief The rating itself, as the authority names it.
  std::string name;
  /// @brief The store's text for the rating.
  std::string description;
  /// @brief The rating's badge; empty when the store gives none on its image
  /// host.
  std::optional<ImageRef> icon;
  /// @brief Content descriptors that accompany the rating.
  std::vector<std::string> descriptors;

  /// @brief Compares every field.
  friend auto operator==(const AgeRating&, const AgeRating&) -> bool = default;
};

/// @brief What the store says about one game beyond its name and cover.
///
/// Details are fetched for a product or for a concept, in one locale, and may
/// be reused for GameDetails::lifetime. Optional parts are empty when the store
/// has nothing or that part of the fetch failed.
///
/// @see Catalog::details_of
struct GameDetails {
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief How long fetched details may be reused.
  static constexpr std::chrono::hours lifetime{24};

  /// @brief The product that was asked about; empty when a concept was asked
  /// about.
  std::optional<ProductId> product{};
  /// @brief The concept the details were fetched for; empty when the product
  /// itself answered.
  std::optional<ConceptId> concept_id{};
  /// @brief The store locale the texts and ratings are in.
  StoreLocale locale;
  /// @brief When the details were fetched.
  Time fetched_at{};
  /// @brief The art to show behind the game, preferring the concept's three
  /// layers and falling back to the game hub cover; empty when the store has
  /// neither.
  std::optional<KeyArt> key_art{};
  /// @brief Screenshots of the product, or of its concept when the product
  /// has none.
  std::vector<ImageRef> screenshots{};
  /// @brief Trailers of the product, or of its concept when the product has
  /// none.
  std::vector<TrailerRef> trailers{};
  /// @brief The player rating, when the store has one.
  std::optional<StarRating> star_rating{};
  /// @brief The age rating, when the store has one.
  std::optional<AgeRating> age_rating{};
  /// @brief The publisher's name; empty when not known.
  std::string publisher{};
  /// @brief The release date the store states; empty when missing or
  /// unreadable.
  std::optional<Time> released_at{};
  /// @brief Genre names in the details' locale.
  std::vector<std::string> genres{};

  /// @brief Whether the details are still recent enough to reuse.
  ///
  /// @param now The current wall-clock time.
  /// @return True from GameDetails::fetched_at until GameDetails::lifetime
  ///     has passed; false for a time before the fetch.
  auto is_fresh_at(Time now) const noexcept -> bool;
  /// @brief Lists every image the details refer to, for preloading or cache
  /// clean-up.
  ///
  /// @return The screenshots, then the three art layers, then the age
  ///     rating's icon.
  auto images() const -> std::vector<ImageRef>;

  /// @brief Compares every field.
  friend auto operator==(const GameDetails&, const GameDetails&)
      -> bool = default;
};

}  // namespace libchiaking::catalog
