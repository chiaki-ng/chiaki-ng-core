#pragma once

#include "src/core/catalog/adapters/ps/streaming_category.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"

#include <libchiaking/detail/catalog/title_number.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {

class CategoryPages final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    urls.emplace_back(request.url.view());
    events.push_back("perform " + offset_of(request));
    for (const auto& header : request.headers) headers.insert(header.name);
    for (const auto& [offset, body] : pages) {
      if (request.url.view().find("&offset=" + std::to_string(offset) + "&") ==
          std::string_view::npos)
        continue;
      libchiaking::platform::HttpResponse response;
      response.status_code = body.empty() ? 503 : 200;
      response.body.assign(std::string_view{body});
      return response;
    }
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  auto start(const libchiaking::platform::HttpRequest& request,
             const libchiaking::platform::StopToken& stop)
      -> libchiaking::platform::PendingHttpResponse override {
    events.push_back("start " + offset_of(request));
    return libchiaking::platform::HttpProvider::start(request, stop);
  }

  static auto offset_of(const libchiaking::platform::HttpRequest& request)
      -> std::string {
    const auto url = request.url.view();
    const auto begin = url.find("&offset=") + 8;
    return std::string{url.substr(begin, url.find('&', begin) - begin)};
  }

  std::map<std::size_t, std::string> pages;
  std::vector<std::string> events;
  std::vector<std::string> urls;
  std::set<std::string> headers;
};

class CategoryStore {
 public:
  using PremiumCategory = libchiaking::catalog::cloud_stream::PremiumCategory;
  using ForSaleCategory = libchiaking::catalog::cloud_stream::ForSaleCategory;
  using PremiumProduct = libchiaking::catalog::cloud_stream::PremiumProduct;
  using Unanswered = libchiaking::catalog::Unanswered;
  using OwnershipId = libchiaking::catalog::OwnershipId;

  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;

  CategoryStore() {
    wall.now_us = static_cast<std::uint64_t>(wall_start_ms) * 1000U;
  }

  static auto entitlement(std::string_view id, std::string_view feature,
                          std::string_view streams = "true",
                          std::string_view package = "PSGD") -> std::string {
    return R"({"id":")" + std::string{id} + R"(","packageType":")" +
           std::string{package} + R"(","featureType":")" +
           std::string{feature} + "\"" +
           (streams.empty()
                ? std::string{}
                : R"(,"streamingSupported":)" + std::string{streams}) +
           "}";
  }

  static auto joined(const std::vector<std::string>& items) -> std::string {
    std::string text;
    for (const auto& item : items) text += (text.empty() ? "" : ",") + item;
    return text;
  }

  static auto sku(std::string_view id, std::string_view branding,
                  const std::vector<std::string>& entitlements) -> std::string {
    return R"({"id":")" + std::string{id} +
           R"(","contentCollections":[{"branding":")" + std::string{branding} +
           R"("}],"entitlements":[)" + joined(entitlements) + "]}";
  }

  static auto unbranded(std::string_view id,
                        const std::vector<std::string>& entitlements)
      -> std::string {
    return R"({"id":")" + std::string{id} + R"(","entitlements":[)" +
           joined(entitlements) + "]}";
  }

  static auto product(std::string_view id, const std::vector<std::string>& skus)
      -> std::string {
    return R"({"id":")" + std::string{id} + R"(","skus":[)" + joined(skus) +
           "]}";
  }

  static auto full_game(int number) -> std::string {
    const auto title = "EP1-PPSA" + std::to_string(10000 + number) + "_00-";
    return product(title + "GAME",
                   {sku(title + "SKU", "PS_PLUS_STREAM_PS5",
                        {entitlement(title + "GAME", "FULL_GAME")})});
  }

  static auto page(std::size_t total, const std::vector<std::string>& products)
      -> std::string {
    return R"({"totalNumProducts":)" + std::to_string(total) +
           R"(,"products":[)" + joined(products) + "]}";
  }

  static auto games(std::size_t total, int first, int count) -> std::string {
    std::vector<std::string> listed;
    for (int number = first; number < first + count; ++number)
      listed.push_back(full_game(number));
    return page(total, listed);
  }

  static auto id(std::string_view text) -> OwnershipId {
    auto parsed = OwnershipId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto included_in(const PremiumCategory& category)
      -> std::set<std::string> {
    std::set<std::string> titles;
    for (const auto& product : category.products())
      if (product.included_offer())
        if (const auto number = product.id().title_number())
          titles.insert(number->text());
    return titles;
  }

  static auto included_entitlement(const PremiumProduct& product)
      -> std::string {
    const auto offer = product.included_offer();
    return offer && offer->entitlement.id() ? offer->entitlement.id()->text()
                                            : std::string{};
  }

  static auto offered(const std::string& listed) -> PremiumProduct {
    CategoryStore store;
    return store.only_product(listed);
  }

  static auto en_gb() -> libchiaking::catalog::StoreLocale {
    auto locale = libchiaking::catalog::StoreLocale::parse("en-GB");
    EXPECT_TRUE(locale);
    return std::move(locale).value();
  }

  auto fetch(int age = 99)
      -> libchiaking::Expected<PremiumCategory, Unanswered> {
    return libchiaking::catalog::ps::StreamingCategory{client, wall}.included(
        en_gb(), age);
  }

  auto for_sale(int age = 99)
      -> libchiaking::Expected<ForSaleCategory, Unanswered> {
    return libchiaking::catalog::ps::StreamingCategory{client, wall}.for_sale(
        en_gb(), age);
  }

  auto only_product(const std::string& listed) -> PremiumProduct {
    http.pages[0] = page(1, {listed});
    const auto category = fetch();
    EXPECT_TRUE(category);
    if (!category) throw std::logic_error{"the category was not read"};
    EXPECT_EQ(category.value().products().size(), 1U);
    if (category.value().products().empty())
      throw std::logic_error{"the category holds no product"};
    return category.value().products().front();
  }

  CategoryPages http;
  test_support::FakeClock wall;
  test_support::FakeClock steady;
  std::shared_ptr<test_support::StopState> stop =
      std::make_shared<test_support::StopState>();
  libchiaking::net::http::Client client{http, steady,
                                        libchiaking::platform::StopToken{stop}};
};

}  // namespace catalog_test
