#include "tests/unit/core/psn/auth/rig.hpp"
#include "tests/unit/core/psn/session_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::ExpirySource;
using libchiaking::psn::auth::TokenState;
using libchiaking::psn::auth::TokenStats;

class SessionStats : public ::testing::Test {
 protected:
  static constexpr std::int64_t start_ms = 1'700'000'000'000;

  auto owned()
      -> libchiaking::Expected<AuthRig::BearerToken, AuthRig::AuthError> {
    return auth.commerce_token(
        libchiaking::psn::auth::owned_entitlements_scope);
  }

  auto first_token() -> std::optional<TokenStats> {
    const auto tokens = rig.snapshot().tokens;
    if (tokens.empty()) return std::nullopt;
    return tokens.front();
  }

  void owned_tokens(std::vector<ClientRig::Reply> replies) {
    rig.http.on("GET", std::string{AuthRig::owned_url}, std::move(replies));
  }

  SessionRig rig;
  libchiaking::psn::auth::Authenticator auth{
      rig.client, libchiaking::psn::auth::Npsso{"NPSSO-SECRET"},
      &rig.internals().tokens()};
};

TEST_F(SessionStats, CountExchangesAndUses) {
  owned_tokens({AuthRig::redirect(
                    "https://x/#access_token=TOKEN-SECRET&expires_in=3600"),
                AuthRig::redirect("https://x/#access_token=TOKEN-TWO")});
  const auto first = owned();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().view(), "TOKEN-SECRET");
  auth.record_use(first.value(), 200);
  rig.clock.advance_ms(1000);
  rig.wall.advance_ms(1000);
  const auto second = owned();
  ASSERT_TRUE(second);
  EXPECT_TRUE(auth.renewable(second.value()));
  auth.record_use(second.value(), 403);
  rig.internals().tokens().record_retry(AuthRig::owned_grant());
  auto view = rig.snapshot();
  EXPECT_EQ(view.created_at_ms, start_ms);
  EXPECT_EQ(view.exchanges, 1U);
  EXPECT_EQ(view.uses, 2U);
  EXPECT_EQ(view.refusals, 1U);
  ASSERT_EQ(view.tokens.size(), 1U);
  const auto& stats = view.tokens.front();
  EXPECT_EQ(stats.label, "owned entitlements");
  EXPECT_EQ(stats.client_id, AuthRig::commerce_client);
  EXPECT_EQ(stats.scope, libchiaking::psn::auth::owned_entitlements_scope);
  EXPECT_EQ(stats.obtained_at_ms, start_ms);
  EXPECT_EQ(stats.expires_at_ms, start_ms + (3600 - 60) * 1000);
  EXPECT_EQ(stats.expiry_source, ExpirySource::expires_in);
  EXPECT_EQ(stats.state, TokenState::valid);
  EXPECT_EQ(stats.usage.exchanges, 1U);
  EXPECT_EQ(stats.usage.uses, 2U);
  EXPECT_EQ(stats.usage.refusals, 1U);
  EXPECT_EQ(stats.usage.retries, 1U);
  EXPECT_EQ(stats.usage.last_exchange_status, 302);
  EXPECT_EQ(stats.usage.last_use_status, 403);
  EXPECT_EQ(stats.last_used_at_ms, 1'700'000'001'000);

  auth.forget(second.value());
  const auto forgotten = first_token();
  ASSERT_TRUE(forgotten);
  EXPECT_EQ(forgotten->state, TokenState::invalidated);
  const auto renewed = owned();
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().view(), "TOKEN-TWO");
  view = rig.snapshot();
  ASSERT_FALSE(view.tokens.empty());
  EXPECT_EQ(view.tokens.front().state, TokenState::valid);
  EXPECT_EQ(view.tokens.front().expiry_source, ExpirySource::fallback);
  EXPECT_EQ(view.tokens.front().usage.exchanges, 2U);
  EXPECT_EQ(view.exchanges, 2U);
  EXPECT_EQ(view.tokens.front().usage.uses, 2U);

  const auto json = rig.session.snapshot_json().value();
  EXPECT_EQ(json.find("TOKEN-SECRET"), std::string::npos) << json;
  EXPECT_EQ(json.find("TOKEN-TWO"), std::string::npos) << json;
  EXPECT_EQ(json.find("NPSSO"), std::string::npos) << json;
  EXPECT_NE(json.find(R"("createdAtMs":1700000000000)"), std::string::npos)
      << json;
  EXPECT_NE(json.find(R"("totals":{"exchanges":2,"uses":2,"refusals":1})"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(R"("label":"owned entitlements")"), std::string::npos)
      << json;
  EXPECT_NE(
      json.find(
          R"("expirySource":"default","state":"valid","exchanges":2,"uses":2,"refusals":1,"retries":1,"lastExchangeStatus":302,"lastUseStatus":403,"lastUsedAtMs":1700000001000})"),
      std::string::npos)
      << json;
}

TEST_F(SessionStats, AreEmptiedByClearButKeepTheCreationTime) {
  owned_tokens({AuthRig::redirect("https://x/#access_token=T1")});
  const auto token = owned();
  ASSERT_TRUE(token);
  auth.record_use(token.value(), 403);
  rig.session.clear();
  const auto view = rig.snapshot();
  EXPECT_EQ(view.exchanges, 0U);
  EXPECT_EQ(view.uses, 0U);
  EXPECT_EQ(view.refusals, 0U);
  EXPECT_TRUE(view.tokens.empty());
  EXPECT_EQ(view.created_at_ms, start_ms);
  EXPECT_EQ(
      rig.session.snapshot_json().value(),
      R"({"createdAtMs":1700000000000,"totals":{"exchanges":0,"uses":0,"refusals":0},"tokens":[],"subscription":null,"graphql":[]})");
}

TEST_F(SessionStats, WriteNoDocumentForASubscriptionThatIsNotUtf8) {
  EXPECT_FALSE(rig.internals().snapshot_json("\"\xFF\""));
  EXPECT_TRUE(rig.internals().snapshot_json("\"ok\""));
}

TEST_F(SessionStats, TrackTokenExpiry) {
  owned_tokens(
      {AuthRig::redirect("https://x/#access_token=T1&expires_in=600"),
       AuthRig::redirect("https://x/#access_token=T2&expires_in=600")});
  EXPECT_TRUE(owned());
  auto stats = first_token();
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->state, TokenState::valid);
  rig.clock.advance_ms((600 - 60) * 1000);
  stats = first_token();
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->state, TokenState::expired);
  EXPECT_EQ(stats->usage.exchanges, 1U);
  const auto renewed = owned();
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().view(), "T2");
  stats = first_token();
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->state, TokenState::valid);
  EXPECT_EQ(stats->usage.exchanges, 2U);
}

TEST_F(SessionStats, RecordAFailedExchange) {
  owned_tokens({AuthRig::redirect("https://x/#error=login_required")});
  EXPECT_EQ(AuthRig::kind_of(owned()), AuthErrorKind::refused);
  const auto stats = first_token();
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->state, TokenState::none);
  EXPECT_EQ(stats->usage.exchanges, 1U);
  EXPECT_EQ(stats->usage.last_exchange_status, 302);
  EXPECT_FALSE(stats->obtained_at_ms);
  EXPECT_FALSE(stats->expires_at_ms);
  EXPECT_FALSE(stats->last_used_at_ms);
  const auto json = rig.session.snapshot_json().value();
  EXPECT_NE(json.find(R"("obtainedAtMs":null,"expiresAtMs":null)"),
            std::string::npos)
      << json;
}

}  // namespace
}  // namespace psn_test
