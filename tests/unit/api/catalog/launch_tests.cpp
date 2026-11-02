#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_api_test {
namespace {

using Launch = CatalogApiTest;

TEST_F(Launch, ClearsAnOwnedPs5TitleAfterCloudStreamingAndItsEntitlement) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, rivet, streams_notice);
  (void)loaded(rig, *user);
  const auto held = rig.catalog.can_launch(*user, product(rivet));
  EXPECT_TRUE(held.is_allowed());
  EXPECT_TRUE(held.cleared_launch());
  EXPECT_EQ(rig.http.count(cloud_benefits), 1);
  EXPECT_EQ(rig.http.count(store_app), 0);

  const auto verdict = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_TRUE(verdict.is_allowed());
  EXPECT_FALSE(verdict.refusal());
  const auto& cleared = verdict.cleared_launch();
  ASSERT_TRUE(cleared);
  EXPECT_EQ(cleared->product, product(rivet));
  EXPECT_EQ(cleared->service, ServiceGeneration::ps5_cloud);
  EXPECT_EQ(cleared->platform, Platform::ps5);
  EXPECT_EQ(cleared->identifier.text(), rivet);
  ASSERT_TRUE(cleared->held_as);
  EXPECT_EQ(cleared->held_as->text(), rivet);
  EXPECT_EQ(cleared->store.text(), "en-GB");
  EXPECT_EQ(cleared->origin, ListOrigin::account_service);
  const std::string reread =
      std::string{ps5_owned_lookup} + std::string{rivet} + "?";
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);
  EXPECT_EQ(rig.http.count(reread), 1);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(plus_services), 0);
  const auto benefits_at = rig.http.first_asking(cloud_benefits);
  const auto reread_at = rig.http.first_asking(reread);
  const auto resolver_at = rig.http.first_asking(Rig::cloud_game_resolver);
  ASSERT_TRUE(benefits_at);
  ASSERT_TRUE(reread_at);
  ASSERT_TRUE(resolver_at);
  EXPECT_LT(*benefits_at, *reread_at);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_EQ(rig.http.count(ps5_owned_list), 1);
  EXPECT_EQ(rig.http.count(Rig::cloud_game_resolver), 1);
  EXPECT_EQ(rig.http.count(Rig::feature_config), 0);
  EXPECT_LT(*reread_at, *resolver_at);

  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);
  EXPECT_EQ(rig.http.count(reread), 2);
  rig.advance(std::chrono::minutes{6});
  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(cloud_benefits), 4);
  EXPECT_EQ(rig.http.count(reread), 3);
}

TEST_F(Launch, TellsEachCheckAsItStarts) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  (void)loaded(rig, *user);
  std::vector<LaunchCheck> checks;
  const catalog::LaunchCheckObserver observer{
      [&checks](LaunchCheck check) { checks.push_back(check); }};
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).is_allowed());
  EXPECT_TRUE(checks.empty());
  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet), {}, observer)
                  .is_allowed());
  EXPECT_EQ(
      checks,
      (std::vector{LaunchCheck::loading_library, LaunchCheck::cloud_streaming,
                   LaunchCheck::entitlement, LaunchCheck::cloud_game,
                   LaunchCheck::exclusions, LaunchCheck::release}));

  checks.clear();
  EXPECT_TRUE(
      rig.catalog.clear_for_launch(*user, product(classic), {}, observer)
          .is_allowed());
  EXPECT_EQ(checks,
            (std::vector{LaunchCheck::loading_library, LaunchCheck::membership,
                         LaunchCheck::release}));

  checks.clear();
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(nowhere),
                                                      {}, observer)),
            LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(checks, (std::vector{LaunchCheck::loading_library}));
}

