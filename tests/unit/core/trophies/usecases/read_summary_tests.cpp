#include "tests/unit/core/trophies/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace trophies_test {
namespace {

using libchiaking::trophies::TrophyCounts;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = Rig::Reply;

class TrophySummary : public ::testing::Test {
 protected:
  void answers_summary() {
    rig.http.on("GET", Rig::summary_url(),
                {Reply{200, std::string{Rig::summary_body}}});
  }

  Rig rig;
};

TEST(TrophySummaryAcrossRestarts, IsReadOnceAndKept) {
  const auto records = Rig::fresh_records();
  {
    Rig rig{records};
    rig.http.on("GET", Rig::summary_url(),
                {Reply{200, std::string{Rig::summary_body}}});
    const auto summary = rig.read().summary(*rig.user, {});
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary->level, 312);
    EXPECT_EQ(summary->tier, 4);
    EXPECT_EQ(summary->progress, 46);
    EXPECT_EQ(summary->earned, (TrophyCounts{2104, 611, 158, 21}));
    EXPECT_EQ(rig.http.seen.back().header("Authorization"),
              "Bearer " + std::string{Rig::app_token});
    const auto again = rig.read().summary(*rig.user, {});
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *summary);
    EXPECT_EQ(rig.requests_to(Rig::summary_url()), 1);
  }
  EXPECT_TRUE(Rig::keeps(*records, "trophies/7/summary"));
  Rig restarted{records};
  const auto summary = restarted.read().summary(*restarted.user, {});
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->level, 312);
  EXPECT_TRUE(restarted.http.seen.empty());
}

TEST_F(TrophySummary, ExpiresSixHoursAfterItWasKept) {
  rig.http.on("GET", Rig::summary_url(),
              {Reply{200, std::string{Rig::summary_body}},
               Reply{200, R"({"trophyLevel":313,"tier":4,"progress":0,)"
                          R"("earnedTrophies":{"bronze":2105,"silver":611,)"
                          R"("gold":158,"platinum":21}})"}});
  ASSERT_TRUE(rig.read().summary(*rig.user, {}));
  rig.advance_wall(std::chrono::hours{6} - std::chrono::milliseconds{1});
  const auto kept = rig.read().summary(*rig.user, {});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->level, 312);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 1);
  rig.advance_wall(std::chrono::milliseconds{1});
  const auto fresh = rig.read().summary(*rig.user, {});
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->level, 313);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 2);
}

TEST_F(TrophySummary, RenewsARefusedTokenOnce) {
  rig.renews_store_access();
  rig.http.on("GET", Rig::summary_url(),
              {Reply{401, R"({"error":"invalid token"})"},
               Reply{200, std::string{Rig::summary_body}}});
  const auto summary = rig.read().summary(*rig.user, {});
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->level, 312);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 2);
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            "Bearer " + std::string{Rig::renewed_token});
}

TEST_F(TrophySummary, LapsesStoreAccessWhenTheRenewedTokenIsRefused) {
  rig.renews_store_access();
  rig.http.on("GET", Rig::summary_url(),
              {Reply{401, R"({"error":"invalid"})"}});
  const auto summary = rig.read().summary(*rig.user, {});
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().reason, Reason::store_access_lapsed);
  EXPECT_EQ(summary.error().http_status, 401);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 2);
  EXPECT_TRUE(rig.records->names().empty());
}

TEST_F(TrophySummary, CoolsTheAccountDownAfterA429) {
  rig.http.on("GET", Rig::summary_url(),
              {Reply{429, "{}"}, Reply{200, std::string{Rig::summary_body}}});
  const auto throttled = rig.read().summary(*rig.user, {});
  ASSERT_FALSE(throttled);
  EXPECT_EQ(throttled.error().reason, Reason::rate_limited);
  EXPECT_EQ(throttled.error().http_status, 429);
  const auto cooling = rig.read().summary(*rig.user, {});
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error().reason, Reason::rate_limited);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 1);
  rig.clock.advance_ms(
      std::chrono::milliseconds{std::chrono::minutes{15}}.count());
  EXPECT_TRUE(rig.read().summary(*rig.user, {}));
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 2);
}

TEST_F(TrophySummary, ReportsAMalformedReply) {
  rig.http.on("GET", Rig::summary_url(), {Reply{200, "not json"}});
  const auto malformed = rig.read().summary(*rig.user, {});
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().reason, Reason::malformed);
}

TEST_F(TrophySummary, ReportsAnUnreachableServiceAfterRetriesWithoutKeepingIt) {
  const auto unreachable = rig.read().summary(*rig.user, {});
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error().reason, Reason::unavailable);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 3);
  EXPECT_TRUE(rig.records->names().empty());
}

TEST_F(TrophySummary, TurnsAHomeUserAwayWithoutARequest) {
  auto added = rig.signed_in.users.add_home_user(libchiaking::AccountId{9},
                                                 "Living Room");
  ASSERT_TRUE(added);
  const auto summary = rig.read().summary(*added.value(), {});
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().reason, Reason::signed_out);
  const auto titles = rig.read().titles(*added.value(), {});
  ASSERT_FALSE(titles);
  EXPECT_EQ(titles.error().reason, Reason::signed_out);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(TrophySummary, IsCancelledBeforeSendingByAFiredStopToken) {
  answers_summary();
  auto stopped = std::make_shared<test_support::StopState>();
  stopped->stopped = true;
  const auto summary =
      rig.read().summary(*rig.user, libchiaking::platform::StopToken{stopped});
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().reason, Reason::cancelled);
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 0);
}

TEST_F(TrophySummary, IsDroppedByStartingOverAndByForgetting) {
  answers_summary();
  ASSERT_TRUE(rig.read().summary(*rig.user, {}));
  EXPECT_FALSE(rig.records->names().empty());
  rig.read().start_over(rig.user->account_id());
  EXPECT_TRUE(rig.records->names().empty());
  ASSERT_TRUE(rig.read().summary(*rig.user, {}));
  EXPECT_EQ(rig.requests_to(Rig::summary_url()), 2);
  rig.read().forget(rig.user->account_id());
  EXPECT_TRUE(rig.records->names().empty());
}

}  // namespace
}  // namespace trophies_test
