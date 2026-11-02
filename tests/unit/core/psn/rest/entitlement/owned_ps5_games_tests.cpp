#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/psn/rest/entitlement/library_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace psn_test {
namespace {

using test_support::psn::Fixtures;

class OwnedPs5Games : public LibraryFixture {};

TEST_F(OwnedPs5Games, ReadTheSyntheticPageFieldByField) {
  rig.fake.on(
      "GET", games_url(0),
      {Reply{200, Fixtures::text("entitlements_v2_psgd_synthetic.json")}});
  const auto found = library.owned_ps5_games();
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().total, 13U);
  EXPECT_EQ(found.value().received, 13U);
  EXPECT_EQ(found.value().requests, 1);
  EXPECT_EQ(found.value().revision_restarts, 0);
  EXPECT_FALSE(found.value().revision_unsettled);
  EXPECT_FALSE(found.value().truncated);
  const auto& read = found.value().entitlements;
  ASSERT_EQ(read.size(), 13U);
  const auto& full = read[0];
  EXPECT_EQ(full.id, "EP0109-PPSA07015_00-FAKE0044TITLE000");
  EXPECT_EQ(full.product_id, "EP0109-PPSA07015_00-FAKE0044TITLE000");
  EXPECT_EQ(full.sku_id, "EP0109-PPSA07015_00-FAKE0044TITLE000-E001");
  EXPECT_EQ(full.active, true);
  EXPECT_EQ(full.active_date, "2026-08-14T00:00:00Z");
  EXPECT_EQ(full.entitlement_type, 5);
  EXPECT_EQ(full.feature_type, 3);
  EXPECT_EQ(full.preorder, false);
  EXPECT_EQ(full.preorder_core, false);
  EXPECT_EQ(full.preorder_placeholder, false);
  EXPECT_EQ(full.streaming_supported, true);
  EXPECT_FALSE(full.playability_date);
  EXPECT_EQ(full.package_type, "PSGD");
  EXPECT_EQ(full.game_name, "Full Game Streams");
  EXPECT_EQ(full.game_icon_url,
            "https://image.api.playstation.com/game/PPSA07015_00/icon0.png");
  EXPECT_EQ(full.title_id, "PPSA07015_00");
  EXPECT_EQ(full.title_name, "Full Game Streams (title)");
  EXPECT_EQ(full.title_image_url,
            "https://image.api.playstation.com/title/PPSA07015_00.png");
  EXPECT_EQ(full.concept_id, "90000001");
  EXPECT_EQ(full.concept_name, "Full Game Streams");
  EXPECT_EQ(full.concept_icon_url,
            "https://image.api.playstation.com/concept/1001.png");
  EXPECT_EQ(full.product_image_url,
            "https://image.api.playstation.com/product/PPSA07015_00.png");
  EXPECT_EQ(read[1].streaming_supported, false);
  EXPECT_FALSE(read[2].streaming_supported);
  EXPECT_EQ(read[3].feature_type, 1);
  EXPECT_EQ(read[4].feature_type, 5);
  EXPECT_EQ(read[5].feature_type, 7);
  EXPECT_EQ(read[6].entitlement_type, 3);
  EXPECT_EQ(read[7].active, false);
  EXPECT_EQ(read[8].preorder, true);
  EXPECT_EQ(read[8].playability_date, "2031-05-01T00:00:00Z");
  EXPECT_EQ(read[8].active_date, "2030-01-01T00:00:00Z");
  EXPECT_EQ(read[9].preorder, false);
  EXPECT_EQ(read[9].preorder_core, true);
  EXPECT_FALSE(read[9].playability_date);
  EXPECT_EQ(read[9].active_date, "2031-06-01T00:00:00Z");
  EXPECT_EQ(read[10].playability_date, "");
  EXPECT_EQ(read[10].active_date, "2031-07-01T00:00:00Z");
  EXPECT_EQ(read[11].preorder_placeholder, true);
  EXPECT_FALSE(read[11].playability_date);
  EXPECT_FALSE(read[11].active_date);
  EXPECT_EQ(read[12].preorder, false);
  EXPECT_EQ(read[12].active_date, "2031-08-01T00:00:00Z");
  EXPECT_TRUE(rig.fake.unmatched.empty());
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  ASSERT_EQ(rig.fake.seen[0].headers.size(), 2U);
  EXPECT_EQ(rig.fake.seen[0].headers[0].value, "Bearer APP-FAKE");
}

