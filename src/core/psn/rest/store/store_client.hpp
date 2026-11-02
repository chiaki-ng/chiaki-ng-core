/// @file
/// @brief Declares StoreClient, the anonymous PlayStation Store requests the
/// catalogue is built from: product pages, the PS5 games lists, the cloud
/// streaming shelves and product details.
///
/// None of these requests carry a credential; they read the public store as a
/// browser does, so the catalogue can describe games before anyone signs in.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/rest/store/store_error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::psn::rest {

/// @brief The diagnostic label of the product page check.
inline constexpr std::string_view store_product_page_label =
    "psn.store.product_page_exists";
/// @brief The diagnostic label of the games lists request.
inline constexpr std::string_view store_games_lists_label =
    "psn.store.games_lists";
/// @brief The diagnostic label of the product details request.
inline constexpr std::string_view store_products_label = "psn.store.products";
/// @brief The diagnostic label of the streaming shelf request.
inline constexpr std::string_view store_streaming_category_label =
    "psn.store.streaming_category";
/// @brief The longest a product page check may take (5 s).
inline constexpr std::chrono::milliseconds store_product_page_total_timeout{
    std::chrono::seconds{5}};
/// @brief The longest a product page check may take to connect (3 s).
inline constexpr std::chrono::milliseconds store_product_page_connect_timeout{
    std::chrono::seconds{3}};

/// @brief One page of a cloud streaming store category (shelf). Plain data;
/// the views must outlive the request.
struct StreamingCategoryPage {
  /// @brief The category id, appended to the categories URL.
  std::string_view category;
  /// @brief The store country, such as `gb`.
  std::string_view country;
  /// @brief The store language, such as `en`.
  std::string_view language;
  /// @brief The age the store filters content for, in years.
  int age{};
  /// @brief Index of the first product.
  std::size_t offset{};
  /// @brief How many products to ask for.
  std::size_t limit{};
};

/// @brief A request for products' details. Plain data; the views must outlive
/// the request.
struct ProductsAsked {
  /// @brief The product ids, separated by commas.
  std::string_view product_ids;
  /// @brief The store country.
  std::string_view country;
  /// @brief The store language.
  std::string_view language;
  /// @brief The age the store filters content for, in years.
  int age{};
  /// @brief Whether each product's concept is included.
  bool with_concept{};
};

/// @brief Sends the anonymous store requests.
///
/// Owns a copy of the HTTP client (whose provider must outlive it). Each call
/// blocks for one round trip, except the start calls, which return a Pending
/// to collect later. Not for concurrent use.
class StoreClient {
 public:
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  explicit StoreClient(net::http::Client client) noexcept;

  /// @brief Checks whether a store product page exists.
  ///
  /// HEAD the page, within 3 s to connect and 5 s in all.
  ///
  /// @param product_url The page's URL.
  /// @return True for 200, false for any other status.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto product_page_exists(std::string_view product_url)
      -> Expected<bool, StoreError>;
  /// @brief Reads one of playstation.com's PS5 games lists.
  ///
  /// GET
  /// `https://www.playstation.com/bin/imagic/gameslist?locale=<locale>&categoryList=<list>`
  /// with the browser user agent and JSON headers.
  ///
  /// @tparam Reply The shape the list is decoded into.
  /// @param locale The site locale, such as `en-gb`.
  /// @param category_list The list's name.
  /// @return The decoded list.
  /// @retval StoreError The failures of document().
  template <class Reply>
  [[nodiscard]] auto games_lists(std::string_view locale,
                                 std::string_view category_list)
      -> Expected<Reply, StoreError> {
    return document<Reply>(games_lists_request(locale, category_list));
  }
  /// @brief Reads one page of a cloud streaming store category.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/catalog/v2/categories/<category>?country&language&age&limit&offset&sortField=contentCollections.contentCollectionStartDate&sortDirection=desc&includeConcept=true`.
  ///
  /// @tparam Reply The shape the page is decoded into.
  /// @param page Which page.
  /// @return The decoded page.
  /// @retval StoreError The failures of document().
  template <class Reply>
  [[nodiscard]] auto streaming_category(const StreamingCategoryPage& page)
      -> Expected<Reply, StoreError> {
    return document<Reply>(streaming_category_request(page));
  }
  /// @brief A started request whose reply is decoded when collected.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  template <class Reply>
  class Pending;

