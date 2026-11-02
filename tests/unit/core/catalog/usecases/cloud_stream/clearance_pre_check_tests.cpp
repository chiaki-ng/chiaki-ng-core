#include "src/core/catalog/model/cloud_stream/exclusions.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace catalog_test {
namespace {

struct HeldFact {
  std::string_view name;
  void (*arrange)(CloudStreamLaunch&);
  libchiaking::catalog::OwnedPs5Games (*owned)();
  std::string_view title;
  std::optional<CloudStreamCases::Kind> expected;
};

class CloudStreamPreCheck : public ::testing::TestWithParam<HeldFact>,
                            protected CloudStreamCases {
 protected:
  static auto same_verdict(const libchiaking::catalog::LaunchVerdict& launch,
                           const libchiaking::catalog::LaunchVerdict& held)
      -> ::testing::AssertionResult {
    if (launch.is_allowed() != held.is_allowed())
      return ::testing::AssertionFailure()
             << "the launch is " << (launch.is_allowed() ? "" : "not ")
             << "allowed but the pre-check is "
             << (held.is_allowed() ? "" : "not ") << "allowed";
    if (launch.is_allowed()) {
      if (*launch.cleared_launch() == *held.cleared_launch())
        return ::testing::AssertionSuccess();
      return ::testing::AssertionFailure() << "the cleared launches differ";
    }
    const auto& at_launch = *launch.refusal();
    const auto& before = *held.refusal();
    if (at_launch.kind() != before.kind())
      return ::testing::AssertionFailure()
             << "the kinds differ: "
             << ::testing::PrintToString(at_launch.kind()) << " at launch, "
             << ::testing::PrintToString(before.kind()) << " before";
    if (at_launch.notice() != before.notice())
      return ::testing::AssertionFailure() << "the notices differ";
    if (at_launch.time() != before.time())
      return ::testing::AssertionFailure() << "the times differ";
    if (at_launch.library_offer() != before.library_offer())
      return ::testing::AssertionFailure() << "the library offers differ";
    if (at_launch.tier_held() != before.tier_held())
      return ::testing::AssertionFailure() << "the tiers held differ";
    if (cause_of(at_launch) != cause_of(before))
      return ::testing::AssertionFailure() << "the causes differ";
    return ::testing::AssertionSuccess();
  }

  static auto cause_of(const libchiaking::catalog::LaunchRefusal& refusal)
      -> std::optional<Reason> {
    return refusal.cause() ? std::optional{refusal.cause()->reason()}
                           : std::nullopt;
  }
};

TEST_P(CloudStreamPreCheck, GivesTheLaunchAnswerWithoutAsking) {
  const auto& fact = GetParam();
  CloudStreamLaunch launch;
  fact.arrange(launch);
  const auto owned = fact.owned();
  const auto at_launch = launch.clear(owned, fact.title);
  const auto asked = launch.rig.http.requests();
  const auto before = launch.held(owned, fact.title);
  EXPECT_EQ(launch.rig.http.requests(), asked);
  EXPECT_TRUE(same_verdict(at_launch, before));
  if (fact.expected)
    EXPECT_EQ(kind_of(at_launch), *fact.expected);
  else
    EXPECT_TRUE(at_launch.is_allowed());
}

INSTANTIATE_TEST_SUITE_P(
    EveryHeldFact, CloudStreamPreCheck,
    ::testing::Values(
        HeldFact{"OwnedAndAllowed", [](CloudStreamLaunch&) {},
                 +[] { return CloudStreamCases::owning_rivet(); },
                 CloudStreamCases::rivet, std::nullopt},
        HeldFact{"StreamingRefused",
                 [](CloudStreamLaunch& launch) {
                   launch.rig.cloud_streaming_denied("TIER_20");
                 },
                 +[] { return CloudStreamCases::owning_rivet(); },
                 CloudStreamCases::rivet,
                 CloudStreamCases::Kind::needs_premium},
        HeldFact{"StreamingUnknown",
                 [](CloudStreamLaunch& launch) {
                   launch.rig.cloud_streaming_unreadable(503);
                 },
                 +[] { return CloudStreamCases::owning_rivet(); },
                 CloudStreamCases::rivet,
                 CloudStreamCases::Kind::check_unavailable},
        HeldFact{"PremiumNotOwned", [](CloudStreamLaunch&) {},
                 &CloudStreamCases::owning_nothing, CloudStreamCases::show,
                 CloudStreamCases::Kind::add_to_library},
        HeldFact{"Excluded",
                 [](CloudStreamLaunch& launch) {
                   launch.rig.excluding(R"("PPSA07028_00")");
                 },
                 +[] { return CloudStreamCases::owning_rivet(); },
                 CloudStreamCases::rivet,
                 CloudStreamCases::Kind::not_streamable},
        HeldFact{"ExclusionsUnavailable",
                 [](CloudStreamLaunch& launch) {
                   launch.rig.http.on("GET",
                                      std::string{ClearanceRig::feature_config},
                                      {{503, ""}});
                 },
                 +[] { return CloudStreamCases::owning_rivet(); },
                 CloudStreamCases::rivet,
                 CloudStreamCases::Kind::check_unavailable},
        HeldFact{
            "NoTrustedTime",
            [](CloudStreamLaunch& launch) {
              launch.rig.ps5_record_answers(
                  200,
                  CloudStreamCases::preordered("2031-01-01T00:00:00.000Z"));
            },
            +[] {
              return CloudStreamCases::owning_rivet("2031-01-01T00:00:00.000Z");
            },
            CloudStreamCases::rivet, CloudStreamCases::Kind::no_trusted_clock},
        HeldFact{
            "NotReleased",
            [](CloudStreamLaunch& launch) {
              launch.rig.ps5_record_answers(
                  200,
                  CloudStreamCases::preordered("2031-01-01T00:00:00.000Z"));
              launch.rig.server.current =
                  ClearanceRig::at("2030-06-01T00:00:00Z");
            },
            +[] {
              return CloudStreamCases::owning_rivet("2031-01-01T00:00:00.000Z");
            },
            CloudStreamCases::rivet, CloudStreamCases::Kind::not_released},
        HeldFact{
            "Released",
            [](CloudStreamLaunch& launch) {
              launch.rig.ps5_record_answers(
                  200,
                  CloudStreamCases::preordered("2031-01-01T00:00:00.000Z"));
              launch.rig.server.current =
                  ClearanceRig::at("2031-06-01T00:00:00Z");
            },
            +[] {
              return CloudStreamCases::owning_rivet("2031-01-01T00:00:00.000Z");
            },
            CloudStreamCases::rivet, std::nullopt}),
    [](const ::testing::TestParamInfo<HeldFact>& tested) {
      return std::string{tested.param.name};
    });

class CloudStreamPreCheckWithoutAnswers : public ::testing::Test,
                                          protected CloudStreamCases {};

TEST_F(CloudStreamPreCheckWithoutAnswers,
       ReportsTheCheckUnavailableBeforeAnythingIsHeld) {
  CloudStreamLaunch nothing_held;
  const auto unasked = nothing_held.held(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(unasked), Kind::check_unavailable);
  EXPECT_EQ(nothing_held.rig.http.requests(), 0U);
}

TEST_F(CloudStreamPreCheckWithoutAnswers, ClearsOnceALaunchHasBeenChecked) {
  CloudStreamLaunch nothing_held;
  EXPECT_TRUE(nothing_held.clear(owning_rivet(), rivet).is_allowed());
  EXPECT_TRUE(nothing_held.held(owning_rivet(), rivet).is_allowed());
}

TEST_F(CloudStreamPreCheckWithoutAnswers, RefusesAStaleExclusionList) {
  CloudStreamLaunch nothing_held;
  ASSERT_TRUE(nothing_held.clear(owning_rivet(), rivet).is_allowed());
  const auto now = libchiaking::catalog::cloud_stream::ExclusionList::Time{
      std::chrono::milliseconds{
          static_cast<std::int64_t>(ClearanceRig::now_us / 1000U)}};
  nothing_held.rig.held.keep(libchiaking::catalog::cloud_stream::ExclusionList{
      {}, now - std::chrono::hours{25}});
  const auto stale = nothing_held.held(owning_rivet(), rivet);
  EXPECT_EQ(kind_of(stale), Kind::check_unavailable);
  ASSERT_TRUE(stale.refusal()->cause());
  EXPECT_EQ(stale.refusal()->cause()->reason(),
            Reason::restriction_list_unavailable);
}

}  // namespace
}  // namespace catalog_test
