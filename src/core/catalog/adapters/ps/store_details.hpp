/// @file
/// @brief Declares the store page adapters: ps::StoreDetails, which reads a
/// game's store details (art, screenshots, trailers, ratings, publisher,
/// release date, genres) for the details page, ps::StoreImages, which
/// downloads one store image, and ps::StorePages, which finds the store page
/// a game links to.
///
/// StoreDetails asks about a game with a concept through the public web
/// GraphQL concept queries, and about a game without one through the store's
/// products endpoint.
#pragma once

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/psn/graphql/public_web/client.hpp"

#include <libchiaking/detail/catalog/content_url.hpp>
#include <libchiaking/detail/catalog/game_details.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/store_page.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Reads one game's store details.
///
/// Borrows the client and clock, which must outlive it.
class StoreDetails {
 public:
  /// @brief Details read by concept and whether every part was read.
  struct Found {
    /// @brief The details.
    GameDetails details;
    /// @brief False when the star rating, age rating or info query failed, so
    /// the details should not be kept as complete.
    bool whole{};
  };

  /// @brief The media role of layered art's background.
  static constexpr std::string_view background_role = "BACKGROUND_LAYER_ART";
  /// @brief The media role of layered art's hero.
  static constexpr std::string_view hero_role = "HERO_CHARACTER";
  /// @brief The media role of the logo.
  static constexpr std::string_view logo_role = "LOGO";
  /// @brief The media role of game hub cover art.
  static constexpr std::string_view game_hub_role = "GAMEHUB_COVER_ART";
  /// @brief The media role of screenshots.
  static constexpr std::string_view screenshot_role = "SCREENSHOT";
  /// @brief The media role of trailers.
  static constexpr std::string_view trailer_role = "PREVIEW";
  /// @brief The media type of images.
  static constexpr std::string_view image_type = "IMAGE";
  /// @brief The media type of videos.
  static constexpr std::string_view video_type = "VIDEO";

  /// @brief Sets up the reads.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param threading Starts the threads a concept's reads run on. Must
  ///     outlive this object.
  /// @param wall Stamps the details. Must outlive this object.
  StoreDetails(net::http::Client& http, platform::ThreadingProvider& threading,
               const platform::ClockProvider& wall) noexcept;

  /// @brief Reads a product's details through the products endpoint.
  ///
  /// Blocks on one request.
  ///
  /// @param product The product.
  /// @param locale The store locale.
  /// @param age The account's real age.
  /// @return The details; Unanswered::Reason::not_listed when the reply does
  /// not
  ///     hold the product or flags it with an error code; otherwise the
  ///     request's failure.
  [[nodiscard]] auto of_product(const ProductId& product,
                                const StoreLocale& locale, int age)
      -> Expected<GameDetails, Unanswered>;
  /// @brief Reads a concept's details through four GraphQL queries.
  ///
  /// Media is asked on the calling thread while star rating, age rating and
  /// info are asked on worker threads (on the calling thread when none can
  /// start); blocks until all four return.
  ///
  /// @param grouped_under The concept.
  /// @param locale The store locale.
  /// @return The details, whole only when every query succeeded; a
  ///     cancellation when stopped; the media query's failure.
  [[nodiscard]] auto of_concept(const ConceptId& grouped_under,
                                const StoreLocale& locale)
      -> Expected<Found, Unanswered>;

 private:
  /// @brief A view of media items.
  using Media = std::span<const psn::graphql::StoreMedia>;
  /// @brief A tolerant text field.
  using Text = json::ps::Text;
  /// @brief A field read when it has the expected shape, empty otherwise.
  ///
  /// @tparam Value The expected shape.
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;

