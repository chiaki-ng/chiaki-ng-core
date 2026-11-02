#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

using ForSale = CatalogApiTest;

TEST_F(ForSale, RemembersAUsedPlayFirstTrialFromTheSignedInStore) {
  constexpr std::string_view hail = "UP0113-PPSA37006_00-FAKE0126TITLE000";
  const std::string sold =
      R"({"id":"UP0113-PPSA37006_00-FAKE0126TITLE000","name":"Hail to the Rainbow","concept":{"id":"7"},"skus":[{"id":"UP0113-PPSA37006_00-FAKE0126TITLE000-U001","entitlements":[{"id":"UP0113-PPSA37006_00-FAKE0126TITLE000","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true}]},{"id":"UP0113-PPSA37006_00-FAKE0126TITLE000-U003","entitlements":[{"id":"UP0113-PPSA37006_00-FAKE0094TITLE000","packageType":"PSTRACK","featureType":"unknown"},{"id":"UP0113-PPSA37006_00-FAKE0126TITLE000","packageType":"PSGD","featureType":"PLAY_FIRST_TRIAL","streamingSupported":true}]}]})";
  const std::string bought_only =
      R"(,"mobilectas":[{"type":"ADD_TO_CART","action":{"type":"ADD_TO_CART","param":[{"name":"skuId","values":["UP0113-PPSA37006_00-FAKE0126TITLE000-U001"]}]}},{"type":"DOWNLOAD_TRIAL","action":{"type":"BACKGROUND_PURCHASE_AND_DOWNLOAD","param":[]}}])";
  const std::string trial_offered =
      R"(,"mobilectas":[{"type":"DOWNLOAD_PS_PLUS_TRIAL","action":{"type":"BACKGROUND_PURCHASE_AND_DOWNLOAD","param":[{"name":"skuId","values":["UP0113-PPSA37006_00-FAKE0126TITLE000-U003"]}]}}])";
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  store_sells(rig, {sold});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  rig.http.on("GET", std::string{store_app},
              {{200, store_product(hail, streams_notice + bought_only)},
               {200, store_product(hail, streams_notice + trial_offered)}});
  const auto offer = [&](Catalog& service) {
    return game_of(*loaded(service, *user), hail).trial_offer();
  };
  EXPECT_EQ(offer(rig.catalog), catalog::TrialOffer::play_first_trial);

  const auto used =
      rig.catalog.check_title_against_storefront(*user, product(hail));
  ASSERT_TRUE(used);
  EXPECT_FALSE(used.value().offers_trial());
  EXPECT_EQ(offer(rig.catalog), catalog::TrialOffer::none);
  EXPECT_TRUE(rig.has_record("user/1/used_trials"));
  auto restarted_catalog = restarted(rig);
  EXPECT_EQ(offer(restarted_catalog), catalog::TrialOffer::none);

  rig.advance(std::chrono::minutes{6});
  const auto offered =
      restarted_catalog.check_title_against_storefront(*user, product(hail));
  ASSERT_TRUE(offered);
  EXPECT_TRUE(offered.value().offers_trial());
  EXPECT_EQ(offer(restarted_catalog), catalog::TrialOffer::play_first_trial);
  EXPECT_FALSE(rig.has_record("user/1/used_trials"));
}

