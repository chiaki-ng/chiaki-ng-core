#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

using CloudStreaming = CatalogApiTest;

TEST_F(CloudStreaming,
       RefusedHidesPs5TitlesWhileATierHidesOnlyLegacyCloudStream) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  storefront_says(rig, rivet, streams_notice);
  const auto unknown = loaded(rig, *user);
  EXPECT_FALSE(unknown->inputs().tier);
  EXPECT_EQ(shown(*unknown), 3U);
  EXPECT_EQ(rig.http.count(cloud_benefits), 1);

  membership_is(rig, "TIER_20");
  EXPECT_TRUE(user->get_my_membership());
  rig.advance(std::chrono::seconds{31});
  const auto asked = rig.http.requests();
  const auto extra = loaded(rig, *user);
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);
  EXPECT_EQ(extra->inputs().tier, Tier::extra);
  EXPECT_EQ(shown(*extra), 2U);
  EXPECT_EQ(game_of(*extra, classic).visibility().hide_reason(),
            HideReason::tier_does_not_include);
  EXPECT_TRUE(game_of(*extra, rivet).is_visible());
  EXPECT_TRUE(game_of(*extra, comet).is_visible());

  cloud_streaming_denied(rig, "TIER_20");
  const auto refused = rig.catalog.clear_for_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(refused), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(refused.refusal()->tier_held(), Tier::extra);
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);
  EXPECT_EQ(rig.http.count(plus_services), 1);
  EXPECT_EQ(rig.http.count(store_app), 0);
  const auto hidden = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);
  EXPECT_TRUE(hidden->visible().empty());
  EXPECT_EQ(held_back(*hidden), 3U);
  for (const auto& game : hidden->hidden())
    EXPECT_EQ(game.visibility().hide_reason(),
              HideReason::tier_does_not_include);
  const auto held = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(held), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(held.refusal()->tier_held(), Tier::extra);
  EXPECT_EQ(rig.catalog.explain(*user, product(rivet)).hide_reason(),
            HideReason::tier_does_not_include);
  const auto again = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_EQ(refusal_kind(again), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(rig.http.count(cloud_benefits), 4);
  EXPECT_EQ(rig.http.count(store_app), 0);

  cloud_streaming_allowed(rig);
  const auto cleared = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_TRUE(cleared.is_allowed());
  ASSERT_TRUE(cleared.cleared_launch());
  EXPECT_EQ(cleared.cleared_launch()->product, product(rivet));
  EXPECT_EQ(rig.http.count(cloud_benefits), 5);
  EXPECT_EQ(rig.http.count(subscriptions), 1);
  const auto visible = loaded(rig, *user);
  EXPECT_EQ(shown(*visible), 2U);
  EXPECT_TRUE(game_of(*visible, rivet).is_visible());
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(classic))),
            LaunchRefusal::Kind::hidden);
}

TEST_F(CloudStreaming, APremiumTierShowsEveryTitle) {
  Rig premium;
  const auto member = premium.user();
  ps_as_usual(premium);
  membership_is(premium, "TIER_30");
  EXPECT_TRUE(member->get_my_membership());
  const auto all = loaded(premium, *member);
  EXPECT_EQ(all->inputs().tier, Tier::premium);
  EXPECT_EQ(shown(*all), 3U);
}

TEST_F(CloudStreaming, IsAskedForByAPs5LaunchWhichNeverAsksForTheTier) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_10");
  cloud_streaming_denied(rig, "TIER_10");
  storefront_says(rig, rivet, streams_notice);
  (void)loaded(rig, *user);
  const auto verdict = rig.catalog.clear_for_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::needs_premium);
  EXPECT_FALSE(verdict.refusal()->tier_held());
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);
  EXPECT_EQ(rig.http.count(plus_services), 2);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_FALSE(game_of(*loaded(rig, *user), rivet).remembered_refusal());
  EXPECT_FALSE(rig.has_record("user/1/refusals"));
  const auto asked = rig.http.requests();
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(rig.http.requests(), asked);
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(rivet))),
            LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(store_app), 0);
}

