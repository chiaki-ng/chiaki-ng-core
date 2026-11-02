#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_app_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::ConceptId;
using libchiaking::catalog::ProductId;
using libchiaking::catalog::StorefrontAnswer;
using libchiaking::catalog::Unanswered;
using Streaming = StorefrontAnswer::Streaming;
using Strings = std::vector<std::string>;

class StoreAppConceptDetails : public ::testing::Test, public StoreAppRig {
 protected:
  static auto owned_pricing() -> std::string {
    auto priced = fixture("pricing_900002.json");
    const auto at = priced.find("\"ownedTitles\": []");
    EXPECT_NE(at, std::string::npos);
    priced.replace(
        at, std::string_view{"\"ownedTitles\": []"}.size(),
        R"("ownedTitles": [{"productId":"EP0109-PPSA17001_00-FAKE0056TITLE000-E008"}])");
    return priced;
  }
};

class StoreAppTitleByConcept : public StoreAppConceptDetails {
 protected:
  static auto asked_as_the_app(const std::vector<FragmentHttp::Seen>& requests)
      -> ::testing::AssertionResult {
    for (const auto& seen : requests) {
      if (!seen.url.starts_with(
              "https://m.np.playstation.com/api/graphql/v1/op?"))
        return ::testing::AssertionFailure()
               << seen.url << " is not a graphql operation";
      if (seen.url.find("variables=%7B%22conceptId%22%3A%22900002%22%7D") ==
          std::string::npos)
        return ::testing::AssertionFailure()
               << seen.url << " does not name concept 900002";
      bool authorised = false;
      bool overridden = false;
      for (const auto& header : seen.headers) {
        authorised = authorised || (header.name == "Authorization" &&
                                    header.value == "Bearer app");
        overridden = overridden || header.name == "x-psn-store-locale-override";
      }
      if (!authorised)
        return ::testing::AssertionFailure()
               << seen.url << " is not authorised with the app token";
      if (overridden)
        return ::testing::AssertionFailure()
               << seen.url << " overrides the store locale";
    }
    return ::testing::AssertionSuccess();
  }
};

TEST_F(StoreAppConceptDetails, ReadTheConceptFields) {
  const auto details = concept_of(fixture("concept_900002.json"));
  ASSERT_TRUE(details.has_value());
  EXPECT_EQ(details->concept_id(), "900002");
  EXPECT_EQ(details->name(), "Skyline Zero Ember™");
  EXPECT_EQ(details->publisher(), "Example Publisher");
  EXPECT_EQ(details->release_date(), "2017-12-06T00:00:00Z");
  EXPECT_EQ(details->genres(), (Strings{"Role Playing Games", "Action"}));
  EXPECT_EQ(details->rating().authority, "PEGI");
  EXPECT_EQ(details->rating().name, "PEGI_16");
  EXPECT_FALSE(details->priced());
  ASSERT_EQ(details->products().size(), 1U);
  EXPECT_EQ(details->products().front().id, remaster);
  EXPECT_TRUE(details->products().front().offers.empty());
  EXPECT_TRUE(details->media().preview_video.ends_with(".mp4"));
  EXPECT_FALSE(details->media().logo.empty());
  EXPECT_FALSE(details->media().cover.empty());
  EXPECT_FALSE(details->media().key_art.empty());
  EXPECT_EQ(details->media().screenshots.size(), 2U);
}

TEST_F(StoreAppConceptDetails, ReadTheConceptStreamingNotice) {
  const auto details = skyline();
  EXPECT_TRUE(details.streaming().notices);
  EXPECT_TRUE(details.streaming().listed);
  EXPECT_TRUE(details.streaming().supported);
  EXPECT_EQ(details.streaming().platforms, (Strings{"PS5", "DWC"}));
  EXPECT_EQ(details.streaming().value, "OPTIONAL");
}

TEST_F(StoreAppConceptDetails, ReadAnEditionAndItsStreamingNotice) {
  const auto details = skyline();
  const auto& remastered = edition(details, remaster);
  EXPECT_EQ(remastered.name, "Skyline Zero Ember™ Remastered");
  EXPECT_EQ(remastered.np_title_id, "PPSA17001_00");
  EXPECT_EQ(remastered.platforms, Strings{"PS5"});
  EXPECT_EQ(remastered.release_date, "2024-10-31T00:00:00Z");
  EXPECT_EQ(remastered.classification, "FULL_GAME");
  EXPECT_TRUE(remastered.streaming.notices);
  EXPECT_TRUE(remastered.streaming.supported);
  EXPECT_EQ(remastered.streaming.platforms, (Strings{"PS5", "DWC"}));
  EXPECT_EQ(remastered.streaming.value, "OPTIONAL");
  EXPECT_TRUE(remastered.streaming_notice.starts_with(
      "Streaming currently supported: Skyline Zero Ember™ Remastered<br/><br/>"
      "Streaming currently not supported: Skyline Zero Ember™ Artbook"));
}

