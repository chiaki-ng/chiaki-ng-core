#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_app_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::StorefrontAnswer;
using libchiaking::catalog::Unanswered;
using Streaming = StorefrontAnswer::Streaming;
using Strings = std::vector<std::string>;

class StoreAppTitleByProduct : public ::testing::Test, public StoreAppRig {
 protected:
  static auto offering(std::string_view action) -> std::string {
    constexpr std::string_view catalog_action = "DOWNLOAD_PS_PLUS_GAME_CATALOG";
    auto body = fixture("product_FAKE0030TITLE000.json");
    for (auto at = body.find(catalog_action); at != std::string::npos;
         at = body.find(catalog_action))
      body.replace(at, catalog_action.size(), action);
    return body;
  }
};

TEST_F(StoreAppTitleByProduct, FindsTheIncludedEdition) {
  replies(by_product, fixture("product_FAKE0030TITLE000.json"));
  const auto answer = asked(standard);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().answered_product(), PsRig::product(standard));
  EXPECT_FALSE(answer.value().owned_per_store());
  EXPECT_EQ(answer.value().streaming(), Streaming::streams);
  const auto& offer = answer.value().library_offer();
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->entitled_through_subscription, false);
  EXPECT_TRUE(answer.value().offers_library_add());
  EXPECT_EQ(offer->sku_id, "EP0109-PPSA07033_00-FAKE0030TITLE000-E002");
  EXPECT_EQ(offer->entitlement_ids,
            Strings{"EP0109-PPSA07033_00-FAKE0030TITLE000"});
}

TEST_F(StoreAppTitleByProduct, AsksOnceWithThePersistedQuery) {
  replies(by_product, fixture("product_FAKE0030TITLE000.json"));
  ASSERT_TRUE(asked(standard));
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& seen = rig.http.seen.front();
  EXPECT_NE(seen.url.find("variables=%7B%22productId%22%3A%22EP0109-PPSA07033_"
                          "00-FAKE0030TITLE000%22%7D"),
            std::string::npos);
  EXPECT_NE(seen.url.find("%22sha256Hash%22%3A%"
                          "22e9ff5930be8bf1dc5c1e6c1c7edc615503fef5b75bf3cd7b3"
                          "f24b34f88ee7141%22"),
            std::string::npos);
}

TEST_F(StoreAppTitleByProduct, ReadsTheConceptOfAnIncludedProduct) {
  replies(by_product, fixture("product_FAKE0030TITLE000.json"));
  auto app = libchiaking::users::CatalogCalls{*user}.store_app(rig.client);
  ASSERT_TRUE(app);
  const auto details = app.value().concept_by_product(standard, {});
  ASSERT_TRUE(details);
  EXPECT_EQ(details.value().concept_id(), "900004");
  EXPECT_TRUE(details.value().priced());
  const auto& streamed = edition(details.value(), standard);
  EXPECT_EQ(streamed.streaming.value, "ENABLED");
  EXPECT_EQ(streamed.streaming.platforms, (Strings{"PS5", "DWC"}));
  const auto& old_edition =
      edition(details.value(), "EP0109-CUSA17007_00-FAKE0028TITLE000");
  EXPECT_TRUE(old_edition.streaming.notices);
  EXPECT_FALSE(old_edition.streaming.supported);
}

TEST_F(StoreAppTitleByProduct, OffersNoTrialWithoutATrialOffer) {
  replies(by_product, fixture("product_FAKE0030TITLE000.json"));
  const auto plain = asked(standard);
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain.value().offers_trial());
}

TEST_F(StoreAppTitleByProduct, OffersATrialWhileTheStoreMakesIt) {
  replies(by_product, offering("DOWNLOAD_PS_PLUS_TRIAL"));
  const auto trial = asked(standard);
  ASSERT_TRUE(trial);
  EXPECT_TRUE(trial.value().offers_trial());
  EXPECT_FALSE(trial.value().library_offer());
}

