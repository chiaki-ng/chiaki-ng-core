#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace accounts_test {
namespace {

using account::Error;
using libchiaking::AccountId;
using libchiaking::psn::auth::AppToken;

class AccountFailure : public AccountClientTest {};

TEST_F(AccountFailure, MapsAnUnauthorisedReplyToAuth) {
  const auto failure = profile_failure(ScriptedHttp{401, "{}"});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->error, Error::auth);
}

TEST_F(AccountFailure, KeepsTheHttpStatus) {
  const auto failure = profile_failure(ScriptedHttp{403, "{}"});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->http_status, 403);
}

TEST_F(AccountFailure, MapsTooManyRequestsToRateLimited) {
  const auto failure = profile_failure(ScriptedHttp{429, "{}"});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->error, Error::rate_limited);
}

TEST_F(AccountFailure, MapsServerErrorsAndMissingPagesToUnavailable) {
  const auto server = profile_failure(ScriptedHttp{500, "{}"});
  ASSERT_TRUE(server);
  EXPECT_EQ(server->error, Error::unavailable);
  const auto missing = profile_failure(ScriptedHttp{404, "{}"});
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->error, Error::unavailable);
}

TEST_F(AccountFailure, MapsUnreadableOrNamelessProfilesToMalformed) {
  const auto unreadable = profile_failure(ScriptedHttp{200, "not json"});
  ASSERT_TRUE(unreadable);
  EXPECT_EQ(unreadable->error, Error::malformed);
  const auto nameless =
      profile_failure(ScriptedHttp{200, R"({"isPlus":true})"});
  ASSERT_TRUE(nameless);
  EXPECT_EQ(nameless->error, Error::malformed);
}

TEST_F(AccountFailure, MapsADisconnectToUnavailable) {
  const auto failure =
      profile_failure(ScriptedHttp{platform::HttpError::disconnected});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->error, Error::unavailable);
}

TEST_F(AccountFailure, MapsACancelledRequestOrAStoppedCallerToCancelled) {
  const auto cancelled =
      profile_failure(ScriptedHttp{platform::HttpError::cancelled});
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(cancelled->error, Error::cancelled);
  const auto stopped =
      profile_failure(ScriptedHttp{200, "{}"}, test_support::Stopped::token());
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->error, Error::cancelled);
}

TEST_F(AccountFailure, RefusesAnEmptyOrOversizedTokenWithoutARequest) {
  ScriptedHttp unused{200, "{}"};
  account::Client client{unused, clock};
  const auto anonymous = client.profile(AppToken{""}, AccountId{7});
  ASSERT_FALSE(anonymous);
  EXPECT_EQ(anonymous.error().error, Error::invalid_argument);
  const auto oversized =
      client.profile(AppToken{std::string(8193, 't')}, AccountId{7});
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error().error, Error::invalid_argument);
  EXPECT_TRUE(unused.urls.empty());
}

}  // namespace
}  // namespace accounts_test
