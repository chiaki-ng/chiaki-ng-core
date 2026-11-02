#include "tests/unit/core/psn/rest/trophy/trophies_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::TrophyTally;

class RestTrophySummary : public TrophiesFixture {
 protected:
  static constexpr std::string_view reply = R"({
  "accountId": "1234567890123456789",
  "trophyLevel": 312,
  "progress": 46,
  "tier": 4,
  "earnedTrophies": {"bronze": 2104, "silver": 611, "gold": 158, "platinum": 21}
})";

  void answers(Reply answer) {
    rig.fake.on("GET", url("/users/me/trophySummary"), {std::move(answer)});
  }
};

TEST_F(RestTrophySummary, IsReadWithTheBearer) {
  answers(Reply{200, std::string{reply}});
  const auto summary = trophies.summary();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->level, 312);
  EXPECT_EQ(summary->tier, 4);
  EXPECT_EQ(summary->progress, 46);
  EXPECT_EQ(summary->earned, (TrophyTally{2104, 611, 158, 21}));
  EXPECT_EQ(authorization(), "Bearer " + std::string{token});
}

TEST_F(RestTrophySummary, IsMalformedWithoutALevel) {
  answers(Reply{200, R"({"accountId":"1"})"});
  const auto summary = trophies.summary();
  ASSERT_FALSE(summary);
  EXPECT_TRUE(summary.error().is<bad_reply::InvalidSchema>());
}

TEST_F(RestTrophySummary, CarriesAnErrorStatusOver) {
  answers(Reply{429, R"({"error":{"code":2240526}})"});
  const auto summary = trophies.summary();
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().cause(), Cause::status);
  EXPECT_EQ(summary.error().http_status(), 429);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(RestTrophySummary, ReportsARefusedTokenAsRefused) {
  answers(Reply{401, R"({"error":{"message":"Invalid token"}})"});
  const auto summary = trophies.summary();
  ASSERT_FALSE(summary);
  EXPECT_TRUE(summary.error().auth_refused());
}

}  // namespace
}  // namespace psn_test
