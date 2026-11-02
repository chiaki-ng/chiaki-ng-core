#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

using Listing = CatalogApiTest;

TEST_F(Listing, BuildsTheListTheLibraryAndMyGamesFromPsReplies) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(
      rig,
      R"({"links":[{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"},{"id":"not a product!","name":"Dropped"}]})");
  ps5_store_lists(
      rig,
      store_list(
          {comet_listed, rivet_listed,
           R"({"productId":"EP-PPSA10009_00-NODEVICE","name":"No Device","conceptId":9,"streamingSupported":true})"}),
      store_list({comet_listed}));
  library_holds(
      rig,
      owned_page(
          {rivet_owned,
           R"({"id":"EP0109-PPSA07028_00-FAKE0002TITLE000","product_id":"EP0109-PPSA07028_00-FAKE0002TITLE000","active_flag":true,"feature_type":0,"entitlement_attributes":[{"platform_id":"ps5"}],"game_meta":{"name":"Rivet Armour","package_type":"PS5AC"}})"}));
  premium_includes(rig, {premium_offer(comet, "Comet")});

  const auto list = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(list);
  const auto& streamable = *list.value();
  EXPECT_EQ(streamable.region().text(), "en-GB");
  EXPECT_EQ(streamable.settled_locale().text(), "en-GB");
  EXPECT_EQ(streamable.origin(), ListOrigin::account_service);
  EXPECT_TRUE(streamable.is_complete());
  EXPECT_EQ(streamable.fetched_at(), rig.now());
  EXPECT_EQ(streamable.titles().size(), 3U);
  const auto* old = streamable.find(product(classic));
  ASSERT_NE(old, nullptr);
  EXPECT_EQ(old->title.service(), ServiceGeneration::old_service);
  EXPECT_EQ(old->title.platform(), Platform::ps4);
  EXPECT_EQ(old->title.name(), "Classic");
  const auto* cloud = streamable.find(product(comet));
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(cloud->title.service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(cloud->title.platform(), Platform::ps5);
  EXPECT_TRUE(cloud->inclusion.is_included());
  const auto* bought = streamable.find(product(rivet));
  ASSERT_NE(bought, nullptr);
  EXPECT_FALSE(bought->inclusion.is_included());
  EXPECT_FALSE(streamable.covers(product("EP-PPSA10009_00-NODEVICE")));
  EXPECT_EQ(streamable.count_on(ServiceGeneration::old_service), 1U);
  EXPECT_EQ(streamable.count_on(ServiceGeneration::ps5_cloud), 2U);

  const auto library = rig.catalog.library_of(*user);
  ASSERT_TRUE(library);
  EXPECT_EQ(library.value()->account(), AccountId{1});
  ASSERT_EQ(library.value()->size(), 1U);
  EXPECT_EQ(library.value()->titles()[0].id().text(), rivet);
  EXPECT_EQ(library.value()->fetched_at(), rig.now());

  const auto games = loaded(rig, *user);
  EXPECT_EQ(games->account(), AccountId{1});
  EXPECT_EQ(shown(*games), 3U);
  EXPECT_TRUE(games->hidden().empty());
  ASSERT_FALSE(games->visible().empty());
  EXPECT_EQ(games->visible()[0].product_id(), product(rivet));

  const auto& owned = game_of(*games, rivet);
  EXPECT_TRUE(owned.is_owned());
  EXPECT_EQ(owned.play_path(), PlayPath::owned);
  EXPECT_EQ(owned.standing(), Standing::in_library);
  EXPECT_EQ(owned.title().service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(owned.launch_identifier().text(), rivet);
  EXPECT_FALSE(owned.needs_library_add());
  EXPECT_TRUE(owned.needs_storefront_check());

  const auto& included = game_of(*games, comet);
  EXPECT_FALSE(included.is_owned());
  EXPECT_EQ(included.play_path(), PlayPath::included);
  EXPECT_EQ(included.standing(), Standing::included);
  EXPECT_TRUE(included.needs_library_add());
  EXPECT_EQ(included.inclusion().source(),
            catalog::PlusInclusion::Source::store_tags);
  ASSERT_TRUE(included.library_offer());
  EXPECT_EQ(included.library_offer()->sku_id, std::string{comet} + "-E001");

  const auto& streamed = game_of(*games, classic);
  EXPECT_FALSE(streamed.is_owned());
  EXPECT_EQ(streamed.play_path(), PlayPath::old_service);
  EXPECT_EQ(streamed.title().service(), ServiceGeneration::old_service);
  EXPECT_FALSE(streamed.needs_library_add());
  EXPECT_FALSE(streamed.needs_storefront_check());
  EXPECT_EQ(streamed.standing(), Standing::included);

  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(all_ps5_list), 1);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));
  EXPECT_TRUE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.catalog.explain(*user, product(rivet)).is_listed());
  EXPECT_FALSE(rig.catalog.explain(*user, product("EP-PPSA10009_00-NODEVICE"))
                   .is_listed());
}