TEST_F(StoreAppTitleByProduct, DoesNotTakeADemoDownloadForATrial) {
  replies(by_product, offering("DOWNLOAD_TRIAL"));
  const auto offered_demo = asked(standard);
  ASSERT_TRUE(offered_demo);
  EXPECT_FALSE(offered_demo.value().offers_trial());
}

TEST_F(StoreAppTitleByProduct, AnswersAnOwnedRemasterThatStreams) {
  replies(by_product, fixture("product_FAKE0056TITLE000.json"));
  const auto answer = asked(remaster);
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer.value().owned_per_store());
  EXPECT_EQ(answer.value().streaming(), Streaming::streams);
  EXPECT_FALSE(answer.value().library_offer());
  EXPECT_FALSE(answer.value().offers_library_add());
}

TEST_F(StoreAppTitleByProduct, ReadsTheConceptOfAnOwnedProduct) {
  replies(by_product, fixture("product_FAKE0056TITLE000.json"));
  auto app = libchiaking::users::CatalogCalls{*user}.store_app(rig.client);
  ASSERT_TRUE(app);
  const auto details = app.value().concept_by_product(remaster, {});
  ASSERT_TRUE(details);
  EXPECT_EQ(details.value().concept_id(), "900002");
  EXPECT_FALSE(edition(details.value(), ps4_edition).owned);
}

TEST_F(StoreAppTitleByProduct, AsksForAProductWithASkuWithoutIt) {
  replies(by_product, fixture("product_FAKE0056TITLE000.json"));
  const auto with_sku = asked("EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  ASSERT_TRUE(with_sku);
  EXPECT_TRUE(with_sku.value().owned_per_store());
  EXPECT_EQ(with_sku.value().product_id(),
            "EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  EXPECT_NE(rig.http.seen.back().url.find(
                "variables=%7B%22productId%22%3A%22EP0109-PPSA17001_00-"
                "FAKE0056TITLE000%22%7D"),
            std::string::npos);
}

TEST_F(StoreAppTitleByProduct, CallsAnAnswerForAnotherProductMalformed) {
  replies(by_product, fixture("product_FAKE0056TITLE000.json"));
  const auto answer = asked(standard);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::malformed);
}

TEST_F(StoreAppTitleByProduct, CallsAMissingProductNotListed) {
  replies(by_product, enveloped(R"({"productRetrieve":null})"));
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::not_listed);
}

TEST_F(StoreAppTitleByProduct, CallsAnUnpricedProductMalformed) {
  replies(
      by_product,
      enveloped(
          R"({"productRetrieve":{"id":"EP0109-PPSA17001_00-FAKE0056TITLE000","concept":{"id":"900002"}}})"));
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::malformed);
}

TEST_F(StoreAppTitleByProduct, CallsAFailingStoreUnavailable) {
  rig.http.on("GET", std::string{by_product}, {Reply{503, ""}});
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::unavailable);
  EXPECT_EQ(answer.error().http_status(), 503);
  EXPECT_FALSE(answer.error().needs_sign_in());
}

TEST_F(StoreAppTitleByProduct, IsCancelledWithoutARequest) {
  rig.stop->stopped = true;
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_TRUE(answer.error().is_cancelled());
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(StoreAppTitleByProduct, NeedsASignInWhenTheRenewedTokenIsRefused) {
  rig.http.on("GET", std::string{by_product}, {Reply{401, "{}"}});
  ps_issues_a_token_for_the_seed("renewed");
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::user);
  EXPECT_TRUE(answer.error().needs_sign_in());
  ASSERT_TRUE(answer.error().cause());
  EXPECT_EQ(answer.error().cause()->reason,
            libchiaking::users::Unanswered::Reason::store_access_lapsed);
  EXPECT_EQ(answer.error().http_status(), 401);
  EXPECT_EQ(rig.http.count(by_product), 2);
  EXPECT_EQ(renewals(), 1);
  EXPECT_FALSE(user->is_expired());
}

}  // namespace
}  // namespace catalog_test
