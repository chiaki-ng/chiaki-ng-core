#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <utility>

namespace catalog_api_test {
namespace {

using RefusalMemory = CatalogApiTest;

TEST_F(RefusalMemory, HoldsARefusalUntilItExpires) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, rivet, streams_notice);
  (void)loaded(rig, *user);
  const auto refused_at = rig.now();
  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_TRUE(rig.has_record("user/1/refusals"));

  const auto games = loaded(rig, *user);
  const auto& remembered = game_of(*games, rivet).remembered_refusal();
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered->refused_at(), refused_at);
  EXPECT_EQ(remembered->account(), AccountId{1});
  EXPECT_EQ(remembered->kind(), StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_FALSE(game_of(*games, comet).remembered_refusal());
  EXPECT_TRUE(game_of(*games, rivet).is_visible());
  const auto held = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(held), LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(held.refusal()->time(), refused_at);
  const auto asked = rig.http.requests();
  const auto cleared = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_EQ(refusal_kind(cleared), LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(rig.http.requests(), asked);

  auto restarted_catalog = restarted(rig);
  EXPECT_EQ(
      refusal_kind(restarted_catalog.clear_for_launch(*user, product(rivet))),
      LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(rig.http.requests(), asked);

  rig.advance(std::chrono::hours{24} - std::chrono::milliseconds{1});
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  rig.advance(std::chrono::milliseconds{1});
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_FALSE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_FALSE(rig.has_record("user/1/refusals"));
  rig.hold_exclusions();
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).is_allowed());
}

TEST_F(RefusalMemory, ForgetsARefusalWhenTheTierOrTheTitlesOwnershipChanges) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  (void)loaded(rig, *user);
  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  EXPECT_TRUE(user->get_my_membership());
  EXPECT_FALSE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  EXPECT_FALSE(rig.has_record("user/1/refusals"));

  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  rig.advance(std::chrono::minutes{5});
  rig.catalog.start_over(*user);
  const auto refreshed = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_TRUE(game_of(*refreshed, rivet).remembered_refusal());
  EXPECT_TRUE(rig.has_record("user/1/refusals"));

  rig.catalog.remember_refusal(*user, product(comet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_TRUE(game_of(*loaded(rig, *user), comet).remembered_refusal());
  rig.advance(std::chrono::minutes{5});
  rig.http.before_everything("GET",
                             "internal/entitlements/EP-PPSA10001_00-GAME",
                             {{200, comet_owned}});
  const auto rechecked = rig.catalog.recheck_ownership(*user, product(comet));
  ASSERT_TRUE(rechecked);
  EXPECT_TRUE(rechecked.value().is_owned());
  EXPECT_FALSE(game_of(*loaded(rig, *user), comet).remembered_refusal());
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
}

TEST_F(RefusalMemory, OutlivesTheLibraryRefetchItCauses) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  EXPECT_TRUE(user->get_my_membership());
  (void)loaded(rig, *user);
  const auto refused_at = rig.now();
  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_TRUE(rig.has_record("user/1/refusals"));
  EXPECT_FALSE(rig.has_record("user/1/library"));

  rig.advance(std::chrono::minutes{1});
  const auto refetched = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(refetched->inputs().library_fetched_at, rig.now());
  const auto& remembered = game_of(*refetched, rivet).remembered_refusal();
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered->kind(), StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_EQ(remembered->refused_at(), refused_at);
  EXPECT_EQ(remembered->circumstances().tier, Tier::premium);
  ASSERT_TRUE(remembered->circumstances().holding.id());
  EXPECT_EQ(remembered->circumstances().holding.id()->text(), rivet);
  EXPECT_TRUE(game_of(*refetched, rivet).is_owned());
  EXPECT_TRUE(rig.has_record("user/1/refusals"));
  const auto held = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(held), LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(held.refusal()->time(), refused_at);

  rig.advance(std::chrono::minutes{1});
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  EXPECT_EQ(rig.http.count(owned_list), 2);
  auto restarted_catalog = restarted(rig);
  EXPECT_TRUE(
      game_of(*restarted_catalog.process_library_and_list(*user).value(), rivet)
          .remembered_refusal());
  EXPECT_EQ(
      refusal_kind(restarted_catalog.clear_for_launch(*user, product(rivet))),
      LaunchRefusal::Kind::refused_by_service);

  const auto other = rig.user(2, users::Region{"GB", "en"});
  const auto theirs = loaded(rig, *other);
  EXPECT_TRUE(game_of(*theirs, rivet).is_owned());
  EXPECT_FALSE(game_of(*theirs, rivet).remembered_refusal());
  EXPECT_FALSE(rig.has_record("user/2/refusals"));
  EXPECT_TRUE(rig.catalog.can_launch(*other, product(rivet)).is_allowed());

  rig.advance(std::chrono::hours{24} - std::chrono::minutes{2} -
              std::chrono::milliseconds{1});
  EXPECT_TRUE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  rig.advance(std::chrono::milliseconds{1});
  EXPECT_FALSE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  EXPECT_FALSE(rig.has_record("user/1/refusals"));
}

TEST_F(RefusalMemory, NoGameForTheEntitlementQuestionsTheLibrary) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  (void)loaded(rig, *user);
  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_TRUE(rig.has_record("user/1/refusals"));
  EXPECT_FALSE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));
  rig.advance(std::chrono::minutes{1});
  library_pages(rig, {{200, owned_page({})}});
  const auto games = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(rig.http.count(ps5_owned_list), 2);
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(games->find(product(rivet)), nullptr);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_listed);
}

TEST_F(RefusalMemory, KeepsRefusalsHeldUnderAKnownTierWhileTheTierIsUnknown) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  EXPECT_TRUE(user->get_my_membership());
  (void)loaded(rig, *user);
  rig.catalog.remember_refusal(*user, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  const auto known_games = loaded(rig, *user);
  const auto& known = game_of(*known_games, rivet).remembered_refusal();
  ASSERT_TRUE(known);
  EXPECT_EQ(known->circumstances().tier, Tier::premium);

  test_support::users::SignedIn next_run{rig.http, rig.clock, rig.wall};
  auto saved = rig.saved(1);
  saved.region = users::Region{"GB", "en"};
  const auto returning = next_run.as(std::move(saved));
  EXPECT_FALSE(returning->membership().tier());
  auto restarted_catalog = restarted(rig);
  const auto asked = rig.http.requests();
  const auto games = restarted_catalog.process_library_and_list(*returning);
  ASSERT_TRUE(games);
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_FALSE(games.value()->inputs().tier);
  EXPECT_TRUE(game_of(*games.value(), rivet).remembered_refusal());
  EXPECT_TRUE(rig.has_record("user/1/refusals"));
  EXPECT_EQ(
      refusal_kind(restarted_catalog.can_launch(*returning, product(rivet))),
      LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(refusal_kind(
                restarted_catalog.clear_for_launch(*returning, product(rivet))),
            LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(rig.http.requests(), asked + 1);

  EXPECT_TRUE(returning->get_my_membership());
  EXPECT_TRUE(
      game_of(*restarted_catalog.process_library_and_list(*returning).value(),
              rivet)
          .remembered_refusal());
}

}  // namespace
}  // namespace catalog_api_test