  /// @brief Starts asking one page of a cloud streaming store category,
  /// without waiting for it.
  ///
  /// The request is the one streaming_category() sends.
  ///
  /// @tparam Reply The shape the page is decoded into.
  /// @param page Which page.
  /// @return The started request; dropping it uncollected abandons it.
  template <class Reply>
  [[nodiscard]] auto start_streaming_category(const StreamingCategoryPage& page)
      -> Pending<Reply> {
    return Pending<Reply>{client_.start(streaming_category_request(page))};
  }
  /// @brief Reads products' details.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/catalog/v2/products?productIds&country&language&age`,
  /// with `includeConcept=true` when asked.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param asked Which products.
  /// @return The decoded reply.
  /// @retval StoreError The failures of document().
  template <class Reply>
  [[nodiscard]] auto products(const ProductsAsked& asked)
      -> Expected<Reply, StoreError> {
    return document<Reply>(products_request(asked));
  }

 private:
  /// @brief playstation.com's games list endpoint.
  static constexpr std::string_view games_list_url =
      "https://www.playstation.com/bin/imagic/gameslist";

  /// @brief The product page check, with its short timeouts.
  static constexpr net::http::Endpoint product_page_endpoint{
      .label = store_product_page_label,
      .method = net::http::Method::head,
      .connect_timeout = store_product_page_connect_timeout,
      .total_timeout = store_product_page_total_timeout,
  };
  /// @brief The games lists request, logged with its query.
  static constexpr net::http::Endpoint games_lists_endpoint{
      .label = store_games_lists_label, .log_query = true};
  /// @brief The catalogue's products endpoint.
  static constexpr std::string_view products_url =
      "https://m.np.playstation.com/api/catalog/v2/products";
  /// @brief The product details request.
  static constexpr net::http::Endpoint products_endpoint{
      .label = store_products_label};
  /// @brief The catalogue's categories; the category id follows.
  static constexpr std::string_view categories_url =
      "https://m.np.playstation.com/api/catalog/v2/categories/";
  /// @brief The streaming shelf request, logged with its query.
  static constexpr net::http::Endpoint streaming_category_endpoint{
      .label = store_streaming_category_label, .log_query = true};

  /// @brief Builds the games lists request.
  ///
  /// @param locale The site locale.
  /// @param category_list The list's name.
  /// @return The request.
  static auto games_lists_request(std::string_view locale,
                                  std::string_view category_list)
      -> net::http::Request;
  /// @brief Builds the streaming shelf request.
  ///
  /// @param page Which page.
  /// @return The request.
  static auto streaming_category_request(const StreamingCategoryPage& page)
      -> net::http::Request;
  /// @brief Builds the product details request.
  ///
  /// @param asked Which products.
  /// @return The request.
  static auto products_request(const ProductsAsked& asked)
      -> net::http::Request;

  /// @brief Sends a request and decodes a 200 reply.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param request The request.
  /// @return The decoded reply.
  /// @retval StoreError::Reason::failed The request did not complete.
  /// @retval StoreError::Reason::refused The status is not 200.
  /// @retval StoreError::Reason::malformed The reply does not decode.
  template <class Reply>
  [[nodiscard]] auto document(net::http::Request request)
      -> Expected<Reply, StoreError> {
    return decoded<Reply>(client_.send(std::move(request)));
  }
  /// @brief Decodes a 200 reply.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param sent The request's outcome.
  /// @return The decoded reply.
  /// @retval StoreError::Reason::failed The request did not complete.
  /// @retval StoreError::Reason::refused The status is not 200.
  /// @retval StoreError::Reason::malformed The reply does not decode.
  template <class Reply>
  [[nodiscard]] static auto decoded(
      Expected<net::http::Response, net::http::Failure> sent)
      -> Expected<Reply, StoreError> {
    if (!sent) return unexpected(StoreError{sent.error()});
    const auto status = sent.value().status();
    if (net::HttpStatus{status} != net::HttpStatus::ok)
      return unexpected(StoreError{StoreError::Reason::refused, status});
    auto reply = sent.value().document<Reply>();
    if (!reply)
      return unexpected(StoreError{StoreError::Reason::malformed, status});
    return std::move(reply.value());
  }

  /// @brief The client the requests go through.
  net::http::Client client_;
};

/// @brief A request started by a StoreClient start call, collected once with
/// wait().
///
/// Move-only. Dropping it uncollected abandons the request.
///
/// @tparam Reply The shape the reply is decoded into.
template <class Reply>
class StoreClient::Pending {
 public:
  /// @brief Waits for the request and decodes a 200 reply.
  ///
  /// @return The decoded reply.
  /// @retval StoreError The failures of document().
  [[nodiscard]] auto wait() && -> Expected<Reply, StoreError> {
    return decoded<Reply>(std::move(sent_).wait());
  }

 private:
  /// @brief Starts requests.
  friend class StoreClient;

  /// @brief Wraps a started request.
  ///
  /// @param sent The request.
  explicit Pending(net::http::Client::Pending sent) noexcept
      : sent_(std::move(sent)) {}

  /// @brief The started request.
  net::http::Client::Pending sent_;
};

}  // namespace libchiaking::psn::rest
