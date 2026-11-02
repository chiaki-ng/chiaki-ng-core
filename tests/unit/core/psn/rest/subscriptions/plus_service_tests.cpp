#include "src/core/psn/rest/subscriptions/subscriptions_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::rest::Cause;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::PlusServiceState;
using libchiaking::psn::rest::SubscriptionsClient;
using Reply = RestRig::Reply;

class PlusService : public ::testing::Test {
 protected:
  static constexpr std::string_view url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/services/"
      "pssubscriptions?days=1&useTiers=true";

  static auto plus(RestRig& rig, std::string body, int status = 200)
      -> libchiaking::Expected<
          std::optional<PlusServiceState>,
          libchiaking::psn::rest::SubscriptionsClient::PlusServiceError> {
    rig.fake.on("GET", std::string{url}, {Reply{status, std::move(body)}});
    return SubscriptionsClient{
        rig.http, BearerToken{std::string_view{"APP-FAKE"}, "app bearer"}}
        .first_plus_service();
  }

  auto plus(std::string body, int status = 200) -> libchiaking::Expected<
      std::optional<PlusServiceState>,
      libchiaking::psn::rest::SubscriptionsClient::PlusServiceError> {
    return plus(rig, std::move(body), status);
  }

  RestRig rig;
};

TEST_F(PlusService, IsTheFirstPlusEntryAmongTheServices) {
  const auto read = plus(
      R"([{"type":"EAPLAY","state":{"active":true,"status":"SUBSCRIBED"},"currentTier":{"tierLabel":"TIER_30"}},)"
      R"({"type":"PSPLUS","state":{"active":true,"status":"SUBSCRIBED"},"currentTier":{"tierLabel":"TIER_20"}},)"
      R"({"type":"PSPLUS","state":{"active":false,"status":"EXPIRED"},"currentTier":{"tierLabel":"TIER_30"}}])");
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  const auto& first = *read.value();
  EXPECT_TRUE(first.active);
  EXPECT_EQ(first.status, "SUBSCRIBED");
  EXPECT_EQ(first.tier_label, "TIER_20");
  EXPECT_TRUE(rig.fake.unmatched.empty());
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.url, url);
  ASSERT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.headers[0].value, "Bearer APP-FAKE");
}

TEST_F(PlusService, ReadsMissingFieldsAsNothing) {
  const auto read = plus(R"([{"type":"PSPLUS"}])");
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_FALSE(read.value()->active);
  EXPECT_TRUE(read.value()->status.empty());
  EXPECT_TRUE(read.value()->tier_label.empty());
}

TEST_F(PlusService, ReadsMistypedFieldsAsNothing) {
  const auto typed = plus(
      R"([{"type":"PSPLUS","state":{"active":"true","status":7},"currentTier":"TIER_10"}])");
  ASSERT_TRUE(typed);
  ASSERT_TRUE(typed.value());
  EXPECT_FALSE(typed.value()->active);
  EXPECT_TRUE(typed.value()->status.empty());
  EXPECT_TRUE(typed.value()->tier_label.empty());
}

TEST_F(PlusService, IsAbsentWithoutAPlusEntry) {
  for (const std::string_view body :
       {"{}", "null", "[]", R"([{"type":"EAPLAY"},"PSPLUS"])"}) {
    RestRig none;
    const auto absent = plus(none, std::string{body});
    ASSERT_TRUE(absent) << body;
    EXPECT_FALSE(absent.value()) << body;
  }
}

TEST_F(PlusService, ReportsAnUnreadableReply) {
  const auto unread = plus("not json");
  ASSERT_FALSE(unread);
  EXPECT_EQ(unread.error().cause(), Cause::response);
}

TEST_F(PlusService, ReportsAServerError) {
  const auto failed = plus("", 500);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 500);
}

}  // namespace
}  // namespace psn_test
