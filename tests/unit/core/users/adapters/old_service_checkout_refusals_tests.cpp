#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/adapters/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace users_test {
namespace {

namespace psn = libchiaking::psn;
namespace users = libchiaking::users;
using Step = OldServiceRig::Step;
using Kind = psn::auth::AuthErrorKind;

class OldServiceCheckoutRefusals : public ::testing::Test {
 protected:
  static void expect_title_store_refused(std::uint16_t status,
                                         std::string_view location, Kind kind,
                                         bool expired) {
    OldServicePlayer player;
    ScriptedPeer peer;
    auto code = OldServiceRig::oauth("code", "ANON");
    code.status = status;
    code.headers[0].value = std::string{location};
    peer.steps = {code};
    auto http = OldServiceRig::client(peer);
    const auto store =
        users::PlayCalls{*player.user}.old_service_title_store(http);
    ASSERT_FALSE(store);
    EXPECT_EQ(store.error().step(), Step::title_code);
    EXPECT_EQ(peer.calls, 1U);
    ASSERT_TRUE(store.error().authorisation());
    EXPECT_EQ(store.error().http_status(), status);
    EXPECT_EQ(store.error().authorisation()->kind(), kind);
    EXPECT_EQ(player.user->is_expired(), expired);
  }

  OldServicePlayer player;
  ScriptedPeer peer;
};

TEST_F(OldServiceCheckoutRefusals, ReportMissingAccountAttributesInFull) {
  peer.steps = OldServiceRig::full_steps(false);
  peer.steps[4].status = 403;
  peer.steps[4].body =
      R"({"error":{"validationErrors":[{"missingElements":[{"name":"BIRTH_DATE"},{"name":"CITY"}]},{"missingElements":[{"name":"REAL_NAME"}]}]}})";
  const auto launched = OldServiceRig::launch(peer, *player.user);
  ASSERT_TRUE(launched.refusal);
  EXPECT_EQ(launched.refusal->step(), Step::account_attributes);
  EXPECT_EQ(launched.refusal->http_status(), 403);
  EXPECT_EQ(peer.calls, 5U);
  const auto missing = launched.refusal->missing_account_attributes();
  ASSERT_EQ(missing.size(), 3U);
  EXPECT_EQ(missing[0], "BIRTH_DATE");
  EXPECT_EQ(missing[1], "CITY");
  EXPECT_EQ(missing[2], "REAL_NAME");
  EXPECT_TRUE(std::holds_alternative<psn::rest::AccountsError>(
      launched.refusal->cause()));
  EXPECT_FALSE(launched.refusal->is_cancelled());
  EXPECT_FALSE(launched.refusal->expires_sign_in());
  EXPECT_FALSE(player.user->is_expired());
  EXPECT_TRUE(player.user->is_signed_in());
}

TEST_F(OldServiceCheckoutRefusals,
       NameNoAccountAttributesWhenTheReplySaysNone) {
  peer.steps = OldServiceRig::full_steps(false);
  peer.steps[4].status = 403;
  peer.steps[4].body = "{}";
  const auto launched = OldServiceRig::launch(peer, *player.user);
  ASSERT_TRUE(launched.refusal);
  EXPECT_EQ(launched.refusal->step(), Step::account_attributes);
  EXPECT_EQ(launched.refusal->http_status(), 403);
  EXPECT_TRUE(launched.refusal->missing_account_attributes().empty());
}

TEST_F(OldServiceCheckoutRefusals, AreAvoidedBySkippingAccountAttributes) {
  peer.steps = OldServiceRig::full_steps(false);
  peer.steps.erase(peer.steps.begin() + 4);
  const auto launched = OldServiceRig::launch(
      peer, *player.user, {.skip_account_attributes = true});
  EXPECT_TRUE(launched.finished());
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST(OldServiceCheckoutRefusalsInSequence, StopAtAnUnreadablePreview) {
  for (const std::string_view cart :
       {R"({"total_price_value":1})", R"({"total_price_value":0.1})",
        R"({"total_price_value":"unknown"})", R"([])"}) {
    SCOPED_TRACE(cart);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[6].body =
        std::string{R"({"data":{"cart":)"} + std::string{cart} + "}}";
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_EQ(peer.calls, 7U);
    if (launched.refusal) {
      EXPECT_EQ(launched.refusal->step(), Step::preview);
    } else {
      ASSERT_TRUE(launched.preview);
      EXPECT_FALSE(launched.preview->free);
    }
  }
}

TEST(OldServiceCheckoutRefusalsInSequence, StopTheSequenceWhereTheyHappen) {
  for (const std::size_t bad : {1U, 2U, 4U, 5U, 6U, 7U, 9U}) {
    SCOPED_TRACE(bad);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[bad].status = 403;
    peer.steps[bad].headers.clear();
    peer.steps[bad].body = "{}";
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_EQ(peer.calls, bad + 1);
    EXPECT_FALSE(player.user->is_expired());
    EXPECT_TRUE(player.user->is_signed_in());
    switch (bad) {
      case 1:
        ASSERT_TRUE(launched.refusal);
        EXPECT_EQ(launched.refusal->step(), Step::title_store);
        EXPECT_TRUE(
            std::holds_alternative<users::OldServiceRefusal::NoStoreSession>(
                launched.refusal->cause()));
        EXPECT_EQ(launched.refusal->http_status(), 403);
        break;
      case 2:
        ASSERT_TRUE(launched.title);
        EXPECT_EQ(launched.title->http_status(), 403);
        break;
      case 4:
        ASSERT_TRUE(launched.refusal);
        EXPECT_EQ(launched.refusal->step(), Step::account_attributes);
        EXPECT_EQ(launched.refusal->http_status(), 403);
        break;
      case 5:
        EXPECT_FALSE(launched.refusal);
        ASSERT_TRUE(launched.licence);
        EXPECT_EQ(*launched.licence, 403);
        break;
      case 6:
        ASSERT_TRUE(launched.refusal);
        EXPECT_EQ(launched.refusal->step(), Step::preview);
        ASSERT_TRUE(launched.refusal->store());
        EXPECT_EQ(launched.refusal->store()->reason(),
                  psn::rest::StoreError::Reason::refused);
        EXPECT_EQ(launched.refusal->http_status(), 403);
        break;
      case 7:
        ASSERT_TRUE(launched.refusal);
        EXPECT_EQ(launched.refusal->step(), Step::buy);
        EXPECT_TRUE(launched.refusal->store());
        EXPECT_EQ(launched.refusal->http_status(), 403);
        break;
      default:
        EXPECT_FALSE(launched.refusal);
        EXPECT_FALSE(launched.account_accepted);
        break;
    }
  }
}

TEST_F(OldServiceCheckoutRefusals, OfTheTitleCodeExpireOnlyARefusedSignIn) {
  {
    SCOPED_TRACE("rejected");
    expect_title_store_refused(200, "https://x/", Kind::rejected, true);
  }
  {
    SCOPED_TRACE("refused");
    expect_title_store_refused(302, "https://x/#error=login_required",
                               Kind::refused, true);
  }
  {
    SCOPED_TRACE("server error");
    expect_title_store_refused(503, "https://x/", Kind::server_error, false);
  }
  {
    SCOPED_TRACE("malformed");
    expect_title_store_refused(302, "https://x/", Kind::malformed, false);
  }
}

TEST_F(OldServiceCheckoutRefusals, OfTheAccountCodeExpireTheUser) {
  auto code = OldServiceRig::oauth("code", "AUTH");
  code.status = 401;
  peer.steps = {code};
  auto http = OldServiceRig::client(peer);
  psn::rest::KamajiClient store{http};
  const auto account =
      users::PlayCalls{*player.user}.old_service_account_session(
          http, store, psn::StreamingPlatform::ps3);
  ASSERT_FALSE(account);
  EXPECT_EQ(account.error().step(), Step::account_code);
  EXPECT_EQ(account.error().http_status(), 401);
  ASSERT_TRUE(account.error().authorisation());
  EXPECT_EQ(account.error().authorisation()->kind(), Kind::rejected);
  EXPECT_TRUE(player.user->is_expired());
  ASSERT_FALSE(peer.urls.empty());
  EXPECT_NE(peer.urls.front().find("scope=kamaji:commerce_native&"),
            std::string::npos)
      << peer.urls.front();
}

}  // namespace
}  // namespace users_test