TEST_F(StoreAppConceptDetails, GiveThePs4EditionNoticesWithoutStreaming) {
  const auto details = skyline();
  const auto& old_edition = edition(details, ps4_edition);
  EXPECT_TRUE(old_edition.streaming.notices);
  EXPECT_FALSE(old_edition.streaming.listed);
  EXPECT_FALSE(old_edition.streaming.supported);
}

TEST_F(StoreAppConceptDetails, ReadTheOffersOfAnEdition) {
  const auto details = skyline();
  EXPECT_TRUE(details.priced());
  EXPECT_TRUE(details.owned_titles().empty());
  EXPECT_TRUE(details.purchased_titles().empty());
  const auto& remastered = edition(details, remaster);
  EXPECT_FALSE(remastered.owned);
  ASSERT_EQ(remastered.offers.size(), 2U);
  const auto& catalog_offer = remastered.offers[0];
  EXPECT_EQ(catalog_offer.cta_type,
            libchiaking::catalog::ps::StoreAppTitle::library_offer_action);
  EXPECT_EQ(catalog_offer.cta_type, "DOWNLOAD_PS_PLUS_GAME_CATALOG");
  EXPECT_EQ(catalog_offer.action_type, "BACKGROUND_PURCHASE_AND_DOWNLOAD");
  EXPECT_EQ(catalog_offer.sku_id, "EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  EXPECT_EQ(catalog_offer.entitlement_ids,
            (Strings{"EP0109-PPSA17001_00-FAKE0056TITLE000",
                     "EP0109-PPSA27022_00-0000000000000000"}));
  EXPECT_EQ(catalog_offer.entitled_via_subscription, false);
  EXPECT_EQ(catalog_offer.reward_id, "SUB001-SUBC00002_00-FIXTUREREWARD001");
  EXPECT_TRUE(catalog_offer.is_free);
  EXPECT_TRUE(catalog_offer.tied_to_subscription);
  EXPECT_EQ(catalog_offer.service_branding, Strings{"PS_PLUS"});
  EXPECT_EQ(catalog_offer.discounted_price, "Included");
  EXPECT_TRUE(
      catalog_offer.cloud_streaming_message.starts_with("Add to your library"));
  EXPECT_EQ(catalog_offer.ineligibility_reasons, Strings{"UNKNOWN"});
  EXPECT_FALSE(catalog_offer.pre_order);
  const auto& cart = remastered.offers[1];
  EXPECT_EQ(cart.cta_type, "ADD_TO_CART");
  EXPECT_EQ(cart.action_type, "ADD_TO_CART");
  EXPECT_EQ(cart.base_price, "£44.99");
  EXPECT_EQ(cart.discounted_price, "£44.99");
  EXPECT_EQ(cart.base_price_value, 4499);
  EXPECT_EQ(cart.discounted_value, 4499);
  EXPECT_EQ(cart.currency_code, "GBP");
  EXPECT_FALSE(cart.is_free);
  EXPECT_FALSE(cart.tied_to_subscription);
  EXPECT_FALSE(cart.entitled_via_subscription.has_value());
  EXPECT_EQ(cart.reward_id, "OUTRIGHT");
}

TEST_F(StoreAppConceptDetails, MarkTheOwnedEdition) {
  auto details = concept_of(fixture("concept_900002.json"));
  ASSERT_TRUE(details.has_value());
  const auto merged = priced_of(std::move(*details), owned_pricing());
  ASSERT_TRUE(merged.has_value());
  EXPECT_TRUE(edition(*merged, remaster).owned);
  EXPECT_FALSE(edition(*merged, ps4_edition).owned);
}

TEST_F(StoreAppConceptDetails, RefuseAMissingOrNamelessConcept) {
  EXPECT_FALSE(concept_of(enveloped(R"({"conceptRetrieve":null})")));
  EXPECT_FALSE(concept_of(enveloped(R"({"conceptRetrieve":{"name":"x"}})")));
}

TEST_F(StoreAppConceptDetails, RefusePricingOfAnotherOrNoConcept) {
  const auto details = concept_of(fixture("concept_900002.json"));
  ASSERT_TRUE(details.has_value());
  EXPECT_FALSE(
      priced_of(*details, enveloped(R"({"conceptRetrieve":{"id":"900002"}})")));
  EXPECT_FALSE(priced_of(
      *details,
      enveloped(
          R"({"conceptRetrieve":{"id":"1","ownedTitles":[],"selectableProducts":{"purchasableProducts":[],"purchasedTitles":[]}}})")));
}

