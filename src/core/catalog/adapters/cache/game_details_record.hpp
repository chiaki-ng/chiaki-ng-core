/// @file
/// @brief Declares cache::GameDetailsRecord, the stored form of a game's
/// store details (art, screenshots, trailers, ratings, publisher, release,
/// genres) for one locale.
///
/// Details are asked by concept when the game has one, otherwise by product,
/// and stored under details/{locale}/{"concept-" + concept, or product id}.
/// They are served for GameDetails::lifetime.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"

#include <libchiaking/detail/catalog/game_details.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One game's stored details.
///
/// Immutable; may be shared between threads.
class GameDetailsRecord {
 public:
  /// @brief Which game's details, in which locale.
  struct Key {
    /// @brief The store locale the details were asked in.
    StoreLocale locale;
    /// @brief The product asked about, when no concept was.
    std::optional<ProductId> product{};
    /// @brief The concept asked about; wins over the product.
    std::optional<ConceptId> concept_id{};
  };

  /// @brief Stored layered key art: three image URLs.
  struct Layers {
    /// @brief The background image URL.
    std::string background;
    /// @brief The hero image URL.
    std::string hero;
    /// @brief The logo image URL.
    std::string logo;
  };

  /// @brief Stored game hub art.
  struct GameHub {
    /// @brief The cover image URL.
    std::string cover;
    /// @brief The logo image URL, when there is one.
    std::optional<std::string> logo;
  };

  /// @brief One star level's share of the ratings.
  struct Share {
    /// @brief Percent of ratings, 0 to 100.
    std::int64_t percent{};
  };

  /// @brief A stored star rating.
  struct Stars {
    /// @brief The average stars.
    double average{};
    /// @brief The average as the store wrote it.
    std::string average_text;
    /// @brief How many ratings were given.
    std::int64_t ratings{};
    /// @brief Shares for each star level; exactly StarRating::most_stars
    /// entries.
    std::vector<Share> percent_by_stars;
  };

