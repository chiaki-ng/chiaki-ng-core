#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace psn_test {
namespace {

using libchiaking::psn::auth::Authenticator;
using libchiaking::psn::auth::AuthError;
using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::Exchanged;
using libchiaking::psn::auth::Grant;
using libchiaking::psn::auth::GrantKey;
using libchiaking::psn::auth::Npsso;
using libchiaking::psn::auth::owned_entitlements_scope;
using libchiaking::psn::auth::ResponseType;
using libchiaking::psn::auth::TokenCache;

class CachedTokenSharing : public ::testing::Test {
 protected:
  CachedAuthRig rig;
};

TEST_F(CachedTokenSharing, RefusesACodeGrant) {
  test_support::FakeClock clock;
  TokenCache cache{Npsso{"NPSSO-A"}, ClientRig::threading(), clock, clock};
  auto grant = AuthRig::owned_grant();
  grant.response = ResponseType::code;
  const auto refused = cache.acquire(
      grant, {}, ClientRig::StopToken{},
      [](const Npsso&) -> libchiaking::Expected<Exchanged, AuthError> {
        ADD_FAILURE() << "a code grant reached the exchange";
        return libchiaking::unexpected(AuthError{AuthErrorKind::transport});
      });
  EXPECT_EQ(AuthRig::kind_of(refused), AuthErrorKind::invalid_request);
}

TEST_F(CachedTokenSharing, IsKeyedOnlyByClientScopesAndResponse) {
  rig.checkout_tokens({AuthRig::redirect("https://x/#access_token=T1"),
                       AuthRig::redirect("https://x/#access_token=T2")},
                      AuthRig::gk_duid);
  rig.checkout_tokens({AuthRig::redirect("https://x/#access_token=T9")},
                      AuthRig::other_duid);
  const auto first = rig.checkout(AuthRig::gk_duid);
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().view(), "T1");
  EXPECT_FALSE(rig.auth.renewable(first.value()));
  const auto other_duid = rig.checkout(AuthRig::other_duid);
  ASSERT_TRUE(other_duid);
  EXPECT_EQ(other_duid.value().view(), "T1");
  EXPECT_TRUE(rig.auth.renewable(other_duid.value()));
  EXPECT_EQ(rig.exchanges(), 1);

  const Grant reordered{
      std::string{AuthRig::commerce_client},
      "kamaji:s2s.subscriptionsPremium.get user:account.settings.privacy.get "
      "kamaji:get_privacy_settings "
      "user:account.attributes.validate kamaji:get_internal_entitlements",
      ResponseType::token};
  const auto reused = rig.cache.acquire(
      reordered, "other agent", rig.stop_token, AuthRig::issued("NEVER"));
  ASSERT_TRUE(reused);
  EXPECT_TRUE(reused.value().cached);
  EXPECT_EQ(reused.value().token.view(), "T1");
  EXPECT_EQ(rig.exchanges(), 1);

  auto narrower = AuthRig::owned_grant();
  narrower.client_id = "other-client";
  const auto other =
      rig.cache.acquire(narrower, {}, rig.stop_token, AuthRig::issued("OTHER"));
  ASSERT_TRUE(other);
  EXPECT_FALSE(other.value().cached);
  EXPECT_EQ(other.value().token.view(), "OTHER");
}