  /// @brief One media item of a products reply. Plain data.
  struct Shown {
    /// @brief The item's role, such as SCREENSHOT.
    Text type;
    /// @brief The item's URL.
    Text url;
  };
  /// @brief A products reply's media. Plain data.
  struct Gallery {
    /// @brief The images.
    StoreList<Shown> images;
    /// @brief The videos.
    StoreList<Shown> videos;
  };
  /// @brief One star level's vote count. Plain data.
  struct Vote {
    /// @brief The star level, 1 to 5.
    Tolerant<json::ps::Number> score;
    /// @brief How many votes it got.
    Tolerant<json::ps::Number> count;
  };
  /// @brief A products reply's star rating. Plain data.
  struct Stars {
    /// @brief The average as the store wrote it.
    Text score;
    /// @brief The votes per star level.
    StoreList<Vote> count;
  };
  /// @brief One content descriptor. Plain data.
  struct Descriptor {
    /// @brief Its text.
    Text description;
  };
  /// @brief A products reply's age rating. Plain data.
  struct Rated {
    /// @brief The rating authority.
    Text authority;
    /// @brief The rating's name.
    Text name;
    /// @brief The rating's description.
    Text description;
    /// @brief The high-resolution icon URL.
    Text url4k;
    /// @brief The icon URL.
    Text url;
    /// @brief The content descriptors.
    StoreList<Descriptor> descriptors;
  };
  /// @brief One localised genre. Plain data.
  struct Genre {
    /// @brief The genre's name.
    Text value;
  };
  /// @brief The concept block of a products reply. Plain data.
  struct Grouped {
    /// @brief Fields are read under their camelCase names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The concept's media.
    Tolerant<Gallery> media;
    /// @brief The concept's publisher.
    Text publisher_name;
  };
  /// @brief One product of a products reply. Plain data.
  struct Sold {
    /// @brief The product id.
    Text id;
    /// @brief Present when the store could not serve the product.
    std::optional<glz::raw_json> error_code;
    /// @brief The product's media.
    Tolerant<Gallery> media;
    /// @brief The product's concept.
    Tolerant<Grouped> grouped;
    /// @brief The star rating.
    Tolerant<Stars> star_rating;
    /// @brief The age rating.
    Tolerant<Rated> content_rating;
    /// @brief The publisher.
    Text lead_publisher_name;
    /// @brief The release date text.
    Text release_date;
    /// @brief The genres.
    StoreList<Genre> localized_genres;

    /// @brief How a product is read: camelCase keys, with the concept under
    /// "concept".
    struct glaze {
      /// @brief The type described.
      using T = Sold;
      /// @brief The keys read.
      static constexpr auto value = glz::object(
          "id", &T::id, "errorCode", &T::error_code, "media", &T::media,
          "concept", &T::grouped, "starRating", &T::star_rating,
          "contentRating", &T::content_rating, "leadPublisherName",
          &T::lead_publisher_name, "releaseDate", &T::release_date,
          "localizedGenres", &T::localized_genres);
    };
  };

  /// @brief Every image with a role.
  ///
  /// @param media The media.
  /// @param role The role.
  /// @return The acceptable image URLs in order.
  static auto images_in(Media media, std::string_view role)
      -> std::vector<ImageRef>;
  /// @brief Every trailer video.
  ///
  /// @param media The media.
  /// @return The acceptable trailer URLs in order.
  static auto trailers_in(Media media) -> std::vector<TrailerRef>;
  /// @brief The first image with a role.
  ///
  /// @param media The media.
  /// @param role The role.
  /// @return The image, or std::nullopt when none.
  [[nodiscard]] static auto image_in(Media media, std::string_view role)
      -> std::optional<ImageRef>;
  /// @brief The concept's image with a role, else the product's.
  ///
  /// @param of_product The product's media.
  /// @param of_concept The concept's media.
  /// @param role The role.
  /// @return The image, or std::nullopt when neither has one.
  [[nodiscard]] static auto concept_first(Media of_product, Media of_concept,
                                          std::string_view role)
      -> std::optional<ImageRef>;
  /// @brief The key art: the concept's layered art when complete, else game hub
  /// art.
  ///
  /// @param of_product The product's media.
  /// @param of_concept The concept's media.
  /// @return The art, or std::nullopt when neither form is complete.
  [[nodiscard]] static auto key_art_of(Media of_product, Media of_concept)
      -> std::optional<KeyArt>;
  /// @brief Fills the key art, screenshots and trailers, preferring the
  /// product's screenshots and trailers over the concept's.
  ///
  /// @param details The details to fill.
  /// @param of_product The product's media.
  /// @param of_concept The concept's media.
  static void show_media(GameDetails& details, Media of_product,
                         Media of_concept);
  /// @brief A products reply's media as GraphQL media items.
  ///
  /// @param owner The media block.
  /// @return The images then the videos; empty when absent.
  static auto media_in(const Tolerant<Gallery>& owner)
      -> std::vector<psn::graphql::StoreMedia>;
  /// @brief Computes the star rating from the vote counts.
  ///
  /// @param product The product.
  /// @return The rating with its average and rounded percentages, or
  ///     std::nullopt when there are no votes.
  [[nodiscard]] static auto star_rating_in(const Sold& product)
      -> std::optional<StarRating>;
  /// @brief Reads the age rating.
  ///
  /// @param product The product.
  /// @return The rating, preferring the high-resolution icon, or std::nullopt
  ///     when absent or both its name and description are empty.
  [[nodiscard]] static auto age_rating_in(const Sold& product)
      -> std::optional<AgeRating>;
  /// @brief Reads the genres.
  ///
  /// @param product The product.
  /// @return The non-empty genre names.
  static auto genres_in(const Sold& product) -> std::vector<std::string>;
  /// @brief Converts a GraphQL star rating.
  ///
  /// @param rating The rating.
  /// @return The same rating.
  static auto star_rating_of(const psn::graphql::StarRating& rating)
      -> StarRating;
  /// @brief Converts a GraphQL content rating.
  ///
  /// @param rating The rating.
  /// @return The same rating.
  static auto age_rating_of(const psn::graphql::ContentRating& rating)
      -> AgeRating;
  /// @brief Whether a query was cancelled.
  ///
  /// @param error The failure.
  /// @return True for a cancellation.
  static auto gave_up(const psn::graphql::GraphqlError& error) noexcept -> bool;
  /// @brief Maps a public store GraphQL failure to a catalog failure.
  ///
  /// @param error The failure.
  /// @return A cancellation; Unanswered::Reason::malformed for unreadable
  ///     replies; Unanswered::Reason::not_listed for not found;
  ///     Unanswered::Reason::rejected for refused, invalid or partial requests;
  ///     Unanswered::Reason::unavailable for transport and server errors.
  static auto unanswered(const psn::graphql::GraphqlError& error) noexcept
      -> Unanswered;

