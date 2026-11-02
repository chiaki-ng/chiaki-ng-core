#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace accounts_test {
namespace {

using libchiaking::AccountId;
using libchiaking::psn::auth::AppToken;

class AccountClientOnAPlatform : public AccountClientTest {
 protected:
  ScriptedHttp http{200, R"({"onlineId":"plain"})"};
  test_support::FakeLog log;
  platform::DefaultPlatformFactory factory{
      platform::PlatformProviders{.http = &http, .diagnostics = &log}};
};

TEST_F(AccountClientOnAPlatform, IsNotBuiltWithoutHttp) {
  platform::DefaultPlatformFactory bare;
  EXPECT_FALSE(account::Client::on(bare));
}

TEST_F(AccountClientOnAPlatform,
       AsksThroughThePlatformHttpAndNeverLogsTheToken) {
  auto accounts = account::Client::on(factory);
  ASSERT_TRUE(accounts);
  EXPECT_TRUE(accounts->profile(AppToken{app_token}, AccountId{7}));
  EXPECT_EQ(http.urls.size(), 1U);
  for (const auto& line : log.messages)
    EXPECT_EQ(line.find(app_token), std::string::npos) << line;
}

TEST_F(AccountClientOnAPlatform, KeepsItsStopTokenAndSendsNothingOnceStopped) {
  auto stopped = account::Client::on(factory, test_support::Stopped::token());
  ASSERT_TRUE(stopped);
  const auto cancelled = stopped->profile(AppToken{app_token}, AccountId{7});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().error, account::Error::cancelled);
  EXPECT_TRUE(http.urls.empty());
}

}  // namespace
}  // namespace accounts_test
