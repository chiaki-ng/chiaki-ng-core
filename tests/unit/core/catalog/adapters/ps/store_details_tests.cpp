#include "src/core/catalog/adapters/ps/store_details.hpp"
#include "src/core/psn/rest/store/store_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::ProductId;
using catalog::StoreLocale;
using catalog::StorePage;
using catalog::Unanswered;
using test_support::psn::Reply;

class StorePagesResolve : public ::testing::Test {
 protected:
  static constexpr std::string_view death_haulage =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";
  static constexpr std::string_view ghost_grant =
      "EP0109-CUSA37002_00-FAKE0050TITLE000";
  static constexpr std::string_view concept_page =
      "https://store.playstation.com/en-gb/concept/900004";
  static constexpr std::string_view product_page =
      "https://store.playstation.com/en-gb/product/"
      "EP0109-PPSA07033_00-FAKE0030TITLE000";

  static auto locale(std::string_view text) -> StoreLocale {
    auto parsed = StoreLocale::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto product(std::string_view id) -> ProductId {
    auto parsed = ProductId::parse(id);
    EXPECT_TRUE(parsed) << id;
    return std::move(parsed).value();
  }

  auto resolve(std::string_view id, std::string_view concept_url = concept_page,
               std::string_view store = "en-GB")
      -> libchiaking::Expected<StorePage, Unanswered> {
    return catalog::ps::StorePages{http}.page_of(
        product(id), locale(store),
        StorePage::at(concept_url, StorePage::Source::concept_page));
  }

  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  libchiaking::platform::StopToken stop{state};
  test_support::psn::FakeHttp fake;
  libchiaking::platform::SteadyClock steady;
  libchiaking::net::http::Client http{fake, steady, stop};
};

TEST_F(StorePagesResolve, PrefersAnExistingProductPage) {
  fake.on("HEAD", std::string{product_page}, {Reply{200, ""}});
  const auto page = resolve(death_haulage);
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().source(), StorePage::Source::product);
  EXPECT_EQ(page.value().url(), product_page);
  ASSERT_EQ(fake.seen.size(), 1U);
  EXPECT_EQ(fake.seen.front().method, "HEAD");
  EXPECT_EQ(fake.seen.front().total_timeout,
            libchiaking::psn::rest::store_product_page_total_timeout);
  EXPECT_EQ(fake.seen.front().connect_timeout,
            libchiaking::psn::rest::store_product_page_connect_timeout);
  EXPECT_TRUE(fake.unmatched.empty());
}

TEST_F(StorePagesResolve, FallsBackToTheConceptForAMissingProductPage) {
  const std::string missing =
      "https://store.playstation.com/en-gb/product/" + std::string{ghost_grant};
  Reply redirect{302, ""};
  redirect.headers.push_back(
      {"Location", "https://store.playstation.com/en-gb/error?productId=" +
                       std::string{ghost_grant} +
                       "&widgetErrorType=emptyRequiredBatarang"});
  fake.on("HEAD", missing, {redirect});
  const auto page = resolve(
      ghost_grant, "https://store.playstation.com/en-gb/concept/90000017");
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().source(), StorePage::Source::concept_page);
  EXPECT_EQ(page.value().url(),
            "https://store.playstation.com/en-gb/concept/90000017");
  EXPECT_EQ(fake.count("/product/"), 1);
}

TEST_F(StorePagesResolve, FallsBackToTheConceptWhenTheCheckFails) {
  fake.on("HEAD", std::string{product_page},
          {Reply::transport(), Reply{404, ""}, Reply{500, ""}});
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto page = resolve(death_haulage);
    ASSERT_TRUE(page) << attempt;
    EXPECT_EQ(page.value().source(), StorePage::Source::concept_page)
        << attempt;
    EXPECT_EQ(page.value().url(), concept_page) << attempt;
  }
}

TEST_F(StorePagesResolve, SkipsTheRequestForAnUnusableId) {
  const auto page = resolve("EP-CUSA7_00-UPGRADE");
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().source(), StorePage::Source::concept_page);
  EXPECT_EQ(page.value().url(), concept_page);
  EXPECT_TRUE(fake.seen.empty());
}

TEST_F(StorePagesResolve, IsNotListedWithoutAStoreConceptPage) {
  const auto unshaped =
      resolve("EP-CUSA7_00-UPGRADE", "https://evil.example/concept/1");
  ASSERT_FALSE(unshaped);
  EXPECT_EQ(unshaped.error().reason(), Unanswered::Reason::not_listed);
  EXPECT_TRUE(fake.seen.empty());
  fake.on("HEAD", std::string{product_page}, {Reply{302, ""}});
  const auto nowhere = resolve(death_haulage, "");
  ASSERT_FALSE(nowhere);
  EXPECT_EQ(nowhere.error().reason(), Unanswered::Reason::not_listed);
  EXPECT_EQ(fake.seen.size(), 1U);
}

TEST_F(StorePagesResolve, StopsBeforeTheRequestWhenCancelled) {
  state->stopped = true;
  const auto page = resolve(death_haulage);
  EXPECT_TRUE(http.stop_requested());
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().source(), StorePage::Source::concept_page);
  EXPECT_TRUE(fake.seen.empty());
  const auto nowhere = resolve(death_haulage, "");
  ASSERT_FALSE(nowhere);
  EXPECT_TRUE(nowhere.error().is_cancelled());
  EXPECT_TRUE(fake.seen.empty());
}

}  // namespace
}  // namespace catalog_test