TEST_F(StoreAppConceptDetails, AcceptPricingWithoutProducts) {
  const auto details = concept_of(fixture("concept_900002.json"));
  ASSERT_TRUE(details.has_value());
  EXPECT_TRUE(priced_of(
      *details,
      enveloped(
          R"({"conceptRetrieve":{"id":"900002","ownedTitles":[],"selectableProducts":{"purchasableProducts":[],"purchasedTitles":[]}}})")));
}

TEST_F(StoreAppTitleByConcept, StreamsTheRemasterFromItsProductNotice) {
  sells_skyline();
  const auto answer = asked_of_concept(remaster);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(), Streaming::streams);
  EXPECT_TRUE(answer.value().says_it_streams());
  EXPECT_EQ(answer.value().answered_product(), PsRig::product(remaster));
  EXPECT_EQ(answer.value().product_id(), remaster);
  EXPECT_EQ(answer.value().account(), user->account_id());
  EXPECT_EQ(answer.value().checked_at(), rig.now());
  EXPECT_TRUE(answer.value().notice().empty());
}

TEST_F(StoreAppTitleByConcept, AsksForTheConceptThenItsPricingAsTheApp) {
  sells_skyline();
  ASSERT_TRUE(asked_of_concept(remaster));
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_NE(rig.http.seen[0].url.find(by_concept), std::string::npos);
  EXPECT_NE(rig.http.seen[1].url.find(pricing), std::string::npos);
  EXPECT_TRUE(asked_as_the_app(rig.http.seen));
}

TEST_F(StoreAppTitleByConcept, CallsThePs4EditionNotStreamable) {
  const auto details = skyline();
  const auto& old_edition = edition(details, ps4_edition);
  sells_skyline();
  const auto answer = asked_of_concept(ps4_edition);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(), Streaming::not_streamable);
  EXPECT_EQ(answer.value().notice(), old_edition.streaming_notice);
  EXPECT_FALSE(answer.value().says_it_streams());
}

TEST_F(StoreAppTitleByConcept, AnswersTheLibraryOfferOfAnEdition) {
  sells_skyline();
  const auto answer = asked_of_concept(remaster);
  ASSERT_TRUE(answer);
  EXPECT_FALSE(answer.value().owned_per_store());
  const auto& offer = answer.value().library_offer();
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->sku_id, "EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  EXPECT_EQ(offer->entitlement_ids,
            (Strings{"EP0109-PPSA17001_00-FAKE0056TITLE000",
                     "EP0109-PPSA27022_00-0000000000000000"}));
  EXPECT_EQ(offer->entitled_through_subscription, false);
  EXPECT_TRUE(answer.value().offers_library_add());
}

TEST_F(StoreAppTitleByConcept, OffersNothingForAnUnsoldEdition) {
  sells_skyline();
  const auto missing = asked_of_concept("EP0109-PPSA99999_00-NOTHERE");
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing.value().library_offer());
  EXPECT_FALSE(missing.value().offers_library_add());
}

TEST_F(StoreAppTitleByConcept, AnswersAProductWithASkuByItsEdition) {
  sells_skyline();
  const auto with_sku =
      asked_of_concept("EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  ASSERT_TRUE(with_sku);
  EXPECT_EQ(with_sku.value().answered_product(), PsRig::product(remaster));
  EXPECT_EQ(with_sku.value().product_id(),
            "EP0109-PPSA17001_00-FAKE0056TITLE000-E008");
  EXPECT_EQ(with_sku.value().streaming(), Streaming::streams);
}

TEST_F(StoreAppTitleByConcept, CallsAnotherProductOfTheSameTitleMissing) {
  sells_skyline();
  const auto same_title =
      asked_of_concept("UP0114-PPSA17001_00-FAKE0119TITLE000");
  ASSERT_TRUE(same_title);
  EXPECT_EQ(same_title.value().streaming(), Streaming::edition_missing);
  EXPECT_FALSE(same_title.value().answered_product());
  EXPECT_FALSE(same_title.value().owned_per_store());
}

TEST_F(StoreAppTitleByConcept, CallsAnUnsoldProductMissing) {
  sells_skyline();
  const auto missing = asked_of_concept("EP0109-PPSA99999_00-NOTHERE");
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing.value().streaming(), Streaming::edition_missing);
}

TEST_F(StoreAppTitleByConcept, AnswersOwnershipPerEdition) {
  sells_skyline(owned_pricing());
  const auto owned = asked_of_concept(remaster);
  ASSERT_TRUE(owned);
  EXPECT_TRUE(owned.value().owned_per_store());
  EXPECT_FALSE(owned.value().offers_library_add());
  const auto other = asked_of_concept(ps4_edition);
  ASSERT_TRUE(other);
  EXPECT_FALSE(other.value().owned_per_store());
}