TEST_F(Launch, RefusesAnIncludedTitleOutsideTheLibraryWithTheCategoryOffer) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig,
                  store_list({comet_listed, rivet_listed, listed_only_listed}),
                  store_list({comet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, comet, streams_notice + comet_library_offer);
  (void)loaded(rig, *user);
  const auto held = rig.catalog.can_launch(*user, product(comet));
  ASSERT_EQ(refusal_kind(held), LaunchRefusal::Kind::add_to_library);
  const auto verdict = rig.catalog.clear_for_launch(*user, product(comet));
  ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(verdict.cleared_launch());
  const auto& offer = verdict.refusal()->library_offer();
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->sku_id, "EP-PPSA10001_00-GAME-E001");
  EXPECT_EQ(offer->entitlement_ids,
            (std::vector<std::string>{"EP-PPSA10001_00-GAME"}));
  EXPECT_EQ(offer->entitled_through_subscription, true);
  EXPECT_EQ(held.refusal()->library_offer(), offer);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(
                "internal/entitlements/EP-PPSA10001_00-GAME?fields=gameMeta"),
            0);
  EXPECT_EQ(rig.http.count(Rig::cloud_game_resolver), 0);
  EXPECT_EQ(rig.http.count(Rig::feature_config), 1);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_EQ(rig.http.count(ps5_owned_list), 1);

  const auto unbought =
      rig.catalog.clear_for_launch(*user, product(listed_only));
  ASSERT_EQ(refusal_kind(unbought), LaunchRefusal::Kind::not_listed);
  EXPECT_FALSE(unbought.refusal()->library_offer());
  const auto missing = rig.catalog.clear_for_launch(*user, product(nowhere));
  EXPECT_EQ(refusal_kind(missing), LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(subscriptions), 0);

  const auto answer =
      rig.catalog.check_title_against_storefront(*user, product(comet));
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer.value().says_it_streams());
  EXPECT_EQ(rig.http.count(store_app), 1);
  EXPECT_TRUE(answer.value().offers_library_add());
  ASSERT_TRUE(answer.value().library_offer());
  EXPECT_EQ(answer.value().library_offer()->sku_id,
            "EP-PPSA10001_00-GAME-E008");
  rig.http.on("GET", std::string{store_app}, {{500, "{}"}});
  const auto again = rig.catalog.clear_for_launch(*user, product(comet));
  ASSERT_EQ(refusal_kind(again), LaunchRefusal::Kind::add_to_library);
  EXPECT_EQ(again.refusal()->library_offer(), offer);
  EXPECT_EQ(rig.http.count(store_app), 1);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
}

TEST_F(Launch, ClearsATitleAddedSinceTheListingOnceOwnershipIsRechecked) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, comet, streams_notice);
  (void)loaded(rig, *user);
  rig.advance(std::chrono::minutes{1});
  rig.http.before_everything("GET",
                             "internal/entitlements/EP-PPSA10001_00-GAME",
                             {{200, comet_owned}});
  const auto unclaimed = rig.catalog.clear_for_launch(*user, product(comet));
  EXPECT_EQ(refusal_kind(unclaimed), LaunchRefusal::Kind::add_to_library);
  EXPECT_EQ(rig.http.count("internal/entitlements/EP-PPSA10001_00-GAME"), 0);
  const auto rechecked = rig.catalog.recheck_ownership(*user, product(comet));
  ASSERT_TRUE(rechecked);
  EXPECT_TRUE(rechecked.value().is_owned());
  const auto verdict = rig.catalog.clear_for_launch(*user, product(comet));
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), comet);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count("internal/entitlements/EP-PPSA10001_00-GAME"), 2);
  const auto games = loaded(rig, *user);
  EXPECT_TRUE(game_of(*games, comet).is_owned());
  EXPECT_EQ(games->inputs().library_fetched_at, rig.now());
}

