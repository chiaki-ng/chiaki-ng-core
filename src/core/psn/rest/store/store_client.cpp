#include "src/core/psn/rest/store/store_client.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"

#include <format>
#include <string>
#include <utility>

namespace libchiaking::psn::rest {

StoreClient::StoreClient(net::http::Client client) noexcept
    : client_(std::move(client)) {}

auto StoreClient::product_page_exists(std::string_view product_url)
    -> Expected<bool, StoreError> {
  auto sent =
      client_.send(net::http::Request{product_page_endpoint, product_url});
  if (!sent) return unexpected(StoreError{sent.error()});
  return net::HttpStatus{sent.value().status()} == net::HttpStatus::ok;
}

auto StoreClient::games_lists_request(std::string_view locale,
                                      std::string_view category_list)
    -> net::http::Request {
  net::http::Query url{games_list_url};
  url.add("locale", locale).add("categoryList", category_list);
  net::http::Request request{games_lists_endpoint, std::move(url)};
  request.header("Content-Type", "application/json")
      .header("Accept", "application/json")
      .header("User-Agent", kamaji_user_agent);
  return request;
}

auto StoreClient::streaming_category_request(const StreamingCategoryPage& page)
    -> net::http::Request {
  net::http::Query url{std::format("{}{}", categories_url, page.category)};
  url.add("country", page.country)
      .add("language", page.language)
      .add("age", std::to_string(page.age))
      .add("limit", std::to_string(page.limit))
      .add("offset", std::to_string(page.offset))
      .add("sortField", "contentCollections.contentCollectionStartDate")
      .add("sortDirection", "desc")
      .add("includeConcept", "true");
  net::http::Request request{streaming_category_endpoint, std::move(url)};
  request.header("Accept", "application/json");
  return request;
}

auto StoreClient::products_request(const ProductsAsked& asked)
    -> net::http::Request {
  net::http::Query url{products_url};
  url.add("productIds", asked.product_ids)
      .add("country", asked.country)
      .add("language", asked.language)
      .add("age", std::to_string(asked.age));
  if (asked.with_concept) url.add("includeConcept", "true");
  net::http::Request request{products_endpoint, std::move(url)};
  request.header("Accept", "application/json");
  return request;
}

}  // namespace libchiaking::psn::rest
