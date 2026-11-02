/// @file
/// @brief Declares PublicWebClient, the store web site's anonymous GraphQL
/// calls the catalogue describes games with: category grids, media, star and
/// age ratings, and game information.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/psn/graphql/content_rating.hpp"
#include "src/core/psn/graphql/error.hpp"
#include "src/core/psn/graphql/genres.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/psn/graphql/release_date.hpp"
#include "src/core/psn/graphql/reply_integer.hpp"
#include "src/core/psn/graphql/reply_strings.hpp"
#include "src/core/psn/graphql/retrieved.hpp"
#include "src/core/psn/graphql/sender.hpp"
#include "src/core/psn/graphql/stats.hpp"
#include "src/core/psn/graphql/title_media.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief A product on a category grid. Plain data.
struct GridProduct {
  /// @brief The product id; empty when absent.
  std::string id;
  /// @brief Whether the product is included with PlayStation Plus: its price
  /// is branded `PS_PLUS`, or its upsell is branded `PS_PLUS` with the text
  /// `Extra`, `Essential` or `Included`.
  bool ps_plus{};
  /// @brief The upsell text when the upsell is branded `PS_PLUS`; empty
  /// otherwise.
  std::string upsell_text{};
};

/// @brief One page of a category grid. Plain data.
struct GridPage {
  /// @brief The category's `pageInfo.totalCount`; 0 when absent.
  int total_count{};
  /// @brief The readable products, in reply order.
  std::vector<GridProduct> products;
};

/// @brief A concept's media and its default product's. Plain data.
struct ProductMedia {
  /// @brief The default product's id; empty when there is none.
  std::string product_id;
  /// @brief The concept id asked for.
  std::string concept_id;
  /// @brief The default product's media.
  std::vector<StoreMedia> of_product;
  /// @brief The concept's media.
  std::vector<StoreMedia> of_concept;
};

/// @brief A concept's star rating from players. Plain data.
struct StarRating {
  /// @brief The most stars a rating can give.
  static constexpr std::size_t most_stars = 5;

  /// @brief The `averageRating`, 0 to 5.
  double average{};
  /// @brief The `averageRatingForDisplay`, as shown.
  std::string average_text;
  /// @brief How many players rated, at least 0.
  std::int64_t ratings{};
  /// @brief The share of ratings giving one to five stars, in percent clamped
  /// to 0 to 100; index 0 is one star; 0 when not stated.
  std::array<int, most_stars> percent_by_stars{};
};

/// @brief A concept's publisher, release date and genres. Plain data.
struct ProductInfo {
  /// @brief The publisher; the default product's when the concept has none.
  std::string publisher;
  /// @brief The release date as PS wrote it; the default product's as a
  /// fallback.
  std::string release_date;
  /// @brief The localised genres; the default product's as a fallback.
  std::vector<std::string> genres;
};

/// @brief Sends the anonymous store GraphQL operations with the
/// LegacyCloudStream user agent and the given store locale.
///
/// Every reply field is tolerated when malformed. Concept calls retry once a
/// second later; the grid call allows 45 s. Holds references to the HTTP
/// client and the statistics, which must outlive it. Not for concurrent use.
class PublicWebClient {
 public:
  /// @brief Prepares the calls.
  ///
  /// @param client The client the requests go through.
  /// @param stats Where calls are recorded; may be null.
  explicit PublicWebClient(net::http::Client& client,
                           Stats* stats = nullptr) noexcept;

  /// @brief Reads one page of a store category, full games only:
  /// `categoryGridRetrieve` with `{"id","pageArgs":{"size","offset"},
  /// "sortBy":null,"filterBy":["storeDisplayClassification:FULL_GAME"],
  /// "facetOptions":[]}`.
  ///
  /// @param category_id The category id.
  /// @param offset Index of the first product.
  /// @param size How many products to ask for.
  /// @param locale The store locale.
  /// @return The page.
  /// @retval GraphqlError::Kind::malformed The reply has no
  ///     `categoryGridRetrieve`.
  /// @retval GraphqlError The failures of Sender::send_public().
  [[nodiscard]] auto store_category_grid(std::string_view category_id,
                                         int offset, int size,
                                         std::string_view locale)
      -> Expected<GridPage, GraphqlError>;
  /// @brief Reads a concept's media: `conceptRetrieveForMedia` with
  /// `{"conceptId"}`.
  ///
  /// @param concept_id The concept id.
  /// @param locale The store locale.
  /// @return The concept's and its default product's media.
  /// @retval GraphqlError The failures of concept_named().
  [[nodiscard]] auto concept_media(std::string_view concept_id,
                                   std::string_view locale)
      -> Expected<ProductMedia, GraphqlError>;
  /// @brief Reads a concept's star rating: `wcaConceptStarRatingRetrive`.
  ///
  /// @param concept_id The concept id.
  /// @param locale The store locale.
  /// @return The default product's rating; nothing when it has none, or no
  ///     average or count.
  /// @retval GraphqlError The failures of concept_named().
  [[nodiscard]] auto concept_star_rating(std::string_view concept_id,
                                         std::string_view locale)
      -> Expected<std::optional<StarRating>, GraphqlError>;
  /// @brief Reads a concept's age rating: `conceptRetrieveForContentRating`.
  ///
  /// @param concept_id The concept id.
  /// @param locale The store locale.
  /// @return The concept's rating, else its default product's; nothing when
  ///     neither has a name or description.
  /// @retval GraphqlError The failures of concept_named().
  [[nodiscard]] auto concept_content_rating(std::string_view concept_id,
                                            std::string_view locale)
      -> Expected<std::optional<ContentRating>, GraphqlError>;
  /// @brief Reads a concept's publisher, release date and genres:
  /// `conceptRetrieveForGameInfo`.
  ///
  /// @param concept_id The concept id.
  /// @param locale The store locale.
  /// @return The information, each part falling back to the default
  ///     product's.
  /// @retval GraphqlError The failures of concept_named().
  [[nodiscard]] auto concept_info(std::string_view concept_id,
                                  std::string_view locale)
      -> Expected<ProductInfo, GraphqlError>;

