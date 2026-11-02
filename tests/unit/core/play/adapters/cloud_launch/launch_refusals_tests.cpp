#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace play_test {

using test_support::play::LaunchCase;
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using cloud_launches::Launching;

class CloudLaunchRefusals : public cloud_launches::LaunchTest {
 protected:
  auto trial_case() const -> LaunchCase {
    auto launched = ps5_case();
    launched.title.trial = true;
    return launched;
  }
};

TEST_F(CloudLaunchRefusals, LogsARefusedTrialLaunchWithThePsReply) {
  Launching trial{trial_case()};
  trial.ps().other_refusals = 1;
  EXPECT_FALSE(trial.core().run());
  EXPECT_TRUE(noted(trial,
                    "[TRIAL] launch refused after the launch checks for "
                    "synthetic-entitlement: "));
  EXPECT_TRUE(
      noted(trial,
            "; stage start reason rejected detail 3 HTTP 400; not remembered"));
  EXPECT_TRUE(noted(trial, R"(; PS replied: {"name":"somethingElse"})"));
}

TEST_F(CloudLaunchRefusals, LogsThatARefusedTrialLaunchWasRemembered) {
  Launching trial{trial_case()};
  trial.ps().no_game_refusals = 2;
  EXPECT_FALSE(trial.core().run());
  EXPECT_TRUE(noted(trial, "reason entitlement_retry"));
  EXPECT_TRUE(noted(trial, "; remembered as no_game_for_entitlement"));
}

TEST_F(CloudLaunchRefusals, DoesNotLogAFullGameAsATrial) {
  Launching full_game{ps5_case()};
  full_game.ps().other_refusals = 1;
  EXPECT_FALSE(full_game.core().run());
  EXPECT_FALSE(noted(full_game, "[TRIAL]"));
}

TEST_F(CloudLaunchRefusals, ReportsANoGameRefusalToTheCatalogOnce) {
  Launching refused{ps5_case()};
  refused.ps().no_game_refusals = 2;
  const auto slot = refused.core().run();
  ASSERT_FALSE(slot);
  EXPECT_EQ(refused.ps().starts, 2U);
  EXPECT_EQ(refused.clearance().clears, 1);
  ASSERT_EQ(refused.clearance().remembered.size(), 1U);
  EXPECT_EQ(refused.clearance().remembered.front().account,
            player().user->account_id());
  EXPECT_EQ(refused.clearance().remembered.front().product,
            "synthetic-entitlement");
  EXPECT_EQ(refused.clearance().remembered.front().kind,
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
  const auto typed = refused.core().refusal();
  ASSERT_TRUE(typed);
  EXPECT_EQ(typed->kind(), Kind::refused_by_service);
  EXPECT_TRUE(typed->is_reported_to_catalog());
  EXPECT_EQ(typed->ps_refusal(),
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
  const play::CloudLaunch handle{refused.core()};
  ASSERT_TRUE(handle.refusal());
  EXPECT_EQ(handle.refusal()->kind(), Kind::refused_by_service);
  const auto detail = play::LinkFailure::unlaunched(slot.error());
  EXPECT_EQ(detail.stage(), remote_play::FailureStage::cloud_start);
  EXPECT_EQ(detail.message(), R"({"name":"noGameForEntitlementId"})");
}

TEST_F(CloudLaunchRefusals, ReportsNothingWhenARetryStarts) {
  Launching retried{ps5_case()};
  retried.ps().no_game_refusals = 1;
  EXPECT_TRUE(retried.core().run());
  EXPECT_EQ(retried.ps().starts, 2U);
  EXPECT_TRUE(retried.clearance().remembered.empty());
  EXPECT_FALSE(retried.core().refusal());
}

TEST_F(CloudLaunchRefusals, ReportsNoOtherRefusalToTheCatalog) {
  Launching other{ps5_case()};
  other.ps().other_refusals = 1;
  const auto slot = other.core().run();
  EXPECT_FALSE(slot);
  EXPECT_EQ(other.ps().starts, 1U);
  EXPECT_TRUE(other.clearance().remembered.empty());
  ASSERT_TRUE(other.core().refusal());
  EXPECT_EQ(other.core().refusal()->kind(), Kind::unavailable);
  EXPECT_EQ(other.core().refusal()->http_status(), 400);
}

TEST_F(CloudLaunchRefusals, ReportsAResolvedOldServiceTitleWithoutRetrying) {
  Launching resolved{old_service_case()};
  resolved.ps().no_game_refusals = 1;
  const auto slot = resolved.core().run();
  EXPECT_FALSE(slot);
  EXPECT_EQ(resolved.ps().starts, 1U);
  EXPECT_EQ(resolved.ps().lookups, 1U);
  EXPECT_EQ(resolved.clearance().remembered.size(), 1U);
  ASSERT_TRUE(resolved.core().refusal());
  EXPECT_EQ(resolved.core().refusal()->kind(), Kind::refused_by_service);
}

TEST_F(CloudLaunchRefusals, RetriesAHeldOldServiceTitleAfterResolvingIt) {
  Launching known{old_service_case("OWNED")};
  known.ps().no_game_refusals = 1;
  EXPECT_TRUE(known.core().run());
  EXPECT_EQ(known.ps().starts, 2U);
  EXPECT_EQ(known.ps().lookups, 1U);
  EXPECT_TRUE(known.clearance().remembered.empty());
}

TEST_F(CloudLaunchRefusals, ReportsAHeldOldServiceTitleRefusedTwice) {
  Launching twice{old_service_case("OWNED")};
  twice.ps().no_game_refusals = 2;
  EXPECT_FALSE(twice.core().run());
  EXPECT_EQ(twice.ps().starts, 2U);
  EXPECT_EQ(twice.ps().lookups, 1U);
  EXPECT_EQ(twice.clearance().remembered.size(), 1U);
}

TEST_F(CloudLaunchRefusals, StopsAtAStorefrontRefusalWithoutReportingIt) {
  for (const auto& refusal :
       {catalog::LaunchRefusal::not_streamable("Not on PS5"),
        catalog::LaunchRefusal::needs_premium(libchiaking::users::Tier::extra),
        catalog::LaunchRefusal::refused_by_service(
            catalog::LaunchRefusal::Time{}),
        catalog::LaunchRefusal::not_listed()}) {
    SCOPED_TRACE(static_cast<int>(refusal.kind()));
    auto launched = ps5_case();
    launched.refusal = refusal;
    Launching storefront{launched};
    const auto slot = storefront.core().run();
    EXPECT_FALSE(slot);
    EXPECT_EQ(storefront.ps().starts, 0U);
    EXPECT_EQ(storefront.ps().requests, 1U);
    EXPECT_EQ(storefront.clearance().clears, 1);
    EXPECT_TRUE(storefront.clearance().remembered.empty());
    const auto typed = storefront.core().refusal();
    ASSERT_TRUE(typed);
    EXPECT_EQ(typed->kind(), Kind::not_cleared);
    EXPECT_FALSE(typed->is_reported_to_catalog());
    EXPECT_FALSE(typed->ps_refusal());
    ASSERT_TRUE(typed->clearance());
    EXPECT_EQ(typed->clearance()->kind(), refusal.kind());
  }
}

}  // namespace
}  // namespace play_test
