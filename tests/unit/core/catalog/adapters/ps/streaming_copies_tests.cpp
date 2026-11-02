#include "src/core/catalog/adapters/ps/streaming_copies.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::ProductId;
using libchiaking::catalog::StoreLocale;

class CopyStore final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    urls.emplace_back(request.url.view());
    for (const auto& header : request.headers) headers.insert(header.name);
    if (urls.size() > replies.size())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::disconnected);
    libchiaking::platform::HttpResponse response;
    const auto& body = replies[urls.size() - 1];
    response.status_code = body.empty() ? 503 : 200;
    response.body.assign(std::string_view{body});
    return response;
  }

  auto ask(const std::vector<ProductId>& products, int age = 34)
      -> libchiaking::catalog::ps::StreamingCopies::Answer {
    const libchiaking::platform::SteadyClock steady;
    libchiaking::net::http::Client client{
        *this, steady,
        libchiaking::platform::StopToken{
            std::make_shared<test_support::StopState>()}};
    return libchiaking::catalog::ps::StreamingCopies{client}.of(
        products, *StoreLocale::parse("en-GB"), age);
  }

  std::vector<std::string> replies;
  std::vector<std::string> urls;
  std::set<std::string> headers;
};

class StreamingCopies : public ::testing::Test {
 protected:
  static constexpr std::string_view ronin =
      "EP0109-CUSA07018_00-FAKE0074TITLE000";
  static constexpr std::string_view far_cry =
      "EP0100-CUSA07016_00-FAKE0038TITLE000";
  static constexpr std::string_view carto =
      "EP0105-CUSA27001_00-FAKE0018TITLE000";
  static constexpr std::string_view unsold =
      "EP0100-CUSA00001_00-FAKE0135TITLE000";
  static constexpr std::string_view absent =
      "EP0100-CUSA00002_00-FAKE0001TITLE000";

  static auto sku(std::string_view package,
                  const std::vector<std::string_view>& brandings)
      -> std::string {
    std::string collections;
    for (const auto branding : brandings)
      collections += (collections.empty() ? "" : ",") +
                     (R"({"branding":")" + std::string{branding} + "\"}");
    return R"({"contentCollections":[)" + collections +
           R"(],"entitlements":[{"packageType":")" + std::string{package} +
           R"("}]})";
  }

  static auto listed(std::string_view id, const std::vector<std::string>& skus)
      -> std::string {
    std::string joined;
    for (const auto& item : skus) joined += (joined.empty() ? "" : ",") + item;
    return R"({"id":")" + std::string{id} + R"(","skus":[)" + joined + "]}";
  }

  static auto product(std::string_view id) -> ProductId {
    auto parsed = ProductId::parse(id);
    EXPECT_TRUE(parsed) << id;
    return std::move(parsed).value();
  }

  static auto numbered(int number) -> std::string {
    return "EP0100-CUSA" + std::to_string(10000 + number) +
           "_00-GAME000000000000";
  }

  auto ask_five() -> libchiaking::catalog::ps::StreamingCopies::Answer {
    store.replies = {
        "[" +
        listed(ronin, {sku("PS4GD", {"PS_PLUS_DOWNLOAD"}),
                       sku("PS4GS", {"PS_NOW", "PS_PLUS_STREAM"})}) +
        "," +
        listed(far_cry, {sku("PS4GD", {"UBISOFT_PLUS", "PS_PLUS_DOWNLOAD"})}) +
        "," + listed(carto, {sku("PS4GD", {})}) + "," +
        listed(unsold, {sku("PS4GS", {}), sku("PS4GD", {"PS_PLUS_STREAM"})}) +
        R"(,{"id":"not a product","skus":[]},{"id":")" + std::string{absent} +
        "\"}]"};
    return store.ask({product(ronin), product(far_cry), product(carto),
                      product(unsold), product(absent)});
  }

  CopyStore store;
};

TEST_F(StreamingCopies, ACopyUnderAStreamingPlanDecidesEachTitle) {
  const auto answer = ask_five();
  EXPECT_EQ(answer.asked, 5U);
  EXPECT_EQ(answer.answered(), 4U);
  EXPECT_EQ(answer.covered, std::set<ProductId>{product(ronin)});
  EXPECT_EQ(
      answer.without_a_covered_copy,
      (std::set<ProductId>{product(far_cry), product(carto), product(unsold)}));
}

TEST_F(StreamingCopies, AreAskedOfThePublicCatalogWithoutCredentials) {
  static_cast<void>(ask_five());
  ASSERT_EQ(store.urls.size(), 1U);
  EXPECT_EQ(store.urls[0].find("https://m.np.playstation.com/api/catalog/v2/"
                               "products?productIds="),
            0U);
  EXPECT_NE(store.urls[0].find("&country=GB&language=en&age=34"),
            std::string::npos);
  EXPECT_FALSE(store.headers.contains("Authorization"));
  EXPECT_FALSE(store.headers.contains("Cookie"));
}

TEST_F(StreamingCopies, AreAskedInBatchesWhereAFailedBatchDecidesNothing) {
  std::vector<ProductId> products;
  std::string first = "[";
  for (int number = 0; number < 120; ++number) {
    products.push_back(product(numbered(number)));
    if (number < 50)
      first += (number == 0 ? "" : ",") +
               listed(numbered(number), {sku("PS4GD", {})});
  }
  store.replies = {
      first + "]", "",
      "[" + listed(numbered(100), {sku("PS4GS", {"PS_PLUS_STREAM"})}) + "]"};
  const auto answer = store.ask(products);
  EXPECT_EQ(store.urls.size(), 3U);
  EXPECT_EQ(answer.asked, 120U);
  EXPECT_EQ(answer.without_a_covered_copy.size(), 50U);
  EXPECT_EQ(answer.covered, std::set<ProductId>{product(numbered(100))});
  EXPECT_FALSE(answer.without_a_covered_copy.contains(product(numbered(60))));
}

TEST_F(StreamingCopies, DecideNothingFromAMalformedReply) {
  store.replies = {R"({"products":[]})"};
  EXPECT_EQ(store.ask({product(ronin)}).answered(), 0U);
}

TEST_F(StreamingCopies, AskNothingForNoTitles) {
  EXPECT_EQ(store.ask({}).asked, 0U);
  EXPECT_TRUE(store.urls.empty());
}

}  // namespace
}  // namespace catalog_test