TEST_F(OwnedPs5Games, TolerateMissingAndOddFields) {
  rig.fake.on(
      "GET", games_url(0),
      {Reply{
          200,
          R"({"revisionId":"r1","totalResults":4,"entitlements":[)"
          R"({"id":"EP0100-PPSA00001_00-FAKE0083TITLE000","activeFlag":"true","featureType":"3",)"
          R"("streamingSupported":"true","skuMeta":{"playabilityDate":20310101},"conceptMeta":{"conceptId":77}},)"
          R"({"productId":"EP0100-PPSA00002_00-FAKE0075TITLE000"},)"
          R"("not an object",)"
          R"({"id":"EP0100-PPSA00003_00-FAKE0008TITLE000","skuMeta":null,"activeDate":null}]})"}});
  const auto found = library.owned_ps5_games();
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().received, 4U);
  ASSERT_EQ(found.value().entitlements.size(), 2U);
  const auto& odd = found.value().entitlements[0];
  EXPECT_FALSE(odd.active);
  EXPECT_FALSE(odd.feature_type);
  EXPECT_FALSE(odd.streaming_supported);
  EXPECT_EQ(odd.playability_date, "");
  EXPECT_EQ(odd.concept_id, "77");
  EXPECT_TRUE(odd.product_id.empty());
  const auto& bare = found.value().entitlements[1];
  EXPECT_FALSE(bare.playability_date);
  EXPECT_FALSE(bare.active_date);
  EXPECT_TRUE(bare.package_type.empty());
}

TEST_F(OwnedPs5Games, AdvanceByWhatWasReceived) {
  rig.fake.on("GET", games_url(0), {Reply{200, games(250, 0, 100)}});
  rig.fake.on("GET", games_url(100), {Reply{200, games(250, 100, 100)}});
  rig.fake.on("GET", games_url(200), {Reply{200, games(250, 200, 50)}});
  const auto found = library.owned_ps5_games();
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().entitlements.size(), 250U);
  EXPECT_EQ(found.value().requests, 3);
  EXPECT_EQ(found.value().received, 250U);
  EXPECT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{100, 100}));
}

TEST_F(OwnedPs5Games, StopAtAnEmptyPage) {
  rig.fake.on("GET", games_url(0), {Reply{200, games(90, 0, 40)}});
  rig.fake.on("GET", games_url(40), {Reply{200, games(90, 40, 0)}});
  const auto stopped = library.owned_ps5_games();
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped.value().entitlements.size(), 40U);
  EXPECT_EQ(stopped.value().requests, 2);
  EXPECT_FALSE(stopped.value().truncated);
}

TEST_F(OwnedPs5Games, AreTruncatedAtThePageCap) {
  for (std::size_t page = 0; page < 30; ++page)
    rig.fake.on("GET", games_url(page * 100),
                {Reply{200, games(100000, page * 100, 100)}});
  const auto cut = library.owned_ps5_games();
  ASSERT_TRUE(cut);
  EXPECT_TRUE(cut.value().truncated);
  EXPECT_EQ(cut.value().requests, 25);
  EXPECT_EQ(cut.value().entitlements.size(), 2500U);
}

TEST_F(OwnedPs5Games, RestartTheScanWhenTheRevisionChanges) {
  rig.fake.on(
      "GET", games_url(0),
      {Reply{200, games(150, 0, 100, 1)}, Reply{200, games(120, 0, 100, 2)}});
  rig.fake.on(
      "GET", games_url(100),
      {Reply{200, games(150, 100, 50, 2)}, Reply{200, games(120, 100, 20, 2)}});
  const auto found = library.owned_ps5_games();
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().revision_restarts, 1);
  EXPECT_FALSE(found.value().revision_unsettled);
  EXPECT_EQ(found.value().entitlements.size(), 120U);
  EXPECT_EQ(found.value().total, 120U);
  EXPECT_EQ(found.value().received, 120U);
  EXPECT_EQ(found.value().requests, 4);
  EXPECT_EQ(rig.fake.count("offset=0&"), 2);
}

TEST_F(OwnedPs5Games, ArePartialWhenTheRevisionKeepsChanging) {
  rig.fake.on(
      "GET", games_url(0),
      {Reply{200, games(150, 0, 100, 1)}, Reply{200, games(150, 0, 100, 3)},
       Reply{200, games(150, 0, 100, 5)}});
  rig.fake.on(
      "GET", games_url(100),
      {Reply{200, games(150, 100, 50, 2)}, Reply{200, games(150, 100, 50, 4)},
       Reply{200, games(150, 100, 50, 6)}});
  const auto found = library.owned_ps5_games();
  ASSERT_TRUE(found);
  EXPECT_TRUE(found.value().revision_unsettled);
  EXPECT_EQ(found.value().revision_restarts,
            LibraryEntitlements::revision_restart_limit);
  EXPECT_EQ(found.value().entitlements.size(), 100U);
  EXPECT_EQ(found.value().requests, 6);
}

TEST_F(OwnedPs5Games, ReportARefusal) {
  rig.fake.on("GET", games_url(0), {Reply{403, "{}"}});
  const auto failed = library.owned_ps5_games();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 403);
}

TEST_F(OwnedPs5Games, FailWhenALaterPageFails) {
  rig.fake.on("GET", games_url(0), {Reply{200, games(150, 0, 100)}});
  rig.fake.on("GET", games_url(100), {Reply{500, ""}});
  EXPECT_FALSE(library.owned_ps5_games());
}

TEST_F(OwnedPs5Games, ReportAnUnreadablePage) {
  rig.fake.on("GET", games_url(0), {Reply{200, R"({"entitlements":[]})"}});
  const auto unread = library.owned_ps5_games();
  ASSERT_FALSE(unread);
  EXPECT_EQ(unread.error().cause(), libchiaking::psn::rest::Cause::response);
}

}  // namespace
}  // namespace psn_test