TEST_F(Listing, HidesALegacyCloudStreamTitleWhoseStreamingCopyNoPlanCovers) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.on(
      "GET",
      "/api/catalog/v2/"
      "products?productIds=EP-CUSA00001_00-LEGACY_CLOUD_STREAM&country=GB&"
      "language=en&age=34",
      {{200,
        R"([{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","skus":[{"contentCollections":[{"branding":"PS_PLUS_DOWNLOAD"}],"entitlements":[{"packageType":"PS4GD"}]}]}])"}});
  const auto games = loaded(rig, *user);
  const auto* uncovered =
      games->find(product("EP-CUSA00001_00-LEGACY_CLOUD_STREAM"));
  ASSERT_NE(uncovered, nullptr);
  EXPECT_FALSE(uncovered->is_visible());
  EXPECT_EQ(uncovered->visibility().hide_reason(),
            catalog::HideReason::not_in_service);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(
                *user, product("EP-CUSA00001_00-LEGACY_CLOUD_STREAM"))),
            LaunchRefusal::Kind::hidden);

  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->legacy_cloud_stream.origin,
            catalog::ListOrigin::account_service);
  EXPECT_TRUE(report->legacy_cloud_stream.complete);
  EXPECT_EQ(report->legacy_cloud_stream.titles, 1U);
  EXPECT_EQ(report->legacy_cloud_stream.fetched_at, rig.now());
  EXPECT_EQ(report->legacy_cloud_stream.copies_checked,
            (catalog::StreamableList::CopiesChecked{1, 1}));
  EXPECT_EQ(report->legacy_cloud_stream.without_a_covered_copy, 1U);
  ASSERT_TRUE(report->cloud_stream.audience);
  EXPECT_EQ(report->cloud_stream.audience->country, "GB");
  EXPECT_EQ(report->cloud_stream.audience->language, "en");
  EXPECT_EQ(report->cloud_stream.audience->age, 34);
  ASSERT_TRUE(report->cloud_stream.premium_category);
  EXPECT_EQ(report->cloud_stream.premium_category->titles, 86U);
  EXPECT_EQ(report->cloud_stream.premium_category->products_read, 86U);
  EXPECT_EQ(report->cloud_stream.premium_category->server_total, 86U);
  EXPECT_TRUE(report->cloud_stream.premium_category->complete);
  EXPECT_EQ(report->cloud_stream.premium_category->fetched_at, rig.now());
  ASSERT_TRUE(report->cloud_stream.owned);
  EXPECT_EQ(report->cloud_stream.owned->state, catalog::LibraryState::fetched);
  EXPECT_EQ(report->cloud_stream.owned->records_read, 1U);
  EXPECT_EQ(report->cloud_stream.owned->revision_restarts, 0U);
  EXPECT_EQ(report->cloud_stream.owned->fetched_at, rig.now());
  EXPECT_EQ(report->cloud_stream.titles, 87U);
  EXPECT_FALSE(report->cloud_stream.trusted_time);
  ASSERT_TRUE(report->cloud_stream.exclusions);
  EXPECT_TRUE(report->cloud_stream.exclusions->fresh);
  EXPECT_EQ(report->library, catalog::LibraryState::fetched);
  EXPECT_EQ(report->library_records, 1U);
  EXPECT_EQ(report->library_fetched_at, rig.now());

  Rig stranger;
  EXPECT_FALSE(stranger.catalog.how_fetched(*stranger.user()));
}

