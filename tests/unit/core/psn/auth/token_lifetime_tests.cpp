#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::ExpirySource;
using libchiaking::psn::auth::token_ceiling_seconds;
using libchiaking::psn::auth::token_default_seconds;
using libchiaking::psn::auth::token_margin_seconds;
using libchiaking::psn::auth::TokenCache;
using Reply = ClientRig::Reply;

class CachedTokenLifetime : public ::testing::Test {
 protected:
  using Expiry = std::pair<ExpirySource, std::int64_t>;

  static auto expiry_after(std::string_view suffix) -> std::optional<Expiry> {
    CachedAuthRig fresh;
    fresh.owned_tokens(
        {AuthRig::redirect("https://x/#access_token=T" + std::string{suffix})});
    if (!fresh.owned()) {
      ADD_FAILURE() << "no token was issued for " << suffix;
      return std::nullopt;
    }
    const auto tokens = fresh.cache.snapshot().tokens;
    if (tokens.empty()) {
      ADD_FAILURE() << "no token was kept for " << suffix;
      return std::nullopt;
    }
    const auto& stats = tokens.front();
    return Expiry{stats.expiry_source,
                  *stats.expires_at_ms - *stats.obtained_at_ms};
  }

  static constexpr Expiry fallback{ExpirySource::fallback,
                                   std::int64_t{token_default_seconds} * 1000};

  CachedAuthRig rig;
};

TEST_F(CachedTokenLifetime, CoversASecondCall) {
  rig.owned_tokens(
      {AuthRig::redirect("https://x/#access_token=T1&expires_in=3600"),
       AuthRig::redirect("https://x/#access_token=T2")});
  const auto first = rig.owned();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().view(), "T1");
  EXPECT_FALSE(rig.auth.renewable(first.value()));
  const auto second = rig.owned();
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value().view(), "T1");
  EXPECT_TRUE(rig.auth.renewable(second.value()));
  EXPECT_EQ(rig.exchanges(), 1);
  ASSERT_FALSE(rig.http.seen.empty());
  EXPECT_EQ(rig.http.seen.front().header("Cookie"), "npsso=NPSSO-A");
}

TEST_F(CachedTokenLifetime, EndsAMarginBeforeItsExpiresIn) {
  rig.owned_tokens(
      {AuthRig::redirect("https://x/#access_token=T1&expires_in=600"),
       AuthRig::redirect("https://x/#access_token=T2&expires_in=600")});
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.clock.advance_ms((600 - 60) * 1000 - 1);
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.clock.advance_ms(1);
  EXPECT_EQ(rig.owned_view(), "T2");
  EXPECT_EQ(rig.exchanges(), 2);
}

TEST_F(CachedTokenLifetime, FallsBackToTheDefaultWithoutAnExpiresIn) {
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=T1"),
                    AuthRig::redirect("https://x/#access_token=T2")});
  EXPECT_EQ(rig.owned_view(), "T1");
  const auto tokens = rig.cache.snapshot().tokens;
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.front().expiry_source, ExpirySource::fallback);
  rig.clock.advance_ms(std::int64_t{token_default_seconds} * 1000 - 1);
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.clock.advance_ms(1);
  EXPECT_EQ(rig.owned_view(), "T2");
  EXPECT_EQ(rig.exchanges(), 2);
}

TEST_F(CachedTokenLifetime, IsCappedForAHugeExpiresIn) {
  rig.owned_tokens(
      {AuthRig::redirect("https://x/#access_token=T1&expires_in=999999"),
       AuthRig::redirect("https://x/#access_token=T2")});
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.clock.advance_ms(
      std::int64_t{token_ceiling_seconds - token_margin_seconds} * 1000);
  EXPECT_EQ(rig.owned_view(), "T2");
}

TEST_F(CachedTokenLifetime, LosesTheMarginFromItsExpiresIn) {
  EXPECT_EQ(TokenCache::lifetime_seconds(30), 0U);
  EXPECT_EQ(TokenCache::lifetime_seconds(60), 0U);
  EXPECT_EQ(TokenCache::lifetime_seconds(61), 1U);
  EXPECT_EQ(TokenCache::lifetime_seconds(0), token_default_seconds);
}

TEST_F(CachedTokenLifetime, IsNotKeptWhenTooShort) {
  rig.owned_tokens(
      {AuthRig::redirect("https://x/#access_token=T1&expires_in=30"),
       AuthRig::redirect("https://x/#access_token=T2")});
  EXPECT_EQ(rig.owned_view(), "T1");
  EXPECT_EQ(rig.owned_view(), "T2");
  EXPECT_EQ(rig.exchanges(), 2);
}

TEST_F(CachedTokenLifetime, FallsBackToTheDefaultForAMalformedExpiresIn) {
  EXPECT_EQ(expiry_after("&expires_in=12x"), fallback);
  EXPECT_EQ(expiry_after("&expires_in="), fallback);
  EXPECT_EQ(expiry_after("&expires_in=12345678901"), fallback);
  EXPECT_EQ(expiry_after("&expires_in=4294967295"),
            (Expiry{ExpirySource::expires_in,
                    std::int64_t{token_ceiling_seconds - token_margin_seconds} *
                        1000}));
  EXPECT_EQ(expiry_after("&expires_in=3599&token_type=bearer"),
            (Expiry{ExpirySource::expires_in, std::int64_t{3599 - 60} * 1000}));
}

TEST_F(CachedTokenLifetime, NeverStartsWithAFailedExchange) {
  rig.owned_tokens({AuthRig::redirect("https://x/#error=login_required"),
                    Reply::transport(),
                    AuthRig::redirect("https://x/#access_token=T1")});
  EXPECT_EQ(AuthRig::kind_of(rig.owned()), AuthErrorKind::refused);
  EXPECT_EQ(AuthRig::kind_of(rig.owned()), AuthErrorKind::transport);
  const auto ok = rig.owned();
  ASSERT_TRUE(ok);
  EXPECT_FALSE(rig.auth.renewable(ok.value()));
  const auto again = rig.owned();
  ASSERT_TRUE(again);
  EXPECT_TRUE(rig.auth.renewable(again.value()));
  EXPECT_EQ(rig.exchanges(), 3);
}

}  // namespace
}  // namespace psn_test
