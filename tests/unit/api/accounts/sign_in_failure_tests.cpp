#include "libchiaking/users_types.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace accounts_test {
namespace {

using libchiaking::psn::auth::Npsso;
using libchiaking::psn::auth::SignIn;
using libchiaking::psn::auth::SignInError;
using libchiaking::psn::auth::SignInErrorKind;

static_assert(SignInError{SignInErrorKind::rate_limited, 429}.cause() ==
              "the server is rate limiting sign-in");
static_assert(SignInError{SignInErrorKind::malformed, 200,
                          "the token reply is not a JSON object"}
                  .cause() == "the token reply is not a JSON object");
static_assert(SignInError{SignInErrorKind::transport}.step().empty());
static_assert(SignInError{SignInErrorKind::transport}.at("psn.step").step() ==
              "psn.step");

class SignInFailure : public ::testing::Test {
 protected:
  static constexpr std::string_view secret = "NPSSO-SECRET-7f3a";
  static constexpr std::string_view duid =
      "000000070041008000000000000000000000000000000000";

  platform::SteadyClock clock;
  test_support::api::OfflineHttp http;
  test_support::FakeLog log;
  SignIn offline{http, clock, {}, &log};
};

TEST_F(SignInFailure, NamesTheTokenStepAndTheTransportCause) {
  const auto unreachable = offline.app_tokens(Npsso{std::string{secret}}, duid);
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error().kind(), SignInErrorKind::transport);
  EXPECT_EQ(unreachable.error().cause(), "the request did not complete");
  EXPECT_EQ(unreachable.error().step(), "psn.auth.sign_in.token");
  EXPECT_EQ(unreachable.error().cause().find(secret), std::string_view::npos);
}

TEST_F(SignInFailure, SaysStopWasRequestedWhenCancelled) {
  SignIn stopped{http, clock, test_support::Stopped::token(), &log};
  const auto cancelled = stopped.app_tokens(Npsso{std::string{secret}}, duid);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind(), SignInErrorKind::cancelled);
  EXPECT_EQ(cancelled.error().cause(), "stop was requested before the request");
}

TEST_F(SignInFailure, SaysAnEmptyCredentialIsNotUsable) {
  const auto unusable = offline.app_tokens(Npsso{""}, duid);
  ASSERT_FALSE(unusable);
  EXPECT_EQ(unusable.error().kind(), SignInErrorKind::invalid_request);
  EXPECT_EQ(unusable.error().cause(), "the credential is not usable");
}

TEST_F(SignInFailure, IsLoggedWithItsCauseAndWithoutTheCredential) {
  EXPECT_FALSE(offline.app_tokens(Npsso{std::string{secret}}, duid));
  SignIn stopped{http, clock, test_support::Stopped::token(), &log};
  EXPECT_FALSE(stopped.app_tokens(Npsso{std::string{secret}}, duid));
  EXPECT_FALSE(offline.app_tokens(Npsso{""}, duid));
  for (const auto& line : log.messages)
    EXPECT_EQ(line.find(secret), std::string::npos) << line;
  EXPECT_TRUE(std::ranges::any_of(log.messages, [](const std::string& line) {
    return line.find("the request did not complete") != std::string::npos;
  }));
}

}  // namespace
}  // namespace accounts_test