TEST_F(ForSale, AStreamableFullGameIsAStoreRowRefusedUntilBought) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  store_sells(
      rig,
      {sale_offer(for_sale_only, "Buy To Stream"), sale_offer(comet, "Comet"),
       sale_offer(rivet, "Rivet & Bolt: Split Worlds"),
       sale_offer(sold_trial, "Trial Only", "TRIAL"),
       sale_offer(sold_offline, "No Streaming", "FULL_GAME", "false")});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  const auto games = loaded(rig, *user);

  const auto& sold = game_of(*games, for_sale_only);
  EXPECT_EQ(sold.play_path(), PlayPath::store_only);
  EXPECT_TRUE(sold.is_visible());
  EXPECT_EQ(sold.standing(), Standing::store);
  EXPECT_FALSE(sold.is_owned());
  EXPECT_FALSE(sold.inclusion().is_included());
  EXPECT_FALSE(sold.library_offer());
  EXPECT_EQ(sold.title().name(), "Buy To Stream");
  EXPECT_EQ(sold.title().cover_url(), "https://image/Buy To Stream.png");
  ASSERT_TRUE(sold.title().concept_id());
  EXPECT_EQ(sold.title().concept_id()->text(), "7");
  ASSERT_TRUE(sold.title().concept_page());
  EXPECT_EQ(sold.title().concept_page()->url(),
            "https://store.playstation.com/en-gb/product/"
            "EP0109-PPSA47002_00-FAKE0017TITLE000");
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(for_sale_only))),
            LaunchRefusal::Kind::buy_to_stream);
  EXPECT_EQ(
      refusal_kind(rig.catalog.clear_for_launch(*user, product(for_sale_only))),
      LaunchRefusal::Kind::buy_to_stream);

  EXPECT_EQ(game_of(*games, comet).play_path(), PlayPath::included);
  EXPECT_EQ(game_of(*games, rivet).play_path(), PlayPath::owned);
  const auto rows_of = [&games](std::string_view id) {
    return std::ranges::count_if(games->visible(),
                                 [id](const catalog::MyGame& game) {
                                   return game.product_id().text() == id;
                                 });
  };
  EXPECT_EQ(rows_of(comet), 1);
  EXPECT_EQ(rows_of(rivet), 1);
  EXPECT_EQ(rows_of(for_sale_only), 1);
  EXPECT_EQ(games->find(product(sold_trial)), nullptr);
  EXPECT_EQ(games->find(product(sold_offline)), nullptr);

  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  ASSERT_TRUE(report->cloud_stream.for_sale_category);
  EXPECT_EQ(report->cloud_stream.for_sale_category->titles, 3U);
  EXPECT_EQ(report->cloud_stream.for_sale_category->products_read, 5U);
  EXPECT_EQ(report->cloud_stream.for_sale_category->server_total, 5U);
  EXPECT_TRUE(report->cloud_stream.for_sale_category->complete);
  EXPECT_EQ(report->cloud_stream.for_sale_category->fetched_at, rig.now());
  EXPECT_TRUE(rig.has_record("for_sale/en-gb"));
  for (const auto& request : rig.http.seen()) {
    if (request.url.find(store_grid) == std::string::npos) continue;
    EXPECT_NE(request.url.find("country=GB&language=en&age=34&limit=100&"
                               "offset=0&"),
              std::string::npos)
        << request.url;
    for (const auto& header : request.headers) {
      EXPECT_NE(header.name, "Authorization");
      EXPECT_NE(header.name, "Cookie");
    }
  }
}

TEST_F(ForSale, APremiumOrOwnedGameKeepsItsRouteByEntitlement) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  store_sells(
      rig,
      {sale_offer("EP0109-PPSA07028_00-FAKE0109TITLE000", "Rivet Deluxe"),
       R"({"id":"EP-PPSA10001_00-ASTRODELUXE00000","name":"Comet Deluxe","skus":[{"id":"EP-PPSA10001_00-ASTRODELUXE00000-E001","entitlements":[{"id":"EP-PPSA10001_00-GAME","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true}]}]})",
       sale_offer(for_sale_only, "Buy To Stream")});
  library_holds(
      rig,
      owned_page(
          {R"({"id":"EP0109-PPSA07028_00-FAKE0109TITLE000","productId":"EP0109-PPSA07028_00-FAKE0109TITLE000","skuId":"EP0109-PPSA07028_00-FAKE0109TITLE000-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Rivet Deluxe","packageType":"PSGD"},"streamingSupported":true})"}));
  const auto games = loaded(rig, *user);
  EXPECT_EQ(game_of(*games, "EP0109-PPSA07028_00-FAKE0109TITLE000").play_path(),
            PlayPath::owned);
  EXPECT_EQ(games->find(product("EP-PPSA10001_00-ASTRODELUXE00000")), nullptr);
  EXPECT_EQ(game_of(*games, comet).play_path(), PlayPath::included);
  EXPECT_EQ(game_of(*games, for_sale_only).play_path(), PlayPath::store_only);
  const auto store_rows =
      std::ranges::count_if(games->visible(), [](const catalog::MyGame& game) {
        return game.play_path() == PlayPath::store_only;
      });
  EXPECT_EQ(store_rows, 1);
}

TEST_F(ForSale, AFailedStoreCategoryListsNothingForSaleAndKeepsTheRest) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.on("GET", std::string{store_grid}, {{500, ""}});
  const auto games = loaded(rig, *user);
  EXPECT_GE(rig.http.count(store_grid), 1);
  EXPECT_EQ(game_of(*games, rivet).play_path(), PlayPath::owned);
  EXPECT_EQ(game_of(*games, comet).play_path(), PlayPath::included);
  EXPECT_TRUE(game_of(*games, classic).is_visible());
  EXPECT_TRUE(
      std::ranges::none_of(games->visible(), [](const catalog::MyGame& game) {
        return game.play_path() == PlayPath::store_only;
      }));
  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  EXPECT_FALSE(report->cloud_stream.for_sale_category);
  EXPECT_TRUE(report->cloud_stream.premium_category);
  EXPECT_TRUE(rig.has_record("for_sale/en-gb"));
}

TEST_F(ForSale, NothingIsListedWhilePs5OwnershipIsUnknown) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  store_sells(rig, {sale_offer(for_sale_only, "Buy To Stream")});
  rig.http.on("GET", std::string{ps5_owned_list}, {{500, ""}});
  const auto games = loaded(rig, *user);
  EXPECT_EQ(games->find(product(for_sale_only)), nullptr);
}

}  // namespace
}  // namespace catalog_api_test
