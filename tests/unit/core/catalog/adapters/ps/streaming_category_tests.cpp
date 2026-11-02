#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/catalog/adapters/ps/streaming_category_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace catalog_test {
namespace {

namespace ps = libchiaking::catalog::ps;
using libchiaking::catalog::cloud_stream::PremiumEntitlement;
using Feature = PremiumEntitlement::Feature;
using Scan = libchiaking::catalog::cloud_stream::PremiumCategory::Scan;

class StreamingCategoryScan : public ::testing::Test,
                              protected CategoryStore {};

TEST_F(StreamingCategoryScan, ReadsPagesByTheServersOwnOffset) {
  http.pages[0] = page(3, {full_game(1), full_game(2)});
  http.pages[2] = page(3, {full_game(3)});
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_TRUE(offers.value().is_complete());
  EXPECT_EQ(offers.value().scan(), (Scan{3, 3, true}));
  EXPECT_EQ(offers.value().products().size(), 3U);
  EXPECT_EQ(offers.value().fetched_at().time_since_epoch().count(),
            wall_start_ms);
  EXPECT_EQ(included_in(offers.value()),
            (std::set<std::string>{"PPSA10001", "PPSA10002", "PPSA10003"}));
  EXPECT_EQ(http.urls.size(), 2U);
}

TEST_F(StreamingCategoryScan, AsksForTheIncludedCategoryAnonymously) {
  http.pages[0] = page(3, {full_game(1), full_game(2)});
  http.pages[2] = page(3, {full_game(3)});
  EXPECT_TRUE(fetch());
  ASSERT_FALSE(http.urls.empty());
  EXPECT_EQ(
      http.urls[0].find(
          "https://m.np.playstation.com/api/catalog/v2/categories/"
          "a23e4f41-6e78-4dab-95e2-af1c10e4ab96?country=GB&language=en&age="
          "99&limit=50&offset=0&"),
      0U);
  EXPECT_NE(http.urls[0].find("includeConcept=true"), std::string::npos);
  EXPECT_EQ(http.urls[0].find("benefitPlanIds"), std::string::npos);
  EXPECT_FALSE(http.headers.contains("Authorization"));
  EXPECT_FALSE(http.headers.contains("Cookie"));
}

TEST_F(StreamingCategoryScan, StartsTheFullPagesAfterTheFirstTogether) {
  http.pages[0] = games(150, 1, 50);
  http.pages[50] = games(150, 51, 50);
  http.pages[100] = games(150, 101, 50);
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_TRUE(offers.value().is_complete());
  EXPECT_EQ(offers.value().scan(), (Scan{150, 150, true}));
  EXPECT_EQ(offers.value().products().size(), 150U);
  EXPECT_EQ(http.events, (std::vector<std::string>{
                             "start 0", "perform 0", "start 50", "start 100",
                             "perform 50", "perform 100"}));
}

TEST_F(StreamingCategoryScan,
       DropsPagesStartedAfterAShortPageAndAsksFromItsEnd) {
  http.pages[0] = games(150, 1, 50);
  http.pages[50] = games(150, 51, 49);
  http.pages[99] = games(150, 100, 51);
  http.pages[100] = games(150, 900, 50);
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_TRUE(offers.value().is_complete());
  EXPECT_EQ(offers.value().scan(), (Scan{150, 150, true}));
  EXPECT_EQ(offers.value().products().size(), 150U);
  EXPECT_EQ(http.events,
            (std::vector<std::string>{"start 0", "perform 0", "start 50",
                                      "start 100", "perform 50", "start 99",
                                      "start 149", "perform 99"}));
}

class StreamingCategoryRealProduct : public ::testing::Test,
                                     protected CategoryStore {
 protected:
  void SetUp() override {
    auto skyline = test_support::psn::Fixtures::text(
        "catalog_products_FAKE0056TITLE000.json");
    skyline = skyline.substr(skyline.find('{'));
    skyline = skyline.substr(0, skyline.rfind('}') + 1);
    listed.emplace(only_product(skyline));
  }

  std::optional<PremiumProduct> listed;
};

TEST_F(StreamingCategoryRealProduct, KeepsItsFactsAndMasterImage) {
  const auto& facts = listed->facts();
  EXPECT_EQ(facts.id.text(), "EP0109-PPSA17001_00-FAKE0056TITLE000");
  EXPECT_EQ(facts.name, "Skyline Zero Ember™ Remastered");
  ASSERT_TRUE(facts.concept_id);
  EXPECT_EQ(facts.concept_id->text(), "900002");
  EXPECT_TRUE(
      facts.master_image_url.starts_with("https://image.api.playstation.com/"));
  EXPECT_EQ(facts.master_image_url.find("c0b8df9fd1c06837b44"),
            std::string::npos);
}

TEST_F(StreamingCategoryRealProduct, KeepsEverySkuAndEntitlement) {
  const auto& facts = listed->facts();
  ASSERT_EQ(facts.skus.size(), 2U);
  const auto& first = facts.skus[0];
  ASSERT_TRUE(first.id());
  EXPECT_EQ(first.id()->text(), "EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  EXPECT_TRUE(first.is_premium_stream_branded());
  EXPECT_FALSE(facts.skus[1].is_premium_stream_branded());
  ASSERT_EQ(first.entitlements().size(), 3U);
  ASSERT_EQ(facts.skus[1].entitlements().size(), 2U);
  const auto& tracker = first.entitlements()[1].facts();
  EXPECT_EQ(tracker.id, id("EP0109-PPSA17001_00-FAKE0063TITLE000"));
  EXPECT_EQ(tracker.package_type, "PSTRACK");
  EXPECT_EQ(tracker.feature, Feature::unknown);
  EXPECT_FALSE(tracker.streaming_supported);
  EXPECT_EQ(first.entitlements()[2].facts().streaming_supported, false);
  EXPECT_EQ(facts.skus[1].entitlements()[0].facts().feature,
            Feature::disc_required);
}

TEST_F(StreamingCategoryRealProduct, IncludesTheBrandedSkusGame) {
  const auto& first = listed->facts().skus.at(0);
  EXPECT_EQ(included_entitlement(*listed),
            "EP0109-PPSA17001_00-FAKE0056TITLE000");
  ASSERT_TRUE(listed->included_offer());
  EXPECT_EQ(listed->included_offer()->sku, first.id());
  EXPECT_FALSE(listed->is_bundle());
  const auto children = listed->streamable_children();
  ASSERT_EQ(children.size(), 1U);
  EXPECT_EQ(children[0].sku, first.id());
}

class StreamingCategoryImages : public ::testing::Test,
                                protected CategoryStore {
 protected:
  static auto with_images(std::string_view images) -> std::string {
    return R"({"id":"EP1-PPSA37001_00-GAME","name":"Game","media":{"images":[)" +
           std::string{images} + R"(]},"skus":[]})";
  }
};

TEST_F(StreamingCategoryImages, TakeTheFirstImageTypedMaster) {
  EXPECT_EQ(
      offered(
          with_images(
              R"({"type":"EDITION_KEY_ART","url":"https://key"},)"
              R"({"type":"MASTER","url":"https://master"},{"type":"MASTER","url":"https://second"})"))
          .facts()
          .master_image_url,
      "https://master");
}

TEST_F(StreamingCategoryImages, LeaveTheMasterImageEmptyWithoutOne) {
  EXPECT_TRUE(
      offered(
          with_images(R"({"type":"GAMEHUB_COVER_ART","url":"https://cover"})"))
          .facts()
          .master_image_url.empty());
}

TEST_F(StreamingCategoryImages, ReadABareProductWithoutAConcept) {
  const auto bare = offered(
      R"({"id":"EP1-PPSA37002_00-GAME","concept":{"id":"0"},"skus":[]})");
  EXPECT_TRUE(bare.facts().master_image_url.empty());
  EXPECT_FALSE(bare.facts().concept_id);
  EXPECT_TRUE(bare.facts().name.empty());
  const auto conceived = offered(
      R"({"id":"EP1-PPSA37003_00-GAME","concept":{"id":"4501"},"skus":[]})");
  ASSERT_TRUE(conceived.facts().concept_id);
  EXPECT_EQ(conceived.facts().concept_id->text(), "4501");
}

TEST_F(StreamingCategoryScan,
       DropsUnreadableProductsButStillAdvancesTheOffset) {
  http.pages[0] = page(5, {full_game(1), R"({"id":"bad id","skus":[]})",
                           R"({"id":"EP1-PPSA47002_00-NOSKUS"})"});
  http.pages[3] = page(5, {full_game(4), full_game(5)});
  const auto category = fetch();
  ASSERT_TRUE(category);
  EXPECT_TRUE(category.value().is_complete());
  EXPECT_EQ(category.value().scan(), (Scan{5, 5, true}));
  EXPECT_EQ(category.value().products().size(), 3U);
  EXPECT_EQ(category.value().included_count(), 3U);
  ASSERT_EQ(http.urls.size(), 2U);
  EXPECT_NE(http.urls[1].find("&offset=3&"), std::string::npos);
  EXPECT_EQ(included_in(category.value()),
            (std::set<std::string>{"PPSA10001", "PPSA10004", "PPSA10005"}));
}

TEST_F(StreamingCategoryScan, IsPartialWhenALaterPageKeepsFailing) {
  http.pages[0] = page(3, {full_game(1), full_game(2)});
  http.pages[2] = "";
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_FALSE(offers.value().is_complete());
  EXPECT_EQ(offers.value().scan(), (Scan{3, 2, false}));
  EXPECT_EQ(offers.value().products().size(), 2U);
  EXPECT_EQ(
      http.urls.size(),
      1 + static_cast<std::size_t>(
              libchiaking::catalog::RetryPolicy::category_page().attempts()));
}

TEST_F(StreamingCategoryScan, IsPartialWhenALaterPageRepeatsProducts) {
  http.pages[0] = page(4, {full_game(1), full_game(2)});
  http.pages[2] = page(4, {full_game(1), full_game(2)});
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_FALSE(offers.value().is_complete());
  EXPECT_EQ(offers.value().products().size(), 2U);
  EXPECT_EQ(http.urls.size(), 2U);
}

TEST_F(StreamingCategoryScan, IsPartialAtThePageLimit) {
  for (std::size_t offset = 0; offset < ps::StreamingCategory::page_limit + 5;
       ++offset)
    http.pages[offset] = page(1000, {full_game(static_cast<int>(offset) + 1)});
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_FALSE(offers.value().is_complete());
  EXPECT_EQ(http.urls.size(), ps::StreamingCategory::page_limit);
}

TEST_F(StreamingCategoryScan, EndsAtAnEmptyPage) {
  http.pages[0] = page(5, {full_game(1), full_game(2)});
  http.pages[2] = page(5, {});
  const auto offers = fetch();
  ASSERT_TRUE(offers);
  EXPECT_TRUE(offers.value().is_complete());
  EXPECT_EQ(offers.value().scan(), (Scan{5, 2, true}));
}

TEST_F(StreamingCategoryScan, IsUnavailableWhenTheFirstPageCannotBeFetched) {
  const auto offers = fetch();
  ASSERT_FALSE(offers);
  EXPECT_EQ(offers.error().reason(), Unanswered::Reason::unavailable);
}

TEST(StreamingCategoryFirstPage, IsMalformedWhenItCannotBeRead) {
  for (auto body : {R"({"products":[]})", R"({"totalNumProducts":3})", R"([])",
                    R"({"totalNumProducts":-1,"products":[]})"}) {
    CategoryStore store;
    store.http.pages[0] = body;
    const auto offers = store.fetch();
    ASSERT_FALSE(offers) << body;
    EXPECT_EQ(offers.error().reason(),
              CategoryStore::Unanswered::Reason::malformed)
        << body;
  }
}

TEST_F(StreamingCategoryScan, IsCancelledBeforeAnyRequest) {
  stop->stopped = true;
  const auto offers = fetch();
  ASSERT_FALSE(offers);
  EXPECT_TRUE(offers.error().is_cancelled());
  EXPECT_TRUE(http.urls.empty());
}

TEST_F(StreamingCategoryScan, AsksForTheAccountsAge) {
  http.pages[0] = page(1, {full_game(1)});
  EXPECT_TRUE(fetch(12));
  ASSERT_FALSE(http.urls.empty());
  EXPECT_NE(http.urls.back().find(
                "?country=GB&language=en&age=12&limit=50&offset=0&"),
            std::string::npos);
}

TEST_F(StreamingCategoryScan, ReadsTheStoreCategoryAnonymouslyAHundredAtATime) {
  http.pages[0] = page(3, {full_game(1), full_game(2)});
  http.pages[2] = page(3, {full_game(3)});
  const auto sold = for_sale(34);
  ASSERT_TRUE(sold);
  EXPECT_TRUE(sold.value().is_complete());
  EXPECT_EQ(sold.value().scan(), (Scan{3, 3, true}));
  EXPECT_EQ(sold.value().for_sale_count(), 3U);
  EXPECT_TRUE(sold.value().is_plausible());
  ASSERT_EQ(http.urls.size(), 2U);
  EXPECT_EQ(
      http.urls[0].find(
          "https://m.np.playstation.com/api/catalog/v2/categories/"
          "83999501-dbe5-413a-b265-6484e0a7626e?country=GB&language=en&age="
          "34&limit=100&offset=0&"),
      0U);
  EXPECT_NE(http.urls[0].find("includeConcept=true"), std::string::npos);
  EXPECT_NE(http.urls[1].find("&limit=100&offset=2&"), std::string::npos);
  EXPECT_FALSE(http.headers.contains("Authorization"));
  EXPECT_FALSE(http.headers.contains("Cookie"));
}

TEST_F(StreamingCategoryScan,
       CallsAStoreCategoryWithoutAGameForSaleImplausible) {
  http.pages[0] = page(
      1,
      {product("EP1-PPSA27003_00-GAME",
               {unbranded("EP1-PPSA27003_00-SKU",
                          {entitlement("EP1-PPSA27003_00-GAME", "TRIAL")})})});
  const auto sold = for_sale();
  ASSERT_TRUE(sold);
  EXPECT_TRUE(sold.value().is_complete());
  EXPECT_EQ(sold.value().for_sale_count(), 0U);
  EXPECT_FALSE(sold.value().is_plausible());
}

}  // namespace
}  // namespace catalog_test