  /// @brief Sends requests; not owned. Shared by the worker threads.
  net::http::Client& http_;
  /// @brief Starts the threads a concept's reads run on.
  platform::ThreadingProvider& threading_;
  /// @brief Stamps the details.
  WallStamp stamp_;
};

/// @brief Downloads store images through one client.
///
/// Borrows the client, which must outlive it.
class StoreImages {
 public:
  /// @brief Uses a client.
  ///
  /// @param http Sends the request. Must outlive this object.
  explicit StoreImages(net::http::Client& http) noexcept : http_(http) {}

  /// @brief Downloads an image.
  ///
  /// Blocks on one request.
  ///
  /// @param image The image's URL.
  /// @return The bytes; a cancellation; Unanswered::Reason::not_listed for 404;
  ///     Unanswered::Reason::unavailable for 5xx or a transport failure;
  ///     Unanswered::Reason::rejected for any other status or a body larger
  ///     than ImageHost::largest_bytes; Unanswered::Reason::malformed for an
  ///     empty body.
  [[nodiscard]] auto bytes_of(const ImageRef& image)
      -> Expected<std::vector<std::byte>, Unanswered>;

 private:
  /// @brief The endpoint images are fetched through, capped at
  /// ImageHost::largest_bytes.
  static constexpr net::http::Endpoint image_endpoint{
      .label = "psn.store.image",
      .maximum_response_bytes = ImageHost::largest_bytes,
  };

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
};

/// @brief Checks that a product's store page exists, falling back to its
/// concept page.
///
/// Borrows the HTTP client, which must outlive it.
class StorePages {
 public:
  /// @brief Uses a client.
  ///
  /// @param http Sends the existence check. Must outlive this object.
  explicit StorePages(net::http::Client& http) noexcept : http_(http) {}

  /// @brief The store page for a product.
  ///
  /// Builds the product page for the locale and asks the store whether it
  /// exists; blocks on that request.
  ///
  /// @param product The product.
  /// @param locale The store locale.
  /// @param concept_page The page to use when the product page does not exist.
  /// @return The product page when it exists, else the concept page when given;
  ///     a cancellation when the client was stopped; otherwise
  ///     Unanswered::Reason::not_listed with the HTTP status the check was
  ///     refused with (0 when there was none).
  [[nodiscard]] auto page_of(const ProductId& product,
                             const StoreLocale& locale,
                             const std::optional<StorePage>& concept_page = {})
      -> Expected<StorePage, Unanswered>;

 private:
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
};

}  // namespace libchiaking::catalog::ps