class EntitlementLaunch : public CatalogApiTest {
 protected:
  static constexpr std::string_view modern_warfare =
      "EP0101-PPSA27021_00-FAKE0072TITLE000";
  static inline const std::string modern_warfare_owned =
      R"({"id":"EP0101-PPSA27021_00-FAKE0072TITLE000","productId":"EP0101-PPSA07041_00-FAKE0022TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],)"
      R"("gameMeta":{"name":"Line of Fire® Front Line 2","packageType":"PSGD"},"streamingSupported":true})";

  static auto reread() -> std::string {
    return std::string{ps5_owned_lookup} + std::string{modern_warfare} + "?";
  }

  static void marked(Rig& rig, std::string_view facts) {
    old_service_lists(rig);
    ps5_store_lists(
        rig,
        store_list(
            {comet_listed,
             R"({"productId":"EP0101-PPSA07041_00-FAKE0023TITLE000","name":"Line of Fire®","conceptId":90000015,"device":["PS5"],"streamingSupported":false})"}));
    library_holds(rig, owned_page({modern_warfare_owned}));
    membership_is(rig, "TIER_30");
    cloud_streaming_allowed(rig);
    rig.http.on("GET", std::string{store_app}, {{200, std::string{facts}}});
  }
};

TEST_F(EntitlementLaunch, ClearsAnOwnedPs5TitleWhateverTheStoreSays) {
  Rig overruled;
  const auto owner = overruled.user();
  marked(overruled, "{}");
  const auto games = loaded(overruled, *owner);
  const auto& listed = game_of(*games, modern_warfare);
  EXPECT_TRUE(listed.is_owned());
  EXPECT_EQ(listed.standing(), Standing::in_library);
  const std::string sold{listed.product_id().text()};
  overruled.http.on("GET", std::string{store_app},
                    {{200, store_product(sold, no_stream_notice)}});
  EXPECT_TRUE(
      overruled.catalog.clear_for_launch(*owner, product(modern_warfare))
          .is_allowed());
  EXPECT_EQ(overruled.http.count(store_app), 0);
  EXPECT_EQ(overruled.http.count(reread()), 1);
}

TEST_F(EntitlementLaunch,
       RefusesAnOwnedPs5TitleWhoseEntitlementNoLongerStreams) {
  Rig withdrawn;
  const auto holder = withdrawn.user();
  marked(withdrawn, store_product(modern_warfare, streams_notice));
  (void)loaded(withdrawn, *holder);
  std::string no_longer = modern_warfare_owned;
  no_longer.replace(no_longer.find(R"("streamingSupported":true)"),
                    std::string_view{R"("streamingSupported":true)"}.size(),
                    R"("streamingSupported":false)");
  withdrawn.http.before_everything("GET", reread(), {{200, no_longer}});
  const auto blocked =
      withdrawn.catalog.clear_for_launch(*holder, product(modern_warfare));
  ASSERT_EQ(refusal_kind(blocked), LaunchRefusal::Kind::streaming_unavailable);
  EXPECT_TRUE(blocked.refusal()->notice().empty());
  EXPECT_EQ(withdrawn.http.count(store_app), 0);
}

TEST_F(EntitlementLaunch, IsNotOverruledByAStoreNoticeOnlyOnTheConcept) {
  Rig concept_only;
  const auto reader = concept_only.user();
  ps_as_usual(concept_only);
  membership_is(concept_only, "TIER_30");
  cloud_streaming_allowed(concept_only);
  concept_only.http.on("GET", std::string{store_app},
                       {{200, StoreConcept{.asked = rivet,
                                           .sold = rivet,
                                           .concept_facts = streams_notice}
                                  .reply()}});
  (void)loaded(concept_only, *reader);
  const auto told = concept_only.catalog.check_title_against_storefront(
      *reader, product(rivet));
  ASSERT_TRUE(told);
  EXPECT_EQ(told.value().streaming(), StorefrontAnswer::Streaming::no_notice);
  EXPECT_FALSE(told.value().says_it_streams());
  EXPECT_TRUE(concept_only.catalog.clear_for_launch(*reader, product(rivet))
                  .is_allowed());
  EXPECT_EQ(concept_only.http.count(store_app), 1);
}