TEST_F(CloudStreaming, ThatCannotBeAskedLeavesAPs5LaunchUncheckedButListed) {
  Rig offline;
  const auto unsure = offline.user();
  ps_as_usual(offline);
  membership_is(offline, "TIER_30");
  EXPECT_TRUE(unsure->get_my_membership());
  offline.http.on("GET", std::string{cloud_benefits}, {{500, ""}});
  storefront_says(offline, rivet, streams_notice);
  (void)loaded(offline, *unsure);
  const auto unchecked =
      offline.catalog.clear_for_launch(*unsure, product(rivet));
  ASSERT_EQ(refusal_kind(unchecked), LaunchRefusal::Kind::check_unavailable);
  EXPECT_TRUE(unchecked.refusal()->cause());
  EXPECT_EQ(offline.http.count(store_app), 0);
  EXPECT_TRUE(game_of(*loaded(offline, *unsure), rivet).is_visible());
}

TEST_F(CloudStreaming, ThatWasRefusedIsAskedAgainAtLaunch) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  membership_is(rig, "TIER_10");
  cloud_streaming_denied(rig, "TIER_10");
  storefront_says(rig, rivet, streams_notice);
  EXPECT_TRUE(user->get_my_membership());
  EXPECT_EQ(user->membership().tier(), Tier::essential);
  EXPECT_TRUE(loaded(rig, *user)->visible().empty());
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(rivet))),
            LaunchRefusal::Kind::needs_premium);
  EXPECT_TRUE(loaded(rig, *user)->visible().empty());
  EXPECT_EQ(rig.http.count(subscriptions), 1);
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);

  const auto asked = rig.http.requests();
  const auto offline = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(offline), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(offline.refusal()->tier_held(), Tier::essential);
  EXPECT_EQ(rig.http.requests(), asked);

  const auto still = rig.catalog.clear_for_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(still), LaunchRefusal::Kind::needs_premium);
  EXPECT_FALSE(still.cleared_launch());
  EXPECT_EQ(still.refusal()->tier_held(), Tier::essential);
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(classic))),
            LaunchRefusal::Kind::hidden);
  EXPECT_EQ(rig.http.count(subscriptions), 2);
  EXPECT_EQ(rig.http.count(cloud_benefits), 3);

  rig.http.hold(std::string{cloud_benefits});
  auto first = offline;
  auto second = offline;
  auto launching = started(
      [&] { first = rig.catalog.clear_for_launch(*user, product(rivet)); });
  rig.http.wait_until_held();
  auto also_launching = started(
      [&] { second = rig.catalog.clear_for_launch(*user, product(rivet)); });
  pause_a_moment();
  rig.http.release();
  EXPECT_TRUE(launching.join());
  EXPECT_TRUE(also_launching.join());
  EXPECT_EQ(refusal_kind(first), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(refusal_kind(second), LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(rig.http.count(cloud_benefits), 4);

  rig.http.on("GET", std::string{cloud_benefits}, {{500, ""}});
  const auto unreachable = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_EQ(refusal_kind(unreachable), LaunchRefusal::Kind::check_unavailable);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::needs_premium);
  const auto failed = rig.http.count(cloud_benefits);
  EXPECT_GT(failed, 4);

  cloud_streaming_allowed(rig);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(rig.http.count(cloud_benefits), failed);
  const auto allowed = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_TRUE(allowed.is_allowed());
  ASSERT_TRUE(allowed.cleared_launch());
  EXPECT_EQ(allowed.cleared_launch()->product, product(rivet));
  EXPECT_EQ(rig.http.count(cloud_benefits), failed + 1);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(user->membership().tier(), Tier::essential);
  EXPECT_EQ(shown(*loaded(rig, *user)), 2U);
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).is_allowed());
  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_EQ(rig.http.count(cloud_benefits), failed + 2);
  EXPECT_EQ(rig.http.count(store_app), 0);
}

struct MembershipShape {
  std::string_view label;
  std::string reply;
  std::optional<Tier> tier;
};

class MembershipShapes : public CatalogApiTest,
                         public testing::WithParamInterface<MembershipShape> {};