TEST_F(CachedTokenSharing, ServesASubsetRequestFromASupersetToken) {
  rig.checkout_tokens(
      {AuthRig::redirect("https://x/#access_token=WIDE&expires_in=3600")});
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=NARROW")});
  const auto wide = rig.checkout();
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide.value().view(), "WIDE");
  EXPECT_FALSE(rig.auth.renewable(wide.value()));
  const auto narrow = rig.owned();
  ASSERT_TRUE(narrow);
  EXPECT_TRUE(rig.auth.renewable(narrow.value()));
  EXPECT_EQ(narrow.value().view(), "WIDE");
  EXPECT_EQ(rig.exchanges(), 1);
  rig.auth.record_use(narrow.value(), 200);
  const auto view = rig.cache.snapshot();
  ASSERT_EQ(view.tokens.size(), 1U);
  EXPECT_EQ(view.tokens.front().usage.uses, 1U);
  EXPECT_EQ(view.tokens.front().label, "ps now commerce");
  EXPECT_EQ(view.tokens.front().scope, AuthRig::checkout_scope);

  rig.auth.forget(narrow.value());
  const auto renewed = rig.owned();
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().view(), "NARROW");
  EXPECT_FALSE(rig.auth.renewable(renewed.value()));
  EXPECT_EQ(rig.http.seen.back().url, AuthRig::owned_url);
}

TEST_F(CachedTokenSharing, NeverWidensARequestWithASubsetToken) {
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=NARROW")});
  rig.checkout_tokens({AuthRig::redirect("https://x/#access_token=WIDE")});
  EXPECT_EQ(rig.owned_view(), "NARROW");
  const auto wide = rig.checkout();
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide.value().view(), "WIDE");
  EXPECT_FALSE(rig.auth.renewable(wide.value()));
  EXPECT_EQ(rig.exchanges(), 2);
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(rig.http.seen[0].url, AuthRig::owned_url);
  EXPECT_EQ(rig.http.seen[0].url.find("get_privacy_settings"),
            std::string::npos);
  EXPECT_EQ(rig.http.seen[1].url, AuthRig::checkout_url(AuthRig::gk_duid));
  const auto narrow = rig.cache.acquire(
      AuthRig::owned_grant(), {}, rig.stop_token, AuthRig::issued("NEVER"));
  ASSERT_TRUE(narrow);
  EXPECT_TRUE(narrow.value().cached);
  EXPECT_EQ(narrow.value().token.view(), "NARROW");
  EXPECT_EQ(GrantKey::of(narrow.value().grant),
            GrantKey::of(AuthRig::owned_grant()));
  EXPECT_EQ(rig.exchanges(), 2);
}

TEST_F(CachedTokenSharing, LetsTheCatalogAndResolverShareTheCommerceToken) {
  Authenticator resolver{rig.client, Npsso{"NPSSO-A"}, &rig.cache};
  Authenticator catalog{rig.client, Npsso{"NPSSO-A"}, &rig.cache};
  rig.checkout_tokens(
      {AuthRig::redirect("https://x/#access_token=SHARED&expires_in=3600")});
  const auto checkout =
      resolver.legacy_cloud_stream_checkout_token(AuthRig::gk_duid);
  ASSERT_TRUE(checkout);
  EXPECT_FALSE(resolver.renewable(checkout.value()));
  auto owned = catalog.commerce_token(owned_entitlements_scope);
  ASSERT_TRUE(owned);
  EXPECT_TRUE(catalog.renewable(owned.value()));
  EXPECT_EQ(owned.value().view(), "SHARED");
  catalog.record_use(owned.value(), 200);
  resolver.record_use(checkout.value(), 204);
  EXPECT_EQ(rig.exchanges(), 1);
  const auto view = rig.cache.snapshot();
  EXPECT_EQ(view.exchanges, 1U);
  EXPECT_EQ(view.uses, 2U);
  ASSERT_EQ(view.tokens.size(), 1U);
  EXPECT_EQ(view.tokens.front().client_id, AuthRig::commerce_client);
  EXPECT_TRUE(rig.log.contains("token cache miss"));
  EXPECT_TRUE(rig.log.contains("token cache hit"));
  EXPECT_FALSE(rig.log.contains("SHARED"));
  EXPECT_FALSE(rig.log.contains("NPSSO-A"));

  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=OWNED")});
  const auto renewed = catalog.refresh(owned.value(), 401);
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().view(), "OWNED");
  EXPECT_FALSE(catalog.renewable(renewed.value()));
  EXPECT_EQ(rig.http.seen.back().url, AuthRig::owned_url);
  EXPECT_TRUE(rig.log.contains("refreshing once"));
  const auto after = rig.cache.snapshot();
  EXPECT_EQ(after.tokens.size(), 2U);
  std::uint64_t retries = 0;
  for (const auto& token : after.tokens) retries += token.usage.retries;
  EXPECT_EQ(retries, 1U);
}