TEST_F(Listing, KeepsAnOwnedPs5TitleTheListMarksUnstreamable) {
  Rig rig;
  const auto user = rig.user();
  const std::string modern_warfare = "EP0101-PPSA27021_00-FAKE0072TITLE000";
  old_service_lists(rig);
  ps5_store_lists(
      rig,
      store_list(
          {comet_listed,
           R"({"productId":"EP0101-PPSA07041_00-FAKE0023TITLE000","name":"Line of Fire®","conceptId":90000015,"device":["PS5"],"streamingSupported":false})"}));
  library_holds(
      rig,
      owned_page(
          {R"({"id":"EP0101-PPSA27021_00-FAKE0072TITLE000","productId":"EP0101-PPSA07041_00-FAKE0022TITLE000",)"
           R"("activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],)"
           R"("gameMeta":{"name":"Line of Fire® Front Line 2","packageType":"PSGD"},"streamingSupported":true})"}));
  const auto games = loaded(rig, *user);
  const auto& game = game_of(*games, modern_warfare);
  EXPECT_TRUE(game.is_visible());
  EXPECT_TRUE(game.is_owned());
  EXPECT_EQ(game.standing(), Standing::in_library);
  ASSERT_TRUE(game.ownership());
  EXPECT_EQ(game.ownership()->streams(), true);
  EXPECT_NE(game.title().streamability(),
            catalog::ListStreamability::marked_unstreamable);
  const auto list = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(list);
  ASSERT_EQ(list.value()->marked_unstreamable().size(), 1U);
  EXPECT_EQ(list.value()->marked_unstreamable().begin()->text(), "PPSA07041");
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));

  auto restarted_catalog = restarted(rig);
  const auto asked = rig.http.requests();
  const auto reloaded = restarted_catalog.process_library_and_list(*user);
  ASSERT_TRUE(reloaded);
  EXPECT_EQ(rig.http.count("imagic"), 6);
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_LE(asked, rig.http.requests());
  EXPECT_TRUE(game_of(*reloaded.value(), modern_warfare).is_visible());
  EXPECT_EQ(game_of(*reloaded.value(), modern_warfare).standing(),
            Standing::in_library);
}

TEST_F(Listing, ListsPs5GamesWhenTheOldServiceListFails) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  old_service_opens(rig);
  rig.http.on("GET", std::string{old_service_stores}, {{500, "{}"}});
  library_holds(rig, owned_page({rivet_owned}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  cloud_streaming_allowed(rig);
  EXPECT_FALSE(rig.catalog.get_streamable_list(*user));
  const auto games = loaded(rig, *user);
  EXPECT_EQ(games->find(product(classic)), nullptr);
  EXPECT_TRUE(game_of(*games, rivet).is_owned());
  EXPECT_TRUE(game_of(*games, rivet).is_visible());
  EXPECT_TRUE(game_of(*games, comet).needs_library_add());
  EXPECT_EQ(shown(*games), 2U);
  const auto verdict = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->store.text(), "en-GB");
  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  EXPECT_TRUE(report->cloud_stream.premium_category);
  EXPECT_EQ(report->cloud_stream.titles, 87U);
  EXPECT_EQ(report->legacy_cloud_stream.titles, 0U);
}

