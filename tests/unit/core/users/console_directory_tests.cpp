#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using namespace std::chrono_literals;
using libchiaking::AccountId;
using users::PsUser;
using users::SignInState;
using users::Unanswered;
using Reply = Psn::Reply;

class ConsoleDirectory : public ::testing::Test {
 protected:
  static constexpr std::string_view consoles_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients"
      "?platform=PS5&includeFields=device,systemData&limit=10&offset=0";
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";
  static constexpr std::string_view played_url =
      "https://m.np.playstation.com/api/gamelist/"
      "v2/users/me/titles?limit=50&offset=0";
  static constexpr std::string_view renewal =
      R"({"access_token":"new-mobile","refresh_token":"next-mobile","expires_in":2000})";

  static auto progress_url() -> std::string {
    return std::string{commands_url} +
           "?platform=PS5&commandIds=CMD&includeFields=actions,commandDetail&"
           "commandStatus=executing,completed";
  }

  static auto stored(SignedInRig& rig) -> std::shared_ptr<PsUser> {
    auto saved = rig.saved(31, "NPSSO-A", "app-a");
    saved.store_refresh.emplace(psn::auth::AppRefreshToken{"old-mobile"});
    return rig.signed_in.as(std::move(saved));
  }
};

TEST_F(ConsoleDirectory, IsAskedUnderStoreAccess) {
  SignedInRig rig;
  const auto user = stored(rig);
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  users::ConsoleCalls calls{*user};
  rig.fake.on("GET", std::string{consoles_url},
              {Reply{200, R"({"clients":[]})"}});
  rig.fake.on(
      "GET", std::string{played_url},
      {Reply{200, R"({"titles":[{"titleId":"PPSA00001_00","name":"Game"}]})"}});
  rig.fake.on("POST", std::string{commands_url},
              {Reply{200, R"({"commandId":"CMD"})"}});
  rig.fake.on("GET", progress_url(), {Reply{200, "{}"}});
  const auto installed = calls.installed_titles(http, 0);
  ASSERT_TRUE(installed);
  EXPECT_TRUE(installed.value().consoles.empty());
  EXPECT_EQ(rig.fake.seen.back().header("X-Psn-Can-Mobile-App-Type"), "PSAPP");
  const auto played = calls.played_titles(http, 0);
  ASSERT_TRUE(played);
  EXPECT_EQ(played.value().received, 1);
  const auto started =
      calls.start_title(http, std::string(64, 'a'), "PPSA00001_00", 5s);
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value(), "CMD");
  EXPECT_EQ(rig.fake.seen.back().method, "POST");
  EXPECT_NE(rig.fake.seen.back().body.find("launchTitle"), std::string::npos)
      << rig.fake.seen.back().body;
  EXPECT_TRUE(calls.title_start_progress(http, "CMD", false, "PS5", 5s));
  EXPECT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  for (const auto& seen : rig.fake.seen)
    EXPECT_EQ(seen.header("Authorization"), "Bearer app-a") << seen.url;
}

TEST_F(ConsoleDirectory, RenewsRefusedStoreAccessAndAsksAgain) {
  SignedInRig refused;
  const auto turned = stored(refused);
  libchiaking::net::http::Client again{refused.fake, Runtime::steady(), {}};
  refused.fake.on("GET", std::string{played_url},
                  {Reply{401, "{}"}, Reply{200, R"({"titles":[]})"}});
  refused.fake.on("POST", std::string{Psn::token_url},
                  {Reply{200, std::string{renewal}}});
  const auto saves = refused.signed_in.credentials.saves;
  const auto retried = users::CatalogCalls{*turned}.played_titles(again, 0);
  EXPECT_TRUE(retried);
  ASSERT_EQ(refused.fake.seen.size(), 3U);
  EXPECT_EQ(refused.fake.seen[0].header("Authorization"), "Bearer app-a");
  EXPECT_TRUE(Psn::exchanges_npsso(refused.fake.seen[1]))
      << refused.fake.seen[1].url;
  EXPECT_EQ(refused.fake.seen[2].header("Authorization"), "Bearer new-mobile");
  EXPECT_EQ(refused.signed_in.credentials.saves, saves + 1);
  const auto kept = refused.signed_in.credentials.saved(AccountId{31});
  ASSERT_TRUE(kept);
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "new-mobile");
  EXPECT_EQ(turned->sign_in_state(), SignInState::signed_in);
}

TEST_F(ConsoleDirectory, LapsesStoreAccessWhenTheRenewedTokenIsRefused) {
  SignedInRig barred;
  const auto unwelcome = stored(barred);
  libchiaking::net::http::Client third{barred.fake, Runtime::steady(), {}};
  barred.fake.on("GET", std::string{played_url}, {Reply{401, "{}"}});
  barred.fake.on("POST", std::string{Psn::token_url},
                 {Reply{200, std::string{renewal}}});
  const auto nothing = users::CatalogCalls{*unwelcome}.played_titles(third, 0);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason, Unanswered::Reason::store_access_lapsed);
  EXPECT_EQ(nothing.error().http_status, 401);
  EXPECT_EQ(barred.fake.seen.size(), 3U);
  EXPECT_EQ(unwelcome->sign_in_state(), SignInState::store_access_lapsed);
  EXPECT_FALSE(unwelcome->is_expired());
}

}  // namespace
}  // namespace users_test