TEST_F(StoreAppTitleByConcept, CallsAMissingConceptNotListedWithoutPricing) {
  replies(by_concept, enveloped(R"({"conceptRetrieve":null})"));
  const auto answer = asked_of_concept(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::not_listed);
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(StoreAppTitleByConcept, CallsANamelessConceptMalformed) {
  replies(by_concept, enveloped(R"({"conceptRetrieve":{"name":"x"}})"));
  const auto answer = asked_of_concept(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::malformed);
  EXPECT_FALSE(answer.error().needs_sign_in());
}

TEST_F(StoreAppTitleByConcept, CallsUnreadablePricingMalformed) {
  sells_skyline(enveloped(R"({"conceptRetrieve":{"id":"900002"}})"));
  const auto answer = asked_of_concept(remaster);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), Unanswered::Reason::malformed);
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

TEST_F(StoreAppTitleByConcept, StreamsAnEditionPricedWithoutOffers) {
  sells_skyline(enveloped(
      R"({"conceptRetrieve":{"id":"900002","ownedTitles":[],"selectableProducts":{"purchasableProducts":[],"purchasedTitles":[]}}})"));
  const auto answer = asked_of_concept(remaster);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(), Streaming::streams);
  EXPECT_FALSE(answer.value().library_offer());
  EXPECT_FALSE(answer.value().owned_per_store());
  EXPECT_FALSE(answer.value().offers_library_add());
}

TEST_F(StoreAppTitleByConcept, NeverLetsTheConceptNoticeDecideAProduct) {
  const auto details = skyline();
  EXPECT_TRUE(details.streaming().supported);
  sells_skyline();
  const auto old_edition = asked_of_concept(ps4_edition);
  ASSERT_TRUE(old_edition);
  EXPECT_EQ(old_edition.value().streaming(), Streaming::not_streamable);
  const auto unsold = asked_of_concept("EP0000-PPSA00001_00-BARE");
  ASSERT_TRUE(unsold);
  EXPECT_EQ(unsold.value().streaming(), Streaming::edition_missing);
  EXPECT_FALSE(unsold.value().says_it_streams());
  const auto bare = PsRig::product("EP0000-PPSA00001_00-BARE");
  const std::array told{StorefrontAnswer::Product{bare}};
  EXPECT_EQ(
      StorefrontAnswer::of(user->account_id(), bare, told, {}).streaming(),
      Streaming::no_notice);
}

TEST(StorefrontAnswerStreaming, IsUndeterminedForAnEditionWithoutANotice) {
  const auto bare = PsRig::product("EP0000-PPSA00001_00-BARE");
  const auto answered = [&bare](std::optional<bool> supported) {
    const std::array told{StorefrontAnswer::Product{bare, supported, "notice"}};
    return StorefrontAnswer::of(libchiaking::AccountId{7}, bare, told, {})
        .streaming();
  };
  EXPECT_EQ(answered(std::nullopt), Streaming::no_notice);
  EXPECT_EQ(answered(false), Streaming::not_streamable);
  EXPECT_EQ(answered(true), Streaming::streams);
}

TEST(StoreAppProductIds, DropTheirSkuToNameTheEdition) {
  EXPECT_EQ(
      PsRig::product("EP0109-PPSA17001_00-FAKE0056TITLE000-E008").without_sku(),
      StoreAppRig::remaster);
  EXPECT_EQ(PsRig::product(StoreAppRig::remaster).without_sku(),
            StoreAppRig::remaster);
  EXPECT_EQ(PsRig::product("A-B-C-x008").without_sku(), "A-B-C-x008");
}

TEST(StoreAppProductIds, CarryTheirTitleNumber) {
  const auto number = PsRig::product("EP0109-PPSA17001_00-X").title_number();
  ASSERT_TRUE(number);
  EXPECT_EQ(number->text(), "PPSA17001");
}

TEST(StoreAppProductIds, RefuseBrokenText) {
  EXPECT_TRUE(ProductId::parse(StoreAppRig::remaster));
  EXPECT_FALSE(ProductId::parse("bad id"));
  EXPECT_FALSE(ProductId::parse("\"x"));
}

TEST(StoreAppConceptIds, AreDigitsOnly) {
  EXPECT_TRUE(ConceptId::parse("900002"));
  EXPECT_FALSE(ConceptId::parse("22a"));
  EXPECT_FALSE(ConceptId::parse(""));
}

}  // namespace
}  // namespace catalog_test
