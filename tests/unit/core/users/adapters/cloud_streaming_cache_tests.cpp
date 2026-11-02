#include "src/core/users/adapters/ps_user_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/cloud_streaming_rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace users_test {
namespace {

using libchiaking::users::CatalogCalls;
using libchiaking::users::CloudStreamingCache;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = CloudStreamingRig::Reply;
using Remedy = CloudStreamingRig::Remedy;
using Freshness = CloudStreamingRig::Freshness;

class CloudStreamingHeldAnswer : public ::testing::Test {
 protected:
  static auto milliseconds_of(auto lifetime) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(lifetime)
        .count();
  }

  auto benefits_asked() const -> std::size_t {
    return rig.asked(CloudStreamingRig::benefits_url);
  }

  CloudStreamingRig rig;
};

TEST_F(CloudStreamingHeldAnswer, ServesTilesWhileALaunchReadsAfresh) {
  rig.benefits({Reply{200, std::string{CloudStreamingRig::granted}},
                Reply{200, std::string{CloudStreamingRig::denied}},
                Reply{503, ""}});
  rig.services({Reply{200, "[" + CloudStreamingRig::plus("TIER_10") + "]"}});
  CatalogCalls calls{*rig.user};
  EXPECT_FALSE(calls.held_cloud_streaming());
  EXPECT_TRUE(rig.ask(Freshness::held_will_do).is_allowed());
  EXPECT_TRUE(rig.ask(Freshness::held_will_do).is_allowed());
  EXPECT_EQ(benefits_asked(), 1U);
  ASSERT_TRUE(calls.held_cloud_streaming());
  EXPECT_TRUE(calls.held_cloud_streaming()->is_allowed());

  const auto fresh = rig.ask(Freshness::read_afresh);
  EXPECT_EQ(fresh.remedy(), Remedy::upgrade);
  EXPECT_EQ(benefits_asked(), 2U);
  ASSERT_TRUE(calls.held_cloud_streaming());
  EXPECT_EQ(calls.held_cloud_streaming()->remedy(), Remedy::upgrade);

  const auto unknown = rig.ask(Freshness::read_afresh);
  EXPECT_TRUE(unknown.cause());
  EXPECT_EQ(benefits_asked(), 3U);
  ASSERT_TRUE(calls.held_cloud_streaming());
  EXPECT_EQ(calls.held_cloud_streaming()->remedy(), Remedy::upgrade);

  rig.clock.advance_ms(milliseconds_of(CloudStreamingCache::lifetime));
  EXPECT_FALSE(calls.held_cloud_streaming());
}

TEST_F(CloudStreamingHeldAnswer, HoldsAnUnknownAnswerBrieflySoTilesDoNotAsk) {
  rig.benefits({Reply{503, ""}, Reply{503, ""},
                Reply{200, std::string{CloudStreamingRig::granted}}});
  CatalogCalls calls{*rig.user};
  EXPECT_TRUE(rig.ask(Freshness::held_will_do).cause());
  const auto held = rig.ask(Freshness::held_will_do);
  ASSERT_TRUE(held.cause());
  EXPECT_EQ(held.cause()->reason, Reason::unavailable);
  EXPECT_EQ(benefits_asked(), 1U);

  EXPECT_TRUE(rig.ask(Freshness::read_afresh).cause());
  EXPECT_EQ(benefits_asked(), 2U);

  rig.clock.advance_ms(milliseconds_of(CloudStreamingCache::unknown_lifetime));
  EXPECT_FALSE(calls.held_cloud_streaming());
  EXPECT_TRUE(rig.ask(Freshness::held_will_do).is_allowed());
  EXPECT_EQ(benefits_asked(), 3U);
}

TEST_F(CloudStreamingHeldAnswer, IsNotKeptWhenTheQuestionIsCancelled) {
  rig.benefits({Reply{503, ""}});
  rig.state->stopped = true;
  const auto stopped = rig.ask(Freshness::held_will_do);
  ASSERT_TRUE(stopped.cause());
  EXPECT_EQ(stopped.cause()->reason, Reason::cancelled);
  EXPECT_FALSE(CatalogCalls{*rig.user}.held_cloud_streaming());
}

TEST_F(CloudStreamingHeldAnswer, BelongsToOneAccountAndEndsAtSignOut) {
  rig.benefits({Reply{200, std::string{CloudStreamingRig::granted}}});
  const auto other = rig.signed_in.as(libchiaking::users::SavedSignIn{
      libchiaking::AccountId{2}, "other",
      libchiaking::psn::auth::Npsso{"NPSSO-C"}});
  EXPECT_TRUE(rig.ask().is_allowed());
  EXPECT_TRUE(CatalogCalls{*rig.user}.held_cloud_streaming());
  EXPECT_FALSE(CatalogCalls{*other}.held_cloud_streaming());

  EXPECT_TRUE(rig.signed_in.users.sign_out(rig.user->account_id()));
  EXPECT_FALSE(
      libchiaking::users::PsUser::Core::of(*rig.user).cloud_streaming().held());
  EXPECT_FALSE(CatalogCalls{*rig.user}.held_cloud_streaming());
  const auto gone = rig.ask();
  ASSERT_TRUE(gone.cause());
  EXPECT_EQ(gone.cause()->reason, Reason::signed_out);
  EXPECT_EQ(benefits_asked(), 1U);
}

}  // namespace
}  // namespace users_test
