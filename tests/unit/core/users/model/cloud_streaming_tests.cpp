#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/cloud_streaming_rig.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace users_test {
namespace {

using libchiaking::users::CloudBenefits;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = CloudStreamingRig::Reply;
using Remedy = CloudStreamingRig::Remedy;
using Freshness = CloudStreamingRig::Freshness;

class CloudStreamingVerdict : public ::testing::Test {
 protected:
  static void unknown_after(std::vector<Reply> replies, Reason expected) {
    CloudStreamingRig rig;
    rig.benefits(std::move(replies));
    rig.services({Reply{200, "[" + CloudStreamingRig::plus("TIER_10") + "]"}});
    const auto verdict = rig.ask();
    EXPECT_FALSE(verdict.is_allowed());
    EXPECT_FALSE(verdict.hides_tiles());
    EXPECT_FALSE(verdict.remedy());
    EXPECT_FALSE(verdict.benefits());
    EXPECT_TRUE(verdict.plans().empty());
    ASSERT_TRUE(verdict.cause());
    EXPECT_EQ(verdict.cause()->reason, expected);
    EXPECT_EQ(rig.asked(CloudStreamingRig::services_url), 0U);
    const auto held =
        libchiaking::users::CatalogCalls{*rig.user}.held_cloud_streaming();
    ASSERT_TRUE(held);
    ASSERT_TRUE(held->cause());
    EXPECT_EQ(held->cause()->reason, expected);
  }
};

class CloudStreamingWording : public ::testing::Test {
 protected:
  static auto plus(std::string_view tier,
                   std::string_view status = "SUBSCRIBED", bool active = true)
      -> std::string {
    return CloudStreamingRig::plus(tier, status, active);
  }

  static auto worded_by(std::string services)
      -> std::optional<CloudStreamingRig::Remedy> {
    return CloudStreamingRig::worded_by(std::move(services));
  }
};

TEST_F(CloudStreamingVerdict, AllowsGrantedBenefitsWithTheFirstOwnersPlans) {
  CloudStreamingRig rig;
  rig.benefits({Reply{200, std::string{CloudStreamingRig::granted}}});
  const auto verdict = rig.ask();
  EXPECT_TRUE(verdict.is_allowed());
  EXPECT_FALSE(verdict.hides_tiles());
  EXPECT_FALSE(verdict.remedy());
  EXPECT_FALSE(verdict.cause());
  ASSERT_EQ(verdict.plans().size(), 2U);
  EXPECT_EQ(verdict.plans()[0].text(), "PLAN-A");
  EXPECT_EQ(verdict.plans()[1].text(), "PLAN-B");
  EXPECT_EQ(rig.asked(CloudStreamingRig::services_url), 0U);
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  const auto& seen = rig.fake.seen.front();
  ASSERT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.headers[0].value, "Bearer app");

  const auto& benefits = verdict.benefits();
  ASSERT_TRUE(benefits);
  EXPECT_EQ(benefits->authorisation(), CloudBenefits::Authorisation::granted);
  EXPECT_EQ(benefits->justification(), "OK");
  ASSERT_EQ(benefits->owners().size(), 2U);
  const auto& second = benefits->owners()[1];
  EXPECT_EQ(second.account_id, "200");
  ASSERT_EQ(second.plans.size(), 1U);
  EXPECT_EQ(second.plans[0].text(), "PLAN-C");
  EXPECT_TRUE(second.has_sgod_benefit);
  EXPECT_FALSE(second.has_est_benefit);
  EXPECT_EQ(benefits->read_at().time_since_epoch(),
            std::chrono::milliseconds{1'000'000});
}

TEST_F(CloudStreamingVerdict, AsksToSubscribeWhenGrantedBenefitsHaveNoPlans) {
  for (
      const std::string& body :
      {std::string{R"({"authorization":true,"cloudgameOwnerInfos":[]})"},
       std::string{R"({"authorization":true})"},
       std::string{
           R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1","benefitPlanIds":[]},{"benefitPlanIds":["PLAN-C"]}]})"}}) {
    SCOPED_TRACE(body);
    CloudStreamingRig rig;
    rig.benefits({Reply{200, body}});
    rig.services({Reply{200, "[" + CloudStreamingRig::plus("TIER_10") + "]"}});
    const auto verdict = rig.ask();
    EXPECT_FALSE(verdict.is_allowed());
    EXPECT_TRUE(verdict.hides_tiles());
    EXPECT_EQ(verdict.remedy(), Remedy::subscribe);
    EXPECT_TRUE(verdict.plans().empty());
    EXPECT_EQ(rig.asked(CloudStreamingRig::services_url), 0U);
  }
}

