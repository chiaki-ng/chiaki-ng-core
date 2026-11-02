#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

using HeldAnswers = CatalogApiTest;

TEST_F(HeldAnswers, ServeASecondLoadWithoutAskingPs) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  cloud_streaming_allowed(rig);
  const auto first = loaded(rig, *user);
  const auto list = rig.catalog.get_streamable_list(*user);
  const auto asked = rig.http.requests();
  rig.advance(std::chrono::minutes{10});

  const auto second = loaded(rig, *user);
  EXPECT_EQ(second, first);
  const auto again = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(again);
  ASSERT_TRUE(list);
  EXPECT_EQ(again.value(), list.value());
  EXPECT_TRUE(rig.catalog.library_of(*user));
  EXPECT_EQ(rig.http.requests(), asked);

  auto restarted_catalog = restarted(rig);
  const auto reloaded = restarted_catalog.process_library_and_list(*user);
  ASSERT_TRUE(reloaded);
  EXPECT_EQ(rig.http.requests(), asked);
  EXPECT_EQ(reloaded.value()->visible().size(), first->visible().size());
  EXPECT_TRUE(game_of(*reloaded.value(), rivet).is_owned());
  EXPECT_EQ(reloaded.value()->inputs().list_fetched_at,
            first->inputs().list_fetched_at);
  EXPECT_EQ(reloaded.value()->inputs().library_fetched_at,
            first->inputs().library_fetched_at);
}

TEST_F(HeldAnswers, AnswerHeldTrialsFromWhatIsKeptOnDisk) {
  constexpr std::string_view demo = "EP0100-PPSA77002_00-FAKE0031TITLE000";
  const std::string demo_owned =
      R"({"id":"EP0100-PPSA77002_00-FAKE0031TITLE000","productId":"EP0100-PPSA77002_00-FAKE0031TITLE000","skuId":"EP0100-PPSA77002_00-FAKE0031TITLE000-E001","activeFlag":true,"entitlementType":5,"featureType":1,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Demo Game Demo","packageType":"PSGD"},"streamingSupported":true})";
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}),
                  store_list({comet_listed}));
  library_holds(rig, owned_page({rivet_owned, demo_owned}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  cloud_streaming_allowed(rig);
  EXPECT_TRUE(rig.catalog.held_trials(*user).empty());
  const auto first = loaded(rig, *user);
  const auto asked = rig.http.requests();
  const auto trials = rig.catalog.held_trials(*user);
  ASSERT_EQ(trials.size(), 1U);
  EXPECT_EQ(trials.front().product_id(), product(demo));

  auto restarted_catalog = restarted(rig);
  const auto kept = restarted_catalog.held_trials(*user);
  ASSERT_EQ(kept.size(), 1U);
  EXPECT_EQ(kept.front().product_id(), product(demo));
  EXPECT_EQ(rig.http.requests(), asked);
}

TEST_F(HeldAnswers, AreAskedAgainAfterADay) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  const auto first = loaded(rig, *user);
  rig.advance(std::chrono::hours{24});
  EXPECT_EQ(loaded(rig, *user), first);
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(owned_list), 1);

  rig.advance(std::chrono::minutes{1});
  const auto later = loaded(rig, *user);
  EXPECT_NE(later, first);
  EXPECT_EQ(rig.http.count(old_service_stores), 2);
  EXPECT_EQ(rig.http.count(all_ps5_list), 2);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(later->inputs().list_fetched_at, rig.now());
  EXPECT_EQ(later->inputs().library_fetched_at, rig.now());
}

TEST_F(HeldAnswers,
       AreDroppedByStartingOverExceptTheTaggedCategoryInItsCooldown) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.on("GET", std::string{tag_grid},
              {{200, tagged_grid(85, "PPSA10001")}});
  const auto first = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), 1);
  EXPECT_TRUE(rig.has_record("included/en-gb"));
  EXPECT_EQ(first->inputs().included_fetched_at, rig.now());

  rig.advance(std::chrono::minutes{30});
  rig.catalog.start_over(*user);
  EXPECT_FALSE(rig.has_record("streamable/gb-en"));
  EXPECT_FALSE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.has_record("included/en-gb"));
  const auto asked = rig.http.requests();
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(rig.http.requests(), asked);
  const auto again = loaded(rig, *user);
  EXPECT_NE(again, first);
  EXPECT_EQ(rig.http.count(old_service_stores), 2);
  EXPECT_EQ(rig.http.count(all_ps5_list), 2);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(rig.http.count(tag_grid), 1);
  EXPECT_EQ(again->inputs().list_fetched_at, rig.now());

  rig.advance(std::chrono::hours{2});
  (void)loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), 1);
  EXPECT_EQ(rig.http.count(old_service_stores), 2);

  rig.catalog.start_over(*user);
  const auto refreshed = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), 2);
  EXPECT_EQ(rig.http.count(old_service_stores), 3);
  EXPECT_EQ(refreshed->inputs().included_fetched_at, rig.now());
  (void)loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), 2);
  EXPECT_EQ(rig.http.count(old_service_stores), 3);
}

TEST_F(HeldAnswers, ThatCannotBeReadAreAskedOfPsAgain) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  const auto first = loaded(rig, *user);
  const auto name =
      libchiaking::platform::RecordName::parse("streamable/gb-en");
  ASSERT_TRUE(name);
  const std::string_view garbage = "{not a record";
  EXPECT_TRUE(
      rig.records.write_atomic(*name, std::as_bytes(std::span{garbage})));
  auto restarted_catalog = restarted(rig);
  const auto again = restarted_catalog.process_library_and_list(*user);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value()->visible().size(), first->visible().size());
  EXPECT_EQ(rig.http.count(old_service_stores), 2);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));
}

}  // namespace
}  // namespace catalog_api_test
