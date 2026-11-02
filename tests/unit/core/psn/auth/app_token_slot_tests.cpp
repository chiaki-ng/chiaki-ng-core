#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "tests/unit/core/psn/auth/rig.hpp"
#include "tests/unit/core/psn/session_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::app_token_label;
using libchiaking::psn::auth::app_token_margin;
using libchiaking::psn::auth::AppToken;
using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::ExpirySource;
using libchiaking::psn::auth::TokenState;

class AppTokenSlot : public ::testing::Test {
 protected:
  static constexpr std::int64_t now_ms = 1'700'000'000'000;

  auto first_state() -> std::optional<TokenState> {
    const auto tokens = rig.snapshot().tokens;
    if (tokens.empty()) return std::nullopt;
    return tokens.front().state;
  }

  SessionRig rig;
  libchiaking::psn::auth::AppTokenSlot& app = rig.app_token();
};

TEST_F(AppTokenSlot, LeasesASuppliedTokenUntilTheMarginBeforeItExpires) {
  EXPECT_EQ(AuthRig::kind_of(app.lease()), AuthErrorKind::missing);
  EXPECT_TRUE(rig.snapshot().tokens.empty());
  app.supply(AppToken{"APP-SECRET"}, SessionRig::at(now_ms + 120'000));
  const auto lease = app.lease();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.value().view(), "APP-SECRET");
  rig.wall.advance_ms(120'000 - app_token_margin.count());
  EXPECT_EQ(AuthRig::kind_of(app.lease()), AuthErrorKind::expired);
}

TEST_F(AppTokenSlot, IsRefusedOnlyForItsOwnTokenUntilReplacedOrCleared) {
  app.supply(AppToken{"APP-SECRET"}, SessionRig::at(0));
  EXPECT_TRUE(app.lease());
  app.record_use(AppToken{"OTHER"}, 401, 0);
  EXPECT_TRUE(app.lease());
  app.record_use(AppToken{"APP-SECRET"}, 200, 1);
  app.record_use(AppToken{"APP-SECRET"}, 403, 0);
  EXPECT_EQ(AuthRig::kind_of(app.lease()), AuthErrorKind::refused);
  const auto view = rig.snapshot();
  EXPECT_EQ(view.uses, 2U);
  EXPECT_EQ(view.refusals, 1U);
  EXPECT_EQ(view.exchanges, 0U);
  ASSERT_EQ(view.tokens.size(), 1U);
  const auto& stats = view.tokens.front();
  EXPECT_EQ(stats.label, app_token_label);
  EXPECT_EQ(stats.expiry_source, ExpirySource::app);
  EXPECT_EQ(stats.state, TokenState::invalidated);
  EXPECT_FALSE(stats.expires_at_ms);
  EXPECT_EQ(stats.usage.uses, 2U);
  EXPECT_EQ(stats.usage.refusals, 1U);
  EXPECT_EQ(stats.usage.retries, 1U);
  EXPECT_EQ(stats.usage.last_use_status, 403);
  const auto json = rig.session.snapshot_json().value();
  EXPECT_EQ(json.find("APP-SECRET"), std::string::npos) << json;
  EXPECT_NE(json.find(R"json("label":"app token (supplied)")json"),
            std::string::npos)
      << json;
  EXPECT_NE(
      json.find(
          R"("expiresAtMs":null,"expirySource":"app","state":"invalidated")"),
      std::string::npos)
      << json;

  app.supply(AppToken{"APP-NEXT"}, SessionRig::at(now_ms + 3'600'000));
  EXPECT_TRUE(app.lease());
  EXPECT_EQ(first_state(), TokenState::valid);
  app.clear();
  EXPECT_EQ(AuthRig::kind_of(app.lease()), AuthErrorKind::missing);
  EXPECT_EQ(first_state(), TokenState::none);
  rig.session.clear();
  EXPECT_TRUE(rig.snapshot().tokens.empty());
}

TEST_F(AppTokenSlot, IsFedByAnIssuedAppToken) {
  rig.http.on(
      "POST", "https://ca.account.sony.com/api/authz/v3/oauth/token",
      {ClientRig::Reply{
          200,
          R"({"access_token":"mobile-access","refresh_token":"mobile-refresh","expires_in":7200})"}});
  libchiaking::psn::auth::SignIn sign_in{rig.http, ClientRig::steady_clock()};
  auto issued =
      sign_in.app_tokens(libchiaking::psn::auth::Npsso{"synthetic-npsso"},
                         "000000070041008012345678901234567890123456789012");
  ASSERT_TRUE(issued);
  test_support::FakeClock clock;
  test_support::FakeClock wall;
  libchiaking::psn::auth::AppTokenSlot slot{ClientRig::threading(), clock,
                                            wall};
  EXPECT_FALSE(slot.lease());
  const std::chrono::system_clock::time_point now{
      std::chrono::microseconds{wall.now_us}};
  slot.supply(std::move(issued.value().access),
              now + issued.value().expires_in);
  const auto leased = slot.lease();
  ASSERT_TRUE(leased);
  EXPECT_EQ(leased.value().view(), "mobile-access");
  wall.advance_ms(7'200'000);
  EXPECT_EQ(AuthRig::kind_of(slot.lease()), AuthErrorKind::expired);
}

}  // namespace
}  // namespace psn_test
