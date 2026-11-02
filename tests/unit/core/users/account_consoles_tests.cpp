#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using libchiaking::AccountId;
using users::PsUser;
using users::SignInState;
using users::Unanswered;
using Reply = Psn::Reply;

class AccountConsoles : public ::testing::Test {
 protected:
  static constexpr std::string_view consoles_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients"
      "?platform=PS5&includeFields=device,systemData&limit=10&offset=0";
  static constexpr std::string_view renewal =
      R"({"access_token":"new-remote","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})";

  static auto listed() -> std::string {
    return std::string{R"({"clients":[{"duid":")"} + std::string(64, 'a') +
           R"(","device":{"name":"Living Room","enabledFeatures":["remotePlay"]}}]})";
  }

  static auto remote(SignedInRig& rig, std::int64_t expires_in_ms = 3'600'000)
      -> std::shared_ptr<PsUser> {
    auto saved = rig.saved(30, "NPSSO-A", "app-a");
    saved.remote_play.emplace(psn::auth::BearerToken{"remote-access"},
                              Psn::at_ms(Psn::wall_start_ms + expires_in_ms));
    saved.remote_play_refresh.emplace(
        psn::auth::RemotePlayRefreshToken{"old-remote"});
    return rig.signed_in.as(std::move(saved));
  }

  static auto asked(SignedInRig& rig, const std::shared_ptr<PsUser>& user,
                    const libchiaking::platform::StopToken& stop = {}) {
    libchiaking::net::http::Client http{rig.fake, Runtime::steady(), stop};
    return users::ConsoleCalls{*user}.account_consoles(http, 0);
  }
};

TEST_F(AccountConsoles, AreAskedOnceUnderRemotePlayAccessForQuestionsInFlight) {
  SignedInRig rig;
  const auto user = remote(rig);
  const auto [first, second] = Runtime::asked_together(
      rig.fake, "GET", std::string{consoles_url}, Reply{200, listed()}, false,
      [&](const libchiaking::platform::StopToken& stop) {
        return asked(rig, user, stop);
      });
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  ASSERT_EQ(first.value().consoles.size(), 1U);
  EXPECT_EQ(second.value().consoles.size(), 1U);
  EXPECT_EQ(first.value().consoles.front().name, "Living Room");
  EXPECT_TRUE(first.value().consoles.front().device_uid);
  EXPECT_TRUE(first.value().consoles.front().remote_play_enabled);
  EXPECT_EQ(first.value().received, 1);
  EXPECT_EQ(rig.fake.seen.front().header("Authorization"),
            "Bearer remote-access");
  EXPECT_TRUE(asked(rig, user));
  EXPECT_EQ(rig.fake.seen.size(), 2U);
}

TEST_F(AccountConsoles, LetAWaitingCallerGiveUp) {
  SignedInRig impatient;
  const auto waiting = remote(impatient);
  const auto [kept, given_up] = Runtime::asked_together(
      impatient.fake, "GET", std::string{consoles_url}, Reply{200, listed()},
      true, [&](const libchiaking::platform::StopToken& stop) {
        return asked(impatient, waiting, stop);
      });
  EXPECT_TRUE(kept);
  ASSERT_FALSE(given_up);
  EXPECT_EQ(given_up.error().reason, Unanswered::Reason::cancelled);
  EXPECT_EQ(impatient.fake.seen.size(), 1U);
}

TEST_F(AccountConsoles, RenewLapsingRemotePlayAccessFirst) {
  SignedInRig lapsed;
  const auto late = remote(lapsed, 30'000);
  lapsed.fake.on("POST", std::string{Psn::legacy_token_url},
                 {Reply{200, std::string{renewal}}});
  lapsed.fake.on("GET", std::string{consoles_url}, {Reply{200, listed()}});
  const auto saves = lapsed.signed_in.credentials.saves;
  const auto renewed = asked(lapsed, late);
  EXPECT_TRUE(renewed);
  ASSERT_EQ(lapsed.fake.seen.size(), 2U);
  EXPECT_EQ(lapsed.fake.seen[0].url, Psn::legacy_token_url);
  EXPECT_EQ(lapsed.fake.seen[0].body, Psn::remote_refresh_body);
  EXPECT_EQ(lapsed.fake.seen[1].header("Authorization"), "Bearer new-remote");
  EXPECT_EQ(lapsed.signed_in.credentials.saves, saves + 1);
  const auto stored = lapsed.signed_in.credentials.saved(AccountId{30});
  ASSERT_TRUE(stored);
  ASSERT_TRUE(stored->remote_play);
  EXPECT_EQ(stored->remote_play->token.view(), "new-remote");
  ASSERT_TRUE(stored->remote_play_refresh);
  EXPECT_EQ(stored->remote_play_refresh->token.view(), "next-remote");
}

TEST_F(AccountConsoles, RenewRefusedRemotePlayAccessAndAskAgain) {
  SignedInRig refused;
  const auto turned = remote(refused);
  refused.fake.on("POST", std::string{Psn::legacy_token_url},
                  {Reply{200, std::string{renewal}}});
  refused.fake.on("GET", std::string{consoles_url},
                  {Reply{401, "{}"}, Reply{200, listed()}});
  const auto retried = asked(refused, turned);
  EXPECT_TRUE(retried);
  ASSERT_EQ(refused.fake.seen.size(), 3U);
  EXPECT_EQ(refused.fake.seen[0].header("Authorization"),
            "Bearer remote-access");
  EXPECT_EQ(refused.fake.seen[1].url, Psn::legacy_token_url);
  EXPECT_EQ(refused.fake.seen[2].header("Authorization"), "Bearer new-remote");
}

TEST_F(AccountConsoles, ReportMissingAccessWhenTheRenewedTokenIsRefused) {
  SignedInRig barred;
  const auto unwelcome = remote(barred);
  barred.fake.on("POST", std::string{Psn::legacy_token_url},
                 {Reply{200, std::string{renewal}}});
  barred.fake.on("GET", std::string{consoles_url}, {Reply{401, "{}"}});
  const auto nothing = asked(barred, unwelcome);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason,
            Unanswered::Reason::remote_play_access_missing);
  EXPECT_EQ(nothing.error().http_status, 401);
  EXPECT_EQ(barred.fake.seen.size(), 3U);
  EXPECT_EQ(unwelcome->sign_in_state(), SignInState::signed_in);
}

TEST_F(AccountConsoles, AreNotAskedWithoutRemotePlayAccess) {
  SignedInRig bare;
  const auto without = bare.signed_in.as(bare.saved(30, "NPSSO-A", "app-a"));
  const auto missing = asked(bare, without);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason,
            Unanswered::Reason::remote_play_access_missing);
  EXPECT_TRUE(bare.fake.seen.empty());
}

}  // namespace
}  // namespace users_test