TEST_F(Listing, ListsLegacyCloudStreamGamesWhenEveryPs5SourceFails) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed}));
  library_holds(rig, owned_page({}));
  rig.http.on("GET", std::string{ps5_owned_list}, {{500, ""}});
  rig.http.on("GET", std::string{tag_grid}, {{500, ""}});
  rig.http.on("GET", std::string{cloud_benefits}, {{500, ""}});
  rig.server.last = catalog::TrustedClock::Sync{"cloudflare", rig.now()};
  const auto games = loaded(rig, *user);
  const auto& streamed = game_of(*games, classic);
  EXPECT_TRUE(streamed.is_visible());
  EXPECT_EQ(streamed.play_path(), PlayPath::old_service);
  EXPECT_EQ(games->size(), 1U);
  EXPECT_FALSE(games->ownership_is_known_on(ServiceGeneration::ps5_cloud));
  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->legacy_cloud_stream.titles, 1U);
  EXPECT_FALSE(report->cloud_stream.premium_category);
  ASSERT_TRUE(report->cloud_stream.owned);
  EXPECT_EQ(report->cloud_stream.owned->state,
            catalog::LibraryState::unavailable);
  ASSERT_TRUE(report->cloud_stream.trusted_time);
  EXPECT_EQ(report->cloud_stream.trusted_time->provider, "cloudflare");
  EXPECT_EQ(report->cloud_stream.trusted_time->synced_at, rig.now());
}

TEST_F(Listing, GivesEveryKindOfRowAStorePage) {
  constexpr std::string_view legacy_cloud_stream_game =
      "EP0100-CUSA00001_00-FAKE0104TITLE000";
  constexpr std::string_view premium = "EP0109-PPSA57002_00-FAKE0096TITLE000";
  constexpr std::string_view joined = "EP0109-PPSA57003_00-FAKE0066TITLE000";
  constexpr std::string_view bundle = "EP0109-PPSA57004_00-FAKE0013TITLE000";
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  old_service_lists(
      rig,
      R"({"links":[{"id":"EP0100-CUSA00001_00-FAKE0104TITLE000","name":"Classic"}]})");
  ps5_store_lists(rig, store_list({}), store_list({}));
  library_holds(rig, owned_page({rivet_owned, owned_ps5(joined, "Joined")}));
  premium_includes(
      rig,
      {premium_offer(premium, "Premium"), premium_offer(joined, "Joined"),
       R"({"id":"EP0109-PPSA57004_00-FAKE0013TITLE000","name":"Bundle","skus":[{"id":"EP0109-PPSA57004_00-FAKE0013TITLE000-E001","contentCollections":[{"branding":"PS_PLUS_STREAM_PS5"}],"entitlements":[{"id":"EP0109-PPSA57005_00-FAKE0011TITLE000","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true},{"id":"EP0109-PPSA57006_00-FAKE0012TITLE000","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true}]}]})"});
  store_sells(rig, {sale_offer(for_sale_only, "Buy To Stream")});
  const auto games = loaded(rig, *user);

  EXPECT_EQ(game_of(*games, legacy_cloud_stream_game).title().service(),
            ServiceGeneration::old_service);
  EXPECT_EQ(game_of(*games, rivet).play_path(), PlayPath::owned);
  EXPECT_FALSE(game_of(*games, rivet).inclusion().is_included());
  EXPECT_EQ(game_of(*games, premium).play_path(), PlayPath::included);
  EXPECT_EQ(game_of(*games, joined).play_path(), PlayPath::owned);
  EXPECT_TRUE(game_of(*games, joined).inclusion().is_included());
  EXPECT_EQ(game_of(*games, bundle).children().size(), 2U);
  EXPECT_EQ(game_of(*games, for_sale_only).play_path(), PlayPath::store_only);
  for (const auto id : {legacy_cloud_stream_game, rivet, premium, joined,
                        bundle, for_sale_only}) {
    const auto& page = game_of(*games, id).title().concept_page();
    ASSERT_TRUE(page) << id;
    EXPECT_TRUE(page->url().starts_with("https://store.playstation.com/en-gb/"))
        << id;
    EXPECT_TRUE(page->url().ends_with(id)) << id;
  }
}

}  // namespace
}  // namespace catalog_api_test
