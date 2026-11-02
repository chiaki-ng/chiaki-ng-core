#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::users::Tier;

class CloudStreamClearanceAccess : public ::testing::Test,
                                   protected CloudStreamCases {};

TEST_F(CloudStreamClearanceAccess, IsReadAfreshAndBlocksWhenDenied) {
  for (const auto* tier : {"TIER_10", "TIER_20", "TIER_30", "NONE"}) {
    CloudStreamLaunch denied;
    denied.rig.cloud_streaming_denied(tier);
    const auto refused = denied.clear(owning_rivet(), rivet);
    EXPECT_EQ(kind_of(refused), Kind::needs_premium) << tier;
    EXPECT_FALSE(refused.refusal()->tier_held()) << tier;
    EXPECT_EQ(denied.rig.http.count(ClearanceRig::cloud_benefits), 1) << tier;
    EXPECT_EQ(denied.rig.http.count(ClearanceRig::plus_services), 1) << tier;
    EXPECT_EQ(denied.rig.http.count(ClearanceRig::ps5_record), 0) << tier;
    EXPECT_EQ(denied.rig.http.count(ClearanceRig::resolver), 0) << tier;
    EXPECT_EQ(denied.rig.http.count(ClearanceRig::feature_config), 0) << tier;
  }
}

TEST_F(CloudStreamClearanceAccess, NamesTheTierHeldWhenTheMembershipIsKnown) {
  CloudStreamLaunch worded;
  worded.rig.cloud_streaming_denied("TIER_20");
  worded.rig.membership_is("TIER_20");
  EXPECT_TRUE(worded.user->get_my_membership());
  const auto extra = worded.clear(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(extra), Kind::needs_premium);
  EXPECT_EQ(extra.refusal()->tier_held(), Tier::extra);
}

TEST_F(CloudStreamClearanceAccess, RefusesAnAuthorisationWithoutAPlan) {
  CloudStreamLaunch planless;
  planless.rig.http.on(
      "GET", std::string{ClearanceRig::cloud_benefits},
      {{200,
        R"({"authorization":true,"cloudgameOwnerInfos":[{"benefitPlanIds":[]}]})"}});
  EXPECT_EQ(kind_of(planless.clear(owning_rivet(), rivet)),
            Kind::needs_premium);
  EXPECT_EQ(planless.rig.http.count(ClearanceRig::plus_services), 0);
}

TEST_F(CloudStreamClearanceAccess,
       BlocksAnUnreadableAnswerEvenForAPremiumMember) {
  for (const int status : {500, 401, 200}) {
    CloudStreamLaunch unknown;
    unknown.rig.cloud_streaming_unreadable(status);
    unknown.rig.membership_is("TIER_30");
    EXPECT_TRUE(unknown.user->get_my_membership()) << status;
    EXPECT_EQ(unknown.user->membership().tier(), Tier::premium) << status;
    const auto blocked = unknown.clear(owning_rivet(), rivet);
    EXPECT_EQ(kind_of(blocked), Kind::check_unavailable) << status;
    ASSERT_TRUE(blocked.refusal()->cause()) << status;
    EXPECT_EQ(blocked.refusal()->cause()->reason(), Reason::user) << status;
    EXPECT_EQ(unknown.rig.http.count(ClearanceRig::ps5_record), 0) << status;
    EXPECT_EQ(unknown.rig.http.count(ClearanceRig::plus_services), 0) << status;
  }
}

TEST_F(CloudStreamClearanceAccess, TrustsAnAnswerHandedInWithoutAsking) {
  CloudStreamLaunch handed;
  handed.rig.http.on("GET", std::string{ClearanceRig::cloud_benefits},
                     {{503, ""}});
  EXPECT_TRUE(
      handed.clear(owning_rivet(), rivet, streaming_allowed()).is_allowed());
  EXPECT_EQ(handed.rig.http.count(ClearanceRig::cloud_benefits), 0);
  EXPECT_EQ(kind_of(handed.clear(owning_rivet(), rivet, streaming_refused())),
            Kind::needs_premium);
}

}  // namespace
}  // namespace catalog_test
