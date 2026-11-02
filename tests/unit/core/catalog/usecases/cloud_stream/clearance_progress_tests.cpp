#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace catalog_test {
namespace {

using Check = libchiaking::catalog::LaunchCheck;

class CloudStreamClearanceProgress : public ::testing::Test,
                                     protected CloudStreamCases {};

TEST_F(CloudStreamClearanceProgress, TellsEveryCheckInLaunchOrder) {
  CloudStreamLaunch owned;
  EXPECT_TRUE(owned.clear(owning_rivet(), rivet).is_allowed());
  EXPECT_EQ(owned.checks, (std::vector{Check::cloud_streaming,
                                       Check::entitlement, Check::cloud_game,
                                       Check::exclusions, Check::release}));
}

TEST_F(CloudStreamClearanceProgress, StopsTellingAtARefusedEntitlement) {
  CloudStreamLaunch lapsed;
  lapsed.rig.ps5_record_answers(404, "{}");
  EXPECT_EQ(kind_of(lapsed.clear(owning_rivet(), rivet)), Kind::invalid_title);
  EXPECT_EQ(lapsed.checks,
            (std::vector{Check::cloud_streaming, Check::entitlement}));
}

TEST_F(CloudStreamClearanceProgress, TellsTheOwnershipCheckWhenItIsUnknown) {
  CloudStreamLaunch unknown;
  unknown.rig.ps5_record_answers(200, show_record());
  EXPECT_TRUE(unknown.clear(owning_unknown(), show).is_allowed());
  EXPECT_EQ(
      unknown.checks,
      (std::vector{Check::cloud_streaming, Check::ownership, Check::entitlement,
                   Check::cloud_game, Check::exclusions, Check::release}));
}

TEST_F(CloudStreamClearanceProgress,
       TellsOnlyCloudStreamingForAnUnclaimedGame) {
  CloudStreamLaunch unclaimed;
  EXPECT_EQ(kind_of(unclaimed.clear(owning_nothing(), show)),
            Kind::add_to_library);
  EXPECT_EQ(unclaimed.checks, (std::vector{Check::cloud_streaming}));
}

TEST_F(CloudStreamClearanceProgress, TellsOnlyCloudStreamingWhenItIsDenied) {
  CloudStreamLaunch denied;
  denied.rig.cloud_streaming_denied("TIER_20");
  EXPECT_EQ(kind_of(denied.clear(owning_rivet(), rivet)), Kind::needs_premium);
  EXPECT_EQ(denied.checks, (std::vector{Check::cloud_streaming}));
}

TEST_F(CloudStreamClearanceProgress, SkipsCloudStreamingWhenItIsHandedIn) {
  CloudStreamLaunch handed;
  EXPECT_TRUE(
      handed.clear(owning_rivet(), rivet, streaming_allowed()).is_allowed());
  EXPECT_EQ(handed.checks, (std::vector{Check::entitlement, Check::cloud_game,
                                        Check::exclusions, Check::release}));
}

TEST_F(CloudStreamClearanceProgress, TellsNothingForAnUnlistedTitle) {
  CloudStreamLaunch unlisted;
  EXPECT_EQ(kind_of(unlisted.clear(owning_nothing(), bus)), Kind::not_listed);
  EXPECT_TRUE(unlisted.checks.empty());
}

TEST_F(CloudStreamClearanceProgress, TellsNothingForThePreCheck) {
  CloudStreamLaunch held;
  static_cast<void>(held.held(owning_rivet(), rivet));
  EXPECT_TRUE(held.checks.empty());
}

TEST_F(CloudStreamClearanceProgress, TellsNothingWithoutAnObserver) {
  CloudStreamLaunch unwatched;
  const auto unobserved =
      unwatched.clearance.clear(*unwatched.user, unwatched.seen(owning_rivet()),
                                product(rivet), std::nullopt, {});
  EXPECT_TRUE(unobserved.is_allowed());
  EXPECT_TRUE(unwatched.checks.empty());
}

}  // namespace
}  // namespace catalog_test