TEST_P(MembershipShapes, DecideALegacyCloudStreamLaunch) {
  const auto& shape = GetParam();
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.on("GET", std::string{subscriptions}, {{200, shape.reply}});
  storefront_says(rig, rivet, streams_notice);
  (void)loaded(rig, *user);
  const auto benefits_asked = rig.http.count(cloud_benefits);
  const auto verdict = rig.catalog.clear_for_launch(*user, product(classic));
  EXPECT_FALSE(verdict.is_allowed());
  EXPECT_FALSE(verdict.cleared_launch());
  EXPECT_EQ(rig.http.count(subscriptions), 1);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_EQ(rig.http.count(cloud_benefits), benefits_asked);
  if (shape.tier) {
    ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::needs_premium);
    EXPECT_EQ(verdict.refusal()->tier_held(), *shape.tier);
  } else {
    ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::check_unavailable);
    ASSERT_TRUE(verdict.refusal()->cause());
    EXPECT_FALSE(verdict.refusal()->cause()->is_cancelled());
  }
  EXPECT_FALSE(rig.has_record("user/1/refusals"));
}

TEST_P(MembershipShapes, NeverDecideAnAllowedPs5Launch) {
  const auto& shape = GetParam();
  Rig streaming;
  const auto streamer = streaming.user();
  ps_as_usual(streaming);
  streaming.http.on("GET", std::string{subscriptions}, {{200, shape.reply}});
  cloud_streaming_allowed(streaming);
  storefront_says(streaming, rivet, streams_notice);
  (void)loaded(streaming, *streamer);
  EXPECT_TRUE(streaming.catalog.clear_for_launch(*streamer, product(rivet))
                  .is_allowed());
  EXPECT_EQ(streaming.http.count(subscriptions), 0);
  EXPECT_EQ(streaming.http.count(cloud_benefits), 2);
}

TEST_P(MembershipShapes, NeverDecideADeniedPs5Launch) {
  const auto& shape = GetParam();
  Rig denied;
  const auto refused = denied.user();
  ps_as_usual(denied);
  denied.http.on("GET", std::string{subscriptions}, {{200, shape.reply}});
  cloud_streaming_denied(denied, "TIER_30");
  (void)loaded(denied, *refused);
  const auto blocked =
      denied.catalog.clear_for_launch(*refused, product(rivet));
  ASSERT_EQ(refusal_kind(blocked), LaunchRefusal::Kind::needs_premium);
  EXPECT_FALSE(blocked.refusal()->tier_held());
  EXPECT_EQ(denied.http.count(subscriptions), 0);
  EXPECT_EQ(denied.http.count(store_app), 0);
}

INSTANTIATE_TEST_SUITE_P(
    EveryShape, MembershipShapes,
    testing::Values(
        MembershipShape{
            "Extra",
            PsReplies::subscription_under("TIER_20",
                                          "IP9101-NPIA90006_00-PLUS2T03M-E008"),
            Tier::extra},
        MembershipShape{
            "Essential",
            PsReplies::subscription_under("TIER_10",
                                          "IP9101-NPIA90006_00-PLUS1T03M-E004"),
            Tier::essential},
        MembershipShape{
            "NoSubscription",
            R"({"data":{"fetchSubscriptions":{"subscriptions":[]}}})",
            Tier::none},
        MembershipShape{
            "PremiumUnderAnExtraSku",
            PsReplies::subscription_under("TIER_30",
                                          "IP9101-NPIA90006_00-PLUS2T03M-E008"),
            std::nullopt},
        MembershipShape{"NullSubscriptions",
                        R"({"data":{"fetchSubscriptions":null}})",
                        std::nullopt}),
    [](const testing::TestParamInfo<MembershipShape>& tested) {
      return std::string{tested.param.label};
    });

TEST_F(CloudStreaming,
       AFailingMembershipLeavesALegacyCloudStreamLaunchUnchecked) {
  Rig failing;
  const auto unsure = failing.user();
  ps_as_usual(failing);
  failing.http.on("GET", std::string{subscriptions}, {{500, ""}});
  (void)loaded(failing, *unsure);
  EXPECT_EQ(
      refusal_kind(failing.catalog.clear_for_launch(*unsure, product(classic))),
      LaunchRefusal::Kind::check_unavailable);
  EXPECT_EQ(failing.http.count(subscriptions), 2);
  EXPECT_EQ(failing.http.count(store_app), 0);
}

}  // namespace
}  // namespace catalog_api_test