  /// @brief A stored age rating.
  struct Age {
    /// @brief The rating authority, such as PEGI or ESRB.
    std::string authority;
    /// @brief The rating's name.
    std::string name;
    /// @brief The rating's description.
    std::string description;
    /// @brief The rating icon URL, when there is one.
    std::optional<std::string> icon;
    /// @brief The content descriptors.
    std::vector<std::string> descriptors;
  };

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief The product id text, when the details were asked by product.
    std::optional<std::string> product;
    /// @brief The concept id text, when asked by concept.
    std::optional<std::string> concept_id;
    /// @brief When the details were fetched; wall-clock milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief Layered key art, when that is the art shown.
    std::optional<Layers> layered_art;
    /// @brief Game hub art, when that is the art shown.
    std::optional<GameHub> game_hub_art;
    /// @brief Screenshot URLs in store order.
    std::vector<std::string> screenshots;
    /// @brief Trailer URLs in store order.
    std::vector<std::string> trailers;
    /// @brief The star rating, when known.
    std::optional<Stars> star_rating;
    /// @brief The age rating, when known.
    std::optional<Age> age_rating;
    /// @brief The publisher's name; empty when unknown.
    std::string publisher;
    /// @brief The release time; wall-clock milliseconds.
    std::optional<std::int64_t> released_at_ms;
    /// @brief The genres in store order.
    std::vector<std::string> genres;
  };

  /// @brief The record kind and first name segment.
  static constexpr std::string_view kind = "details";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 1;
  /// @brief The leaf prefix that marks a record asked by concept.
  static constexpr std::string_view concept_prefix = "concept-";

  /// @brief Wraps details for storing.
  ///
  /// @param details The details.
  explicit GameDetailsRecord(GameDetails details) noexcept;

  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param key The key the record is stored under.
  /// @return The record; std::nullopt when the body is for another game
  ///     than the key, or any URL, the art, the star rating (wrong number
  ///     of levels or a share outside 0 to 100) or the age rating does not
  ///     read.
  [[nodiscard]] static auto from_body(const Body& body, const Key& key)
      -> std::optional<GameDetailsRecord>;
  /// @brief The record's name.
  ///
  /// @param key The key.
  /// @return details/{lower-case locale}/{lower-case "concept-" + concept,
  ///     or product}; std::nullopt when the key names neither or the name
  ///     is not valid.
  [[nodiscard]] static auto name_of(const Key& key)
      -> std::optional<platform::RecordName>;
  /// @brief The key a stored body was written under.
  ///
  /// @param locale The locale the record is stored under.
  /// @param body The stored body.
  /// @return The key, or std::nullopt when the body names neither a valid
  ///     product nor a valid concept.
  [[nodiscard]] static auto key_of(const StoreLocale& locale, const Body& body)
      -> std::optional<Key>;

  /// @brief The record's key.
  ///
  /// @return The details' locale, product and concept.
  auto key() const -> Key {
    return Key{details_.locale, details_.product, details_.concept_id};
  }
  /// @brief The details.
  ///
  /// @return The details, valid while the record lives.
  auto details() const noexcept -> const GameDetails& { return details_; }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares the details.
  friend auto operator==(const GameDetailsRecord&, const GameDetailsRecord&)
      -> bool = default;

 private:
  /// @brief Whether a body is for the game a key names.
  ///
  /// @param body The stored body.
  /// @param key The key.
  /// @return True when the concepts match, or, with no concept on either
  ///     side, the products match.
  static auto same_game(const Body& body, const Key& key) -> bool;
  /// @brief Stored layered art read back.
  ///
  /// @param wire The stored layers.
  /// @return The art, or std::nullopt when any URL is not acceptable.
  [[nodiscard]] static auto layered(const Layers& wire)
      -> std::optional<LayeredArt>;
  /// @brief Stored game hub art read back.
  ///
  /// @param wire The stored art.
  /// @return The art, or std::nullopt when a URL is not acceptable.
  [[nodiscard]] static auto game_hub(const GameHub& wire)
      -> std::optional<GameHubArt>;
  /// @brief The key art a body stores, preferring layers.
  ///
  /// @param body The stored body.
  /// @return An empty inner optional when no art is stored, the art when it
  ///     reads, and std::nullopt when stored art does not read.
  [[nodiscard]] static auto key_art(const Body& body)
      -> std::optional<std::optional<KeyArt>>;
  /// @brief Writes the shown key art into a body.
  ///
  /// @param body The body; one of its art fields is set.
  /// @param art The art.
  static void show_key_art(Body& body, const KeyArt& art);
  /// @brief A stored star rating read back.
  ///
  /// @param wire The stored rating.
  /// @return The rating, or std::nullopt when it has the wrong number of
  ///     levels or a share outside 0 to 100.
  [[nodiscard]] static auto stars(const Stars& wire)
      -> std::optional<StarRating>;
  /// @brief A stored age rating read back.
  ///
  /// @param wire The stored rating.
  /// @return The rating, or std::nullopt when its icon URL is not
  ///     acceptable.
  [[nodiscard]] static auto age(const Age& wire) -> std::optional<AgeRating>;
  /// @brief The stored form of a star rating.
  ///
  /// @param rating The rating.
  /// @return Its stored form.
  static auto stars(const StarRating& rating) -> Stars;
  /// @brief The stored form of an age rating.
  ///
  /// @param rating The rating.
  /// @return Its stored form.
  static auto age(const AgeRating& rating) -> Age;

  /// @brief The details.
  GameDetails details_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How stored layered art is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::Layers> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::Layers;
  /// @brief The JSON keys background, hero and logo.
  static constexpr auto value = glz::object("background", &T::background,
                                            "hero", &T::hero, "logo", &T::logo);
};

/// @brief How stored game hub art is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::GameHub> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::GameHub;
  /// @brief The JSON keys cover and logo.
  static constexpr auto value =
      glz::object("cover", &T::cover, "logo", &T::logo);
};

/// @brief How a star level share is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::Share> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::Share;
  /// @brief The JSON keys percent.
  static constexpr auto value = glz::object("percent", &T::percent);
};

/// @brief How a stored star rating is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::Stars> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::Stars;
  /// @brief The JSON keys average, averageText, ratings and percentByStars.
  static constexpr auto value = glz::object(
      "average", &T::average, "averageText", &T::average_text, "ratings",
      &T::ratings, "percentByStars", &T::percent_by_stars);
};

/// @brief How a stored age rating is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::Age> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::Age;
  /// @brief The JSON keys authority, name, description, icon and descriptors.
  static constexpr auto value = glz::object(
      "authority", &T::authority, "name", &T::name, "description",
      &T::description, "icon", &T::icon, "descriptors", &T::descriptors);
};

/// @brief How a game details record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::GameDetailsRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::GameDetailsRecord::Body;
  /// @brief The JSON keys product, concept, fetchedAtMs, layeredArt,
  /// gameHubArt, screenshots, trailers, starRating, ageRating, publisher,
  /// releasedAtMs and genres.
  static constexpr auto value = glz::object(
      "product", &T::product, "concept", &T::concept_id, "fetchedAtMs",
      &T::fetched_at_ms, "layeredArt", &T::layered_art, "gameHubArt",
      &T::game_hub_art, "screenshots", &T::screenshots, "trailers",
      &T::trailers, "starRating", &T::star_rating, "ageRating", &T::age_rating,
      "publisher", &T::publisher, "releasedAtMs", &T::released_at_ms, "genres",
      &T::genres);
};