TEST_F(CachedTokenSharing, NeverCrossesAccounts) {
  CachedAuthRig second{"NPSSO-B"};
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=TA")});
  second.owned_tokens({AuthRig::redirect("https://x/#access_token=TB")});
  EXPECT_EQ(rig.owned_view(), "TA");
  EXPECT_EQ(second.owned_view(), "TB");
  EXPECT_EQ(rig.owned_view(), "TA");
  EXPECT_EQ(second.owned_view(), "TB");
  ASSERT_EQ(rig.http.seen.size(), 1U);
  ASSERT_EQ(second.http.seen.size(), 1U);
  EXPECT_EQ(rig.http.seen[0].header("Cookie"), "npsso=NPSSO-A");
  EXPECT_EQ(second.http.seen[0].header("Cookie"), "npsso=NPSSO-B");
  EXPECT_TRUE(rig.cache.matches("NPSSO-A"));
  EXPECT_FALSE(rig.cache.matches("NPSSO-B"));
  EXPECT_FALSE(rig.cache.matches("NPSSO-"));
  EXPECT_FALSE(rig.cache.matches(""));
}

TEST_F(CachedTokenSharing, ExchangesWithTheAccountOfTheCache) {
  test_support::FakeClock clock;
  TokenCache cache{Npsso{"NPSSO-CACHE"}, ClientRig::threading(), clock, clock};
  ClientRig net;
  Authenticator auth{net.client, Npsso{"NPSSO-OTHER"}, &cache};
  net.http.on("GET", std::string{AuthRig::owned_url},
              {AuthRig::redirect("https://x/#access_token=T")});
  EXPECT_TRUE(auth.commerce_token(owned_entitlements_scope));
  ASSERT_FALSE(net.http.seen.empty());
  EXPECT_EQ(net.http.seen.front().header("Cookie"), "npsso=NPSSO-CACHE");
}

TEST_F(CachedTokenSharing, LabelsEachTokenByItsFlow) {
  test_support::FakeClock clock;
  test_support::FakeClock wall;
  TokenCache cache{Npsso{"NPSSO-FAKE"}, ClientRig::threading(), clock, wall};
  ClientRig net;
  Authenticator auth{net.client, Npsso{"NPSSO-FAKE"}, &cache};
  net.http.on("GET", std::string{AuthRig::owned_url},
              {AuthRig::redirect("https://x/#access_token=OWNED")});
  net.http.on("GET", AuthRig::checkout_url(AuthRig::gk_duid),
              {AuthRig::redirect("https://x/#access_token=CHECKOUT")});
  EXPECT_TRUE(auth.commerce_token(owned_entitlements_scope));
  EXPECT_TRUE(auth.legacy_cloud_stream_checkout_token(AuthRig::gk_duid));
  const auto view = cache.snapshot();
  EXPECT_EQ(view.tokens.size(), 2U);
  bool owned = false;
  bool checkout = false;
  for (const auto& token : view.tokens) {
    EXPECT_EQ(token.client_id, "dc523cc2-b51b-4190-bff0-3397c06871b3");
    owned = owned || (token.label == "owned entitlements" &&
                      token.scope == owned_entitlements_scope);
    checkout = checkout || (token.label == "ps now commerce" &&
                            token.scope == AuthRig::checkout_scope);
  }
  EXPECT_TRUE(owned);
  EXPECT_TRUE(checkout);
}

}  // namespace
}  // namespace psn_test