 private:
  /// @brief The grid's `pageArgs`.
  struct PageArgs {
    /// @brief How many products.
    std::int64_t size{};
    /// @brief Index of the first product.
    std::int64_t offset{};
  };
  /// @brief The grid's variables.
  struct GridAsked {
    /// @brief The category id.
    std::string_view id;
    /// @brief The page.
    PageArgs page_args;
    /// @brief `sortBy`, always null.
    std::nullptr_t sort_by{};
    /// @brief `filterBy`: full games only.
    std::array<std::string_view, 1> filter_by{
        "storeDisplayClassification:FULL_GAME"};
    /// @brief `facetOptions`, always empty.
    std::vector<std::string_view> facet_options{};

    /// @brief Maps the fields to their camel-case keys, nulls included.
    struct glaze {
      /// @brief The mapped type.
      using T = GridAsked;
      /// @brief The key mapping.
      static constexpr auto value = glz::object(
          "id", &T::id, "pageArgs", &T::page_args, "sortBy", &T::sort_by,
          "filterBy", &T::filter_by, "facetOptions", &T::facet_options);
    };
  };
  /// @brief The concept operations' variables.
  struct ConceptAsked {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The concept id.
    std::string_view concept_id;
  };
  /// @brief A GraphQL failure's kind.
  using Kind = GraphqlError::Kind;
  /// @brief The PlayStation Plus service branding.
  static constexpr std::string_view ps_plus = "PS_PLUS";
  /// @brief How long a grid request may take to connect and in all.
  static constexpr std::chrono::milliseconds grid_timeout{45'000};

  /// @brief The upsell texts that mean "included with PlayStation Plus".
  static constexpr std::array included_upsell_texts{
      std::string_view{"Extra"}, std::string_view{"Essential"},
      std::string_view{"Included"}};

  /// @brief The grid request.
  static constexpr net::http::Endpoint grid_endpoint{
      .label = public_web::category_grid_retrieve.label,
      .connect_timeout = grid_timeout,
      .total_timeout = grid_timeout,
  };

  /// @brief One retry, a second after a failed attempt.
  static constexpr net::http::Retry once_more{
      .retries = 1, .delay = std::chrono::milliseconds{1000}};
  /// @brief The media request.
  static constexpr net::http::Endpoint concept_media_endpoint{
      .label = public_web::concept_media.label,
      .retry = once_more,
  };
  /// @brief The star rating request.
  static constexpr net::http::Endpoint concept_star_rating_endpoint{
      .label = public_web::concept_star_rating.label,
      .retry = once_more,
  };
  /// @brief The age rating request.
  static constexpr net::http::Endpoint concept_content_rating_endpoint{
      .label = public_web::concept_content_rating.label,
      .retry = once_more,
  };
  /// @brief The game information request.
  static constexpr net::http::Endpoint concept_info_endpoint{
      .label = public_web::concept_game_info.label,
      .retry = once_more,
  };

  /// @brief A field tolerated when malformed.
  ///
  /// @tparam Value The field's type.
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;
  /// @brief A list whose entries and itself are tolerated when malformed.
  ///
  /// @tparam Value An entry's type.
  template <class Value>
  using TolerantList = Tolerant<std::vector<Tolerant<Value>>>;

  /// @brief A grid product's `price`.
  struct GridPrice {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `serviceBranding`.
    ReplyStrings service_branding;
    /// @brief `upsellServiceBranding`.
    ReplyStrings upsell_service_branding;
    /// @brief `upsellText`.
    Tolerant<std::string> upsell_text;
  };
  /// @brief A grid product as sent.
  struct GridEntry {
    /// @brief Its `id`.
    Tolerant<std::string> id;
    /// @brief Its `price`.
    Tolerant<GridPrice> price;
  };
  /// @brief The grid's `pageInfo`.
  struct PageInfo {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `totalCount`.
    ReplyInteger<int> total_count;
  };
  /// @brief `categoryGridRetrieve`.
  struct Grid {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `pageInfo`.
    Tolerant<PageInfo> page_info;
    /// @brief Its `products`.
    TolerantList<GridEntry> products;
  };
  /// @brief The grid operation's `data`.
  struct GridData {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `categoryGridRetrieve`.
    Tolerant<Grid> category_grid_retrieve;
  };
  /// @brief One entry of `ratingsDistribution`.
  struct Share {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The star count, 1 to 5.
    ReplyInteger<int> rating;
    /// @brief `percentageRaw`.
    ReplyInteger<int> percentage_raw;
  };
  /// @brief A `starRating`.
  struct Stars {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `averageRating`.
    Tolerant<double> average_rating;
    /// @brief `totalRatingsCount`.
    ReplyInteger<int> total_ratings_count;
    /// @brief `averageRatingForDisplay`.
    Tolerant<std::string> average_rating_for_display;
    /// @brief `ratingsDistribution`.
    TolerantList<Share> ratings_distribution;
  };
  /// @brief A concept's `defaultProduct`.
  struct Edition {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `id`.
    Tolerant<std::string> id;
    /// @brief Its `media`.
    MediaList media;
    /// @brief Its `starRating`.
    Tolerant<Stars> star_rating;
    /// @brief Its `contentRating`.
    Tolerant<ContentRating> content_rating;
    /// @brief Its `publisherName`.
    Tolerant<std::string> publisher_name;
    /// @brief Its `releaseDate`.
    ReleaseDate release_date;
    /// @brief Its `localizedGenres`.
    Genres localized_genres;
  };
  /// @brief A concept as sent.
  struct Concept {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `id`.
    Tolerant<std::string> id;
    /// @brief Its `media`.
    MediaList media;
    /// @brief Its `contentRating`.
    Tolerant<ContentRating> content_rating;
    /// @brief Its `publisherName`.
    Tolerant<std::string> publisher_name;
    /// @brief Its `releaseDate`.
    ReleaseDate release_date;
    /// @brief Its `localizedGenres`.
    Genres localized_genres;
    /// @brief Its `defaultProduct`.
    Tolerant<Edition> default_product;
  };
  /// @brief The concept operations' `data`.
  struct ConceptData {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `conceptRetrieve`.
    Retrieved<Concept> concept_retrieve;
  };

  /// @brief Sends a concept operation and checks it answered for the concept.
  ///
  /// @param operation The operation.
  /// @param endpoint The endpoint.
  /// @param concept_id The concept id, sent as `conceptId`.
  /// @param locale The store locale.
  /// @return The concept.
  /// @retval GraphqlError::Kind::not_found `conceptRetrieve` was absent or
  ///     null.
  /// @retval GraphqlError::Kind::malformed It was unreadable or another
  ///     concept.
  /// @retval GraphqlError The failures of Sender::send_public().
  [[nodiscard]] auto concept_named(const Operation& operation,
                                   const net::http::Endpoint& endpoint,
                                   std::string_view concept_id,
                                   std::string_view locale)
      -> Expected<Concept, GraphqlError>;
  /// @brief Reads a star rating.
  ///
  /// @param rated The rating, moved from.
  /// @return The rating; nothing without an average and a count.
  [[nodiscard]] static auto star_rating_of(Stars& rated)
      -> std::optional<StarRating>;
  /// @brief Keeps an age rating that says something.
  ///
  /// @param rating The rating, moved from.
  /// @return It, or nothing when absent or without a name and description.
  [[nodiscard]] static auto rated(Tolerant<ContentRating>& rating)
      -> std::optional<ContentRating>;
  /// @brief Tells whether a grid price means included with PlayStation Plus.
  ///
  /// @param price The price.
  /// @return As GridProduct::ps_plus describes.
  static auto ps_plus_included(const GridPrice& price) -> bool;
  /// @brief Reads a grid page.
  ///
  /// @param data The reply, moved from.
  /// @return The page; nothing when `categoryGridRetrieve` is missing.
  [[nodiscard]] static auto grid_page(GridData& data)
      -> std::optional<GridPage>;
  /// @brief Makes the failure of a reply that lacked what was asked.
  ///
  /// @param operation The operation's name.
  /// @param result The reply.
  /// @return A malformed failure with its status and attempts.
  static auto unreadable(std::string_view operation, const Result& result)
      -> GraphqlError;

  /// @brief Sends the operations.
  Sender sender_;
};

}  // namespace libchiaking::psn::graphql