TEST_F(CloudStreamingWording, WordsADenialByTheFirstPlusService) {
  EXPECT_EQ(worded_by("[" + plus("TIER_10") + "]"), Remedy::upgrade);
  EXPECT_EQ(worded_by("[" + plus("TIER_20") + "]"), Remedy::upgrade);
  EXPECT_EQ(worded_by("[" + plus("TIER_30") + "]"), Remedy::subscribe);
  EXPECT_EQ(worded_by("[" + plus("TIER_10", "CANCELLED") + "]"),
            Remedy::subscribe);
  EXPECT_EQ(worded_by("[" + plus("TIER_20", "subscribed") + "]"),
            Remedy::subscribe);
  EXPECT_EQ(worded_by("[" + plus("TIER_10", "SUBSCRIBED", false) + "]"),
            Remedy::subscribe);
  EXPECT_EQ(
      worded_by(
          R"([{"type":"EAPLAY","state":{"active":true,"status":"SUBSCRIBED"},"currentTier":{"tierLabel":"TIER_10"}}])"),
      Remedy::subscribe);
  EXPECT_EQ(worded_by(R"([{"type":"EAPLAY"},)" + plus("TIER_20") + "]"),
            Remedy::upgrade);
  EXPECT_EQ(worded_by("[" + plus("TIER_30") + "," + plus("TIER_10") + "]"),
            Remedy::subscribe);
  EXPECT_EQ(worded_by(R"({"type":"PSPLUS"})"), Remedy::subscribe);
  EXPECT_EQ(worded_by("[]"), Remedy::subscribe);
  EXPECT_EQ(worded_by("not json"), Remedy::subscribe);
  EXPECT_EQ(CloudStreamingRig::refused_with(
                std::string{CloudStreamingRig::denied}, {Reply{500, ""}})
                .remedy(),
            Remedy::subscribe);
  EXPECT_EQ(CloudStreamingRig::refused_with(
                std::string{CloudStreamingRig::denied}, {Reply::transport()})
                .remedy(),
            Remedy::subscribe);
}

TEST_F(CloudStreamingVerdict, TreatsUnstatedAuthorisationAsADenial) {
  const auto silent = CloudStreamingRig::refused_with(
      R"({"cloudgameOwnerInfos":[{"ownerAccountId":"1","benefitPlanIds":["PLAN-A"]}]})",
      {Reply{200, "[" + CloudStreamingRig::plus("TIER_20") + "]"}});
  EXPECT_FALSE(silent.is_allowed());
  EXPECT_EQ(silent.remedy(), Remedy::upgrade);
  EXPECT_TRUE(silent.plans().empty());
  ASSERT_TRUE(silent.benefits());
  EXPECT_EQ(silent.benefits()->authorisation(),
            CloudBenefits::Authorisation::unstated);
  EXPECT_EQ(silent.benefits()->owners().size(), 1U);

  const auto nulled = CloudStreamingRig::refused_with(
      R"({"authorization":null})",
      {Reply{200, "[" + CloudStreamingRig::plus("TIER_30") + "]"}});
  EXPECT_EQ(nulled.remedy(), Remedy::subscribe);
}

TEST_F(CloudStreamingVerdict, IsUnknownAndNeverWordedWhenBenefitsAreUnread) {
  {
    SCOPED_TRACE("not json");
    unknown_after({Reply{200, "not json"}}, Reason::malformed);
  }
  {
    SCOPED_TRACE("authorization as text");
    unknown_after({Reply{200, R"({"authorization":"yes"})"}},
                  Reason::malformed);
  }
  {
    SCOPED_TRACE("503");
    unknown_after({Reply{503, ""}}, Reason::unavailable);
  }
  {
    SCOPED_TRACE("429");
    unknown_after({Reply{429, ""}}, Reason::rate_limited);
  }
  {
    SCOPED_TRACE("transport");
    unknown_after({Reply::transport()}, Reason::unavailable);
  }
}

TEST_F(CloudStreamingVerdict, IsUnknownWithoutStoreAccessAndAsksNothing) {
  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp fake;
  auto state = std::make_shared<test_support::StopState>();
  libchiaking::net::http::Client client{
      fake, Runtime::steady(), libchiaking::platform::StopToken{state}};
  test_support::users::SignedIn signed_in{fake, clock, wall};
  const auto tokenless = signed_in.as("NPSSO-B");
  const auto lapsed =
      libchiaking::users::CatalogCalls{*tokenless}.cloud_streaming(
          client, Freshness::read_afresh);
  ASSERT_TRUE(lapsed.cause());
  EXPECT_EQ(lapsed.cause()->reason, Reason::store_access_lapsed);
  EXPECT_TRUE(fake.seen.empty());
}

}  // namespace
}  // namespace users_test
