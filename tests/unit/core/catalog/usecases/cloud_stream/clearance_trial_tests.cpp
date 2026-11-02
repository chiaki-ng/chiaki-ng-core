#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace catalog_test {
namespace {

class CloudStreamClearanceOfTrials : public ::testing::Test,
                                     protected CloudStreamCases {
 protected:
  static auto active_trial() -> std::string {
    return entitlement_reading(
        R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)");
  }
};

TEST_F(CloudStreamClearanceOfTrials, ClearsATrialPsSaysIsActive) {
  CloudStreamLaunch trial;
  trial.rig.ps5_record_answers(200, active_trial());
  const auto verdict = trial.clear(holding_a_trial(), rivet);
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), rivet);
  EXPECT_EQ(trial.rig.http.count(ClearanceRig::ps5_record), 1);
}

TEST_F(CloudStreamClearanceOfTrials, RefusesATrialPsSaysIsInactive) {
  CloudStreamLaunch expired;
  expired.rig.ps5_record_answers(
      200,
      entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)"));
  EXPECT_EQ(kind_of(expired.clear(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_EQ(expired.rig.http.count(ClearanceRig::ps5_record), 1);
  EXPECT_EQ(expired.rig.http.count(ClearanceRig::resolver), 0);
}

TEST_F(CloudStreamClearanceOfTrials,
       RefusesATrialWithoutCloudStreamingBeforeReadingIt) {
  CloudStreamLaunch unsubscribed;
  unsubscribed.rig.cloud_streaming_denied("TIER_20");
  unsubscribed.rig.ps5_record_answers(200, active_trial());
  EXPECT_EQ(kind_of(unsubscribed.clear(holding_a_trial(), rivet)),
            Kind::needs_premium);
  EXPECT_EQ(unsubscribed.rig.http.count(ClearanceRig::ps5_record), 0);
}

TEST_F(CloudStreamClearanceOfTrials,
       ListsAnEndedTrialButRefusesItWhilePsSaysNoTimeIsLeft) {
  CloudStreamLaunch ended;
  const auto held = ended.held(holding_an_ended_trial(), rivet);
  EXPECT_EQ(kind_of(held), Kind::purchase_needed);
  EXPECT_EQ(ended.rig.http.count(ClearanceRig::ps5_record), 0);
  ended.rig.ps5_record_answers(
      200,
      entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":7,"isPlayTimeFullyConsumed":true,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)"));
  EXPECT_EQ(kind_of(ended.clear(holding_an_ended_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_EQ(ended.rig.http.count(ClearanceRig::ps5_record), 1);
  EXPECT_EQ(ended.rig.http.count(ClearanceRig::resolver), 0);
}

TEST_F(CloudStreamClearanceOfTrials, ClearsAnEndedTrialOncePsSaysTimeIsLeft) {
  CloudStreamLaunch renewed;
  renewed.rig.ps5_record_answers(
      200,
      entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":7,"isPlayTimeFullyConsumed":false,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)"));
  const auto verdict = renewed.clear(holding_an_ended_trial(), rivet);
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), rivet);
  EXPECT_EQ(renewed.rig.http.count(ClearanceRig::ps5_record), 1);
}

TEST_F(CloudStreamClearanceOfTrials,
       RemembersATrialPsRefusesAsEndedAndListsItAsEnded) {
  CloudStreamLaunch ended;
  ended.rig.ps5_record_answers(200, trial_reading("false", "true"));
  EXPECT_FALSE(ended.listed_as_ended(holding_a_trial()));
  EXPECT_EQ(kind_of(ended.clear(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_TRUE(
      ended.log.contains("[TRIAL] launch refused by the launch checks for "
                         "EP0109-PPSA07028_00-FAKE0110TITLE000"));
  EXPECT_TRUE(ended.log.contains("at entitlement: purchase_needed"));
  EXPECT_TRUE(
      ended.log.contains("re-read isActive=false streamingSupported=true "
                         "isPlayTimeFullyConsumed=absent featureType=1"));
  EXPECT_TRUE(ended.log.contains("remembered as an ended trial"));
  EXPECT_TRUE(ended.listed_as_ended(holding_a_trial()));
  const auto seen = ended.seen(holding_a_trial());
  const auto* game = seen.games->find(product(rivet));
  ASSERT_NE(game, nullptr);
  const auto& remembered = game->remembered_refusal();
  ASSERT_TRUE(remembered);
  EXPECT_TRUE(remembered->ends_the_trial());
  EXPECT_EQ(kind_of(ended.held(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_EQ(kind_of(ended.clear(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_EQ(ended.rig.http.count(ClearanceRig::ps5_record), 1);
}

TEST_F(CloudStreamClearanceOfTrials, KeepsAnEndedTrialAcrossARestart) {
  CloudStreamLaunch ended;
  ended.rig.ps5_record_answers(200, trial_reading("false", "true"));
  ASSERT_EQ(kind_of(ended.clear(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  libchiaking::catalog::HeldAnswers restarted{
      ended.rig.records, ended.rig.threading, ended.rig.clock, ended.rig.wall};
  const auto stored =
      restarted.refusals().standing_while_membership_is_unknown(account);
  ASSERT_TRUE(stored);
  ASSERT_EQ(stored.value().refusals.size(), 1U);
  EXPECT_EQ(stored.value().refusals.front().kind(),
            libchiaking::catalog::StreamRefusal::Kind::trial_ended);
}

TEST_F(CloudStreamClearanceOfTrials, ListsATrialTakenDownWhileInactiveAsEnded) {
  CloudStreamLaunch taken_down;
  taken_down.rig.ps5_record_answers(200, trial_reading("false", "false"));
  EXPECT_EQ(kind_of(taken_down.clear(holding_a_trial(), rivet)),
            Kind::title_taken_down);
  EXPECT_TRUE(taken_down.listed_as_ended(holding_a_trial()));
}

TEST_F(CloudStreamClearanceOfTrials,
       DoesNotRememberATrialAsEndedWhenPsCannotBeRead) {
  CloudStreamLaunch unreachable;
  unreachable.rig.ps5_record_answers(503, "{}");
  EXPECT_EQ(kind_of(unreachable.clear(holding_a_trial(), rivet)),
            Kind::check_unavailable);
  EXPECT_TRUE(
      unreachable.log.contains("[TRIAL] launch refused by the launch checks"));
  EXPECT_TRUE(unreachable.log.contains("check_unavailable; unanswered"));
  EXPECT_TRUE(unreachable.log.contains("not remembered as ended"));
  EXPECT_FALSE(unreachable.listed_as_ended(holding_a_trial()));
}

TEST_F(CloudStreamClearanceOfTrials,
       DoesNotRememberAnActiveTrialWithoutStreamingAsEnded) {
  CloudStreamLaunch switched_off;
  switched_off.rig.ps5_record_answers(200, trial_reading("true", "false"));
  EXPECT_EQ(kind_of(switched_off.clear(holding_a_trial(), rivet)),
            Kind::streaming_unavailable);
  EXPECT_FALSE(switched_off.listed_as_ended(holding_a_trial()));
}

TEST_F(CloudStreamClearanceOfTrials,
       DoesNotRememberATrialRefusedForTheMembershipAsEnded) {
  CloudStreamLaunch unsubscribed;
  unsubscribed.rig.cloud_streaming_denied("TIER_20");
  EXPECT_EQ(kind_of(unsubscribed.clear(holding_a_trial(), rivet)),
            Kind::needs_premium);
  EXPECT_TRUE(unsubscribed.log.contains("at cloud_streaming: needs_premium"));
  EXPECT_FALSE(unsubscribed.listed_as_ended(holding_a_trial()));
}

TEST_F(CloudStreamClearanceOfTrials,
       DoesNotRememberAnInactiveFullGameAsAnEndedTrial) {
  CloudStreamLaunch full_game;
  full_game.rig.ps5_record_answers(
      200,
      entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)"));
  EXPECT_EQ(kind_of(full_game.clear(owning_rivet(), rivet)),
            Kind::purchase_needed);
  EXPECT_FALSE(full_game.log.contains("[TRIAL]"));
  const auto seen = full_game.seen(owning_rivet());
  const auto* game = seen.games->find(product(rivet));
  ASSERT_NE(game, nullptr);
  EXPECT_FALSE(game->remembered_refusal());
  EXPECT_EQ(kind_of(full_game.clear(owning_rivet(), rivet)),
            Kind::purchase_needed);
  EXPECT_EQ(full_game.rig.http.count(ClearanceRig::ps5_record), 2);
}

TEST_F(CloudStreamClearanceOfTrials,
       LetsAnEndedTrialRefusalLapseLikeAnyOtherRefusal) {
  CloudStreamLaunch expiring;
  expiring.rig.wall.now_us =
      static_cast<std::uint64_t>(library_fetched_ms) * 1000U;
  expiring.rig.ps5_record_answers(200, trial_reading("false", "true"));
  EXPECT_EQ(kind_of(expiring.clear(holding_a_trial(), rivet)),
            Kind::purchase_needed);
  EXPECT_TRUE(expiring.listed_as_ended(holding_a_trial()));
  expiring.rig.wall.now_us += static_cast<std::uint64_t>(
      std::chrono::microseconds{libchiaking::catalog::StreamRefusal::lifetime}
          .count());
  EXPECT_FALSE(expiring.listed_as_ended(holding_a_trial()));
  expiring.rig.ps5_record_answers(200, trial_reading("true", "true"));
  EXPECT_TRUE(expiring.clear(holding_a_trial(), rivet).is_allowed());
  EXPECT_EQ(expiring.rig.http.count(ClearanceRig::ps5_record), 2);
}

}  // namespace
}  // namespace catalog_test