TEST_F(EntitlementLaunch, IsNotOverruledByAStoreThatKnowsNoProduct) {
  Rig unknown;
  const auto asker = unknown.user();
  ps_as_usual(unknown);
  membership_is(unknown, "TIER_30");
  cloud_streaming_allowed(unknown);
  unknown.http.on("GET", std::string{store_app},
                  {{200, R"({"data":{"productRetrieve":null}})"}});
  (void)loaded(unknown, *asker);
  EXPECT_FALSE(
      unknown.catalog.check_title_against_storefront(*asker, product(rivet)));
  EXPECT_TRUE(
      unknown.catalog.clear_for_launch(*asker, product(rivet)).is_allowed());
  EXPECT_FALSE(unknown.has_record("user/1/refusals"));
}

TEST_F(EntitlementLaunch, IsNeverGrantedByTheStoreSayingAnUnownedTitleIsOwned) {
  Rig store_says_owned;
  const auto hopeful = store_says_owned.user();
  ps_as_usual(store_says_owned);
  membership_is(store_says_owned, "TIER_30");
  cloud_streaming_allowed(store_says_owned);
  const std::string owned_title =
      R"({"productId":")" + std::string{comet} + R"("})";
  store_says_owned.http.on("GET", std::string{store_app},
                           {{200, StoreConcept{.asked = comet,
                                               .sold = comet,
                                               .sold_facts = streams_notice,
                                               .owned_titles = owned_title}
                                      .reply()}});
  (void)loaded(store_says_owned, *hopeful);
  const auto claimed = store_says_owned.catalog.check_title_against_storefront(
      *hopeful, product(comet));
  ASSERT_TRUE(claimed);
  EXPECT_TRUE(claimed.value().owned_per_store());
  EXPECT_TRUE(claimed.value().says_it_streams());
  const auto still =
      store_says_owned.catalog.clear_for_launch(*hopeful, product(comet));
  EXPECT_EQ(refusal_kind(still), LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(still.cleared_launch());
  EXPECT_EQ(refusal_kind(
                store_says_owned.catalog.can_launch(*hopeful, product(comet))),
            LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(game_of(*loaded(store_says_owned, *hopeful), comet).is_owned());
}

TEST_F(Launch, ClearsAnOldServiceTitleWithoutAStorefrontCheck) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  (void)loaded(rig, *user);
  const auto unchecked =
      rig.catalog.check_title_against_storefront(*user, product(classic));
  ASSERT_FALSE(unchecked);
  EXPECT_EQ(unchecked.error().reason(), catalog::StorefrontReason::rejected);
  const auto verdict = rig.catalog.clear_for_launch(*user, product(classic));
  EXPECT_TRUE(verdict.is_allowed());
  const auto& cleared = verdict.cleared_launch();
  ASSERT_TRUE(cleared);
  EXPECT_EQ(cleared->product, product(classic));
  EXPECT_EQ(cleared->service, ServiceGeneration::old_service);
  EXPECT_EQ(cleared->platform, Platform::ps4);
  EXPECT_FALSE(cleared->held_as);
  EXPECT_EQ(cleared->identifier.text(), classic);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(subscriptions), 1);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  const auto unowned = rig.catalog.recheck_ownership(*user, product(classic));
  ASSERT_TRUE(unowned);
  EXPECT_EQ(unowned.value().kind(), OwnershipAnswer::Kind::not_owned);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  EXPECT_EQ(rig.http.count(owned_list), 1);
}

TEST_F(Launch,
       OffersAnOwnedLegacyCloudStreamCopyWithAnActivationDateAfterALoad) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed}), store_list({comet_listed}));
  library_holds(
      rig,
      owned_page(
          {R"({"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","product_id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","active_flag":true,"active_date":"2020-01-01T00:00:00.000Z","feature_type":3,"entitlement_attributes":[{"platform_id":"ps4"}],"game_meta":{"name":"Classic","package_type":"PS4GD"}})"}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  rig.server.synced = rig.now();
  const auto games = loaded(rig, *user);
  const auto& streamed = game_of(*games, classic);
  EXPECT_TRUE(streamed.is_owned());
  EXPECT_TRUE(streamed.release().time());
  EXPECT_GE(rig.server.refreshes.load(), 1);
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(classic)).is_allowed());
}

}  // namespace
}  // namespace catalog_api_test
