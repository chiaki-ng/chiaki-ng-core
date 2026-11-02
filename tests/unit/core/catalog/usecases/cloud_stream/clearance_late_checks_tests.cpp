#include "src/core/catalog/model/cloud_stream/exclusions.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::cloud_stream::ExclusionVerdict;

class CloudStreamClearanceResolver : public ::testing::Test,
                                     protected CloudStreamCases {};

TEST_F(CloudStreamClearanceResolver, RefusesWhenTheResolverFails) {
  for (const int status : {503, 500}) {
    CloudStreamLaunch failed;
    failed.rig.resolver_answers(status, "");
    const auto refused = failed.clear(owning_rivet(), rivet);
    EXPECT_EQ(kind_of(refused), Kind::check_unavailable) << status;
    EXPECT_EQ(failed.rig.http.count(ClearanceRig::feature_config), 0) << status;
  }
}

TEST_F(CloudStreamClearanceResolver, LaunchesTheProductWhenNothingIsListed) {
  CloudStreamLaunch unlisted;
  unlisted.rig.resolver_answers(200, R"({"productInfo":[]})");
  const auto verdict = unlisted.clear(owning_rivet(), rivet);
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->identifier.text(), rivet);
}

TEST_F(CloudStreamClearanceResolver,
       LaunchesTheProductWhenNoListedTitleMatches) {
  CloudStreamLaunch childless;
  childless.rig.resolver_answers(
      200,
      R"({"productInfo":[{"productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","titles":[{"titleId":"PPSA99999_00","entitlementId":"EP0109-PPSA99999_00-FAKE0036TITLE000"}]}]})");
  const auto verdict = childless.clear(owning_rivet(), rivet);
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->identifier.text(), rivet);
}

TEST_F(CloudStreamClearanceResolver, LaunchesTheHeldTitleWhenOneMatches) {
  CloudStreamLaunch child;
  child.rig.resolver_answers(
      200,
      R"({"productInfo":[{"productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","titles":[)"
      R"({"titleId":"PPSA99999_00","entitlementId":"EP0109-PPSA99999_00-FAKE0036TITLE000"},)"
      R"({"titleId":"PPSA07028_00","entitlementId":"EP0109-PPSA07028_00-FAKE0108TITLE000"}]}]})");
  const auto chosen = child.clear(owning_rivet(), rivet);
  EXPECT_TRUE(chosen.is_allowed());
  ASSERT_TRUE(chosen.cleared_launch());
  EXPECT_EQ(chosen.cleared_launch()->identifier.text(), rivet);
  ASSERT_TRUE(chosen.cleared_launch()->held_as);
  EXPECT_EQ(chosen.cleared_launch()->held_as->text(), rivet);
}

class CloudStreamClearanceExclusions : public ::testing::Test,
                                       protected CloudStreamCases {};

TEST_F(CloudStreamClearanceExclusions, RefusesAnExcludedTitle) {
  CloudStreamLaunch by_title;
  by_title.rig.excluding(R"("PPSA07028_00")");
  const auto excluded = by_title.clear(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(excluded), Kind::not_streamable);
  EXPECT_EQ(excluded.refusal()->notice(), ExclusionVerdict::excluded_notice);
}

TEST_F(CloudStreamClearanceExclusions, ClearsACopyWhoseTitleIsNotExcluded) {
  CloudStreamLaunch titled;
  titled.rig.excluding(R"("FAKE0110TI")");
  EXPECT_TRUE(titled.clear(owning_rivet(), rivet).is_allowed());
}

TEST_F(CloudStreamClearanceExclusions,
       RefusesACopyWithoutATitleWhoseProductIsExcluded) {
  CloudStreamLaunch by_product;
  by_product.rig.excluding(R"("FAKE0110TI")");
  by_product.rig.copy_streams(rivet, true);
  const auto contained = by_product.clear(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(contained), Kind::not_streamable);
  EXPECT_EQ(contained.refusal()->notice(), ExclusionVerdict::excluded_notice);
}

TEST_F(CloudStreamClearanceExclusions, RefusesWhenTheListIsUnavailable) {
  CloudStreamLaunch unavailable;
  unavailable.rig.http.on("GET", std::string{ClearanceRig::feature_config},
                          {{503, ""}});
  const auto unknown = unavailable.clear(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(unknown), Kind::check_unavailable);
  ASSERT_TRUE(unknown.refusal()->cause());
  EXPECT_EQ(unknown.refusal()->cause()->reason(),
            Reason::restriction_list_unavailable);
  EXPECT_EQ(unknown.refusal()->cause()->http_status(), 503);
}

class CloudStreamClearanceRelease : public ::testing::Test,
                                    protected CloudStreamCases {};

TEST_F(CloudStreamClearanceRelease,
       IsJudgedFromTheFreshRecordByTrustedTimeLast) {
  CloudStreamLaunch dated;
  dated.rig.ps5_record_answers(200, preordered("2031-01-01T00:00:00.000Z"));
  EXPECT_EQ(kind_of(dated.clear(owning_rivet(), rivet)),
            Kind::no_trusted_clock);
  EXPECT_GE(dated.rig.server.refreshes, 1);
  dated.rig.server.current = ClearanceRig::at("2031-01-01T00:00:00Z");
  const auto now = dated.clear(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(now), Kind::not_released);
  EXPECT_EQ(now.refusal()->time(), ClearanceRig::at("2031-01-01T00:00:00Z"));
  dated.rig.server.current = ClearanceRig::at("2031-01-01T00:00:01Z");
  EXPECT_TRUE(dated.clear(owning_rivet(), rivet).is_allowed());
}

TEST_F(CloudStreamClearanceRelease, ClearsWhenTheFreshRecordIsNoLongerDated) {
  CloudStreamLaunch released;
  EXPECT_TRUE(released.clear(owning_rivet("2031-01-01T00:00:00.000Z"), rivet)
                  .is_allowed());
}

TEST_F(CloudStreamClearanceRelease, RefusesAPreorderWithoutADateAsUnannounced) {
  CloudStreamLaunch undated;
  undated.rig.server.current = ClearanceRig::at("2030-01-01T00:00:00Z");
  undated.rig.ps5_record_answers(
      200, ClearanceRig::ps5_game_titled(rivet, "PPSA07028_00",
                                         R"(,"preorderCoreFlag":true)"));
  EXPECT_EQ(kind_of(undated.clear(owning_rivet(), rivet)), Kind::unannounced);
}

}  // namespace
}  // namespace catalog_test
