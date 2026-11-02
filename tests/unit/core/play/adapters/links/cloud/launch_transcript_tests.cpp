#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/play/adapters/links/cloud/launch_transcript_rig.hpp"
#include "tests/unit/core/play/cleared_launch.hpp"
#include "tests/unit/core/play/takion_defaults.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::Title;
namespace {

using cloud_transcripts::TranscriptPs;
using cloud_transcripts::TranscriptRig;

class CloudLaunchTranscript : public ::testing::Test {
 protected:
  static constexpr std::string_view death_haulage =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";

  void SetUp() override {
    if (!TakionDefaults::cloud())
      GTEST_SKIP() << "the build pins non-default cloud Takion versions";
  }

  static auto golden(std::string_view name) -> std::string {
    return "cloud_launch_" + std::string{name};
  }

  static auto subscriptions() -> std::pair<std::string, std::string> {
    return {"operationName=getSubscriptions&",
            test_support::psn::Fixtures::text("get_subscriptions.json")};
  }

  static auto death_haulage_concept() -> std::pair<std::string, std::string> {
    return {"operationName=metGetConceptByProductIdQuery&",
            test_support::psn::Fixtures::text("product_FAKE0030TITLE000.json")};
  }

  static auto cleared_as(std::string_view product)
      -> libchiaking::catalog::ClearedLaunch {
    return Title{.product = product, .held_as = product, .store = "en-GB"}
        .cleared();
  }

  static auto asked_the_catalog(const TranscriptPs& ps) -> bool {
    return ps.asked_for("/premium/features/cloudgame") ||
           ps.asked_for("/internal/entitlements") ||
           ps.asked_for("/api/catalog/v2/products/cloudgame") ||
           ps.asked_for("/portal/np/portal-config.json");
  }
};

TEST_F(CloudLaunchTranscript, MatchesLegacyCloudStreamResolvedTitle) {
  TranscriptRig rig{golden("legacy_cloud_stream_resolved_title")};
  rig.ps().graphql_replies = {subscriptions()};
  rig.hold("en-GB", {.product = "UP0114-CUSA00001_00-FAKE0102TITLE000",
                     .old_service = true});
  auto config = rig.configuration("UP0114-CUSA00001_00-FAKE0102TITLE000");
  config.user = rig.signed_in_user();
  EXPECT_TRUE(rig.launch(std::move(config)));
}

TEST_F(CloudLaunchTranscript, MatchesCloudStreamOwnedTitle) {
  TranscriptRig rig{golden("cloud_stream_owned_title")};
  rig.ps().graphql_replies = {subscriptions(), death_haulage_concept()};
  rig.hold("en-GB", {.product = death_haulage, .owned = true});
  auto config = rig.configuration(death_haulage);
  config.user = rig.signed_in_user("GB", true);
  EXPECT_TRUE(rig.launch(std::move(config)));
}

TEST_F(CloudLaunchTranscript, MatchesCloudStreamBusyLockAndEntitlementRetry) {
  TranscriptRig rig{golden("cloud_stream_busy_lock_and_entitlement_retry")};
  rig.ps().no_game_refusals = 1;
  rig.ps().busy_locks = 1;
  ClearedLaunch cleared;
  auto config = rig.configuration("synthetic-entitlement");
  config.user = rig.signed_in_user();
  ASSERT_TRUE(rig.launch(std::move(config), cleared.seams()));
  EXPECT_EQ(cleared.clearance.clears, 1);
  EXPECT_TRUE(cleared.clearance.remembered.empty());
  EXPECT_EQ(rig.waiter().waits, std::vector<int>{1000});
}

TEST_F(CloudLaunchTranscript, MatchesCloudStreamClearedTitle) {
  TranscriptRig rig{golden("cloud_stream_cleared_title")};
  rig.hold("en-GB", {.product = death_haulage, .owned = true});
  auto config = rig.configuration(death_haulage);
  config.user = rig.signed_in_user("GB", true);
  config.cleared = cleared_as(death_haulage);
  ASSERT_TRUE(rig.launch(std::move(config)));
  EXPECT_FALSE(asked_the_catalog(rig.ps()));
  EXPECT_TRUE(rig.ps().asked_for("/allocate"));
}

TEST_F(CloudLaunchTranscript, MatchesCloudStreamMismatchedClearedTitle) {
  TranscriptRig rig{golden("cloud_stream_mismatched_cleared_title")};
  rig.ps().graphql_replies = {subscriptions(), death_haulage_concept()};
  rig.hold("en-GB", {.product = death_haulage, .owned = true});
  auto config = rig.configuration(death_haulage);
  config.user = rig.signed_in_user("GB", true);
  config.cleared = cleared_as("EP0109-PPSA07028_00-FAKE0110TITLE000");
  ASSERT_TRUE(rig.launch(std::move(config)));
  EXPECT_TRUE(rig.ps().asked_for("/premium/features/cloudgame"));
  EXPECT_TRUE(rig.ps().asked_for(
      "/internal/entitlements/EP0109-PPSA07033_00-FAKE0030TITLE000"));
}

}  // namespace
}  // namespace play_test
