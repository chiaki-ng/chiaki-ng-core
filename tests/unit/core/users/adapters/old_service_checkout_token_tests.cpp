#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/adapters/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace users_test {
namespace {

namespace psn = libchiaking::psn;
namespace users = libchiaking::users;
using Step = OldServiceRig::Step;
using Sent = OldServiceRig::Sent;
using test_support::JsonTree;

class OldServiceCheckoutToken : public ::testing::Test {
 protected:
  static auto number(JsonTree::Tree& view, std::string_view group,
                     std::string_view key) -> std::int64_t {
    return JsonTree::number(view[group][key]);
  }

  static auto first_token(JsonTree::Tree& view, std::string_view key)
      -> JsonTree::Tree& {
    return view["tokens"][0][key];
  }

  OldServicePlayer player;
  ScriptedPeer peer;
};

TEST_F(OldServiceCheckoutToken, IsKeptBetweenLaunches) {
  peer.steps = OldServiceRig::full_steps(false);
  for (auto& step :
       OldServiceRig::without_token(OldServiceRig::full_steps(false)))
    peer.steps.push_back(std::move(step));
  peer.steps.back().check = nullptr;
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
  EXPECT_EQ(peer.calls, peer.steps.size());
  EXPECT_EQ(OldServiceRig::token_count(peer), 1);
  EXPECT_EQ(OldServiceRig::code_count(peer), 4);
  auto view = player.snapshot();
  EXPECT_EQ(number(view, "totals", "exchanges"), 1);
  EXPECT_EQ(number(view, "totals", "uses"), 4);
  EXPECT_EQ(number(view, "totals", "refusals"), 0);
  ASSERT_EQ(JsonTree::count(view["tokens"]), 1U);
  EXPECT_EQ(JsonTree::text(first_token(view, "label")), "ps now commerce");
  EXPECT_EQ(JsonTree::text(first_token(view, "clientId")),
            "dc523cc2-b51b-4190-bff0-3397c06871b3");
  EXPECT_EQ(JsonTree::number(first_token(view, "lastExchangeStatus")), 302);
  EXPECT_EQ(JsonTree::number(first_token(view, "lastUseStatus")), 200);
}

TEST_F(OldServiceCheckoutToken, BelongsToOneAccount) {
  peer.steps = OldServiceRig::full_steps(false);
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
  OldServicePlayer other{8};
  ScriptedPeer apart;
  apart.steps = OldServiceRig::full_steps(false);
  EXPECT_TRUE(OldServiceRig::launch(apart, *other.user).finished());
  EXPECT_EQ(OldServiceRig::token_count(apart), 1);
  EXPECT_EQ(apart.calls, apart.steps.size());
}

TEST_F(OldServiceCheckoutToken, IsRenewedOnceWhenAKeptTokenIsRefused) {
  peer.steps = OldServiceRig::full_steps(false);
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
  {
    ScriptedPeer renewing;
    renewing.steps =
        OldServiceRig::without_token(OldServiceRig::full_steps(false));
    renewing.steps[3].status = 401;
    renewing.steps[3].check = OldServiceRig::bearer("COMMERCE");
    const auto fresh = OldServiceRig::oauth("access_token", "COMMERCE2",
                                            "response_type=token");
    Sent attributes = OldServiceRig::full_steps(false)[4];
    attributes.check = OldServiceRig::bearer("COMMERCE2");
    renewing.steps.insert(renewing.steps.begin() + 4, {fresh, attributes});
    renewing.steps[6].check = OldServiceRig::bearer("COMMERCE2");
    EXPECT_TRUE(OldServiceRig::launch(renewing, *player.user).finished());
    EXPECT_EQ(renewing.calls, renewing.steps.size());
    EXPECT_EQ(OldServiceRig::token_count(renewing), 1);
    auto view = player.snapshot();
    EXPECT_EQ(number(view, "totals", "exchanges"), 2);
    EXPECT_EQ(number(view, "totals", "refusals"), 1);
    EXPECT_EQ(number(view, "totals", "uses"), 2 + 3);
    EXPECT_EQ(JsonTree::number(first_token(view, "retries")), 1);
  }
  {
    ScriptedPeer renewing;
    renewing.steps =
        OldServiceRig::without_token(OldServiceRig::full_steps(false));
    renewing.steps[3].check = OldServiceRig::bearer("COMMERCE2");
    renewing.steps[4].status = 403;
    renewing.steps[4].check = OldServiceRig::bearer("COMMERCE2");
    const auto fresh = OldServiceRig::oauth("access_token", "COMMERCE3",
                                            "response_type=token");
    Sent licence = OldServiceRig::full_steps(false)[5];
    licence.check = OldServiceRig::bearer("COMMERCE3");
    renewing.steps.insert(renewing.steps.begin() + 5, {fresh, licence});
    EXPECT_TRUE(OldServiceRig::launch(renewing, *player.user).finished());
    EXPECT_EQ(renewing.calls, renewing.steps.size());
    EXPECT_EQ(OldServiceRig::token_count(renewing), 1);
  }
  {
    ScriptedPeer renewing;
    renewing.steps =
        OldServiceRig::without_token(OldServiceRig::full_steps(false));
    renewing.steps.resize(4);
    renewing.steps[3].status = 401;
    renewing.steps[3].check = OldServiceRig::bearer("COMMERCE3");
    renewing.steps.push_back(OldServiceRig::oauth("access_token", "COMMERCE4",
                                                  "response_type=token"));
    Sent attributes = OldServiceRig::full_steps(false)[4];
    attributes.status = 401;
    attributes.body = "{}";
    attributes.check = OldServiceRig::bearer("COMMERCE4");
    renewing.steps.push_back(attributes);
    const auto launched = OldServiceRig::launch(renewing, *player.user);
    ASSERT_TRUE(launched.refusal);
    EXPECT_EQ(launched.refusal->step(), Step::account_attributes);
    EXPECT_EQ(launched.refusal->http_status(), 401);
    EXPECT_EQ(renewing.calls, renewing.steps.size());
    EXPECT_EQ(OldServiceRig::token_count(renewing), 1);
  }
}

TEST_F(OldServiceCheckoutToken, IsNotRenewedWhenAFreshTokenIsRefused) {
  peer.steps = OldServiceRig::full_steps(false);
  peer.steps[4].status = 401;
  peer.steps[4].body = "{}";
  peer.steps.resize(5);
  const auto launched = OldServiceRig::launch(peer, *player.user);
  ASSERT_TRUE(launched.refusal);
  EXPECT_EQ(launched.refusal->step(), Step::account_attributes);
  EXPECT_EQ(launched.refusal->http_status(), 401);
  EXPECT_EQ(peer.calls, 5U);
  EXPECT_EQ(OldServiceRig::token_count(peer), 1);
}

TEST_F(OldServiceCheckoutToken, IsNotKeptWhenItFails) {
  peer.steps = OldServiceRig::full_steps(false);
  peer.steps[3].headers[0].value =
      "https://psnow.playstation.com/#error=login_required";
  peer.steps.resize(4);
  const auto launched = OldServiceRig::launch(peer, *player.user);
  ASSERT_TRUE(launched.refusal);
  EXPECT_EQ(launched.refusal->step(), Step::checkout_token);
  ASSERT_TRUE(launched.refusal->authorisation());
  EXPECT_EQ(launched.refusal->authorisation()->kind(),
            psn::auth::AuthErrorKind::refused);
  EXPECT_EQ(launched.refusal->http_status(), 302);
  EXPECT_TRUE(launched.refusal->expires_sign_in());
  EXPECT_EQ(player.user->sign_in_state(), users::SignInState::expired);
  EXPECT_TRUE(player.user->is_expired());
  ScriptedPeer again;
  again.steps = OldServiceRig::full_steps(false);
  EXPECT_TRUE(OldServiceRig::launch(again, *player.user).finished());
  EXPECT_EQ(OldServiceRig::token_count(again), 1);
}

TEST_F(OldServiceCheckoutToken, ServesTheLibrary) {
  peer.steps = OldServiceRig::full_steps(false);
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
  ScriptedPeer library;
  library.steps = {{OldServiceRig::Method::get,
                    "/internal_entitlements/"
                    "EP0000-CUSA12345_00-STREAM?fields=game_meta%2Csku_meta",
                    200,
                    R"({"id":"EP0000-CUSA12345_00-STREAM"})",
                    {},
                    OldServiceRig::bearer("COMMERCE")}};
  auto http = OldServiceRig::client(library);
  const auto record = users::CatalogCalls{*player.user}.library_record(
      http, OldServiceRig::entitlement);
  ASSERT_TRUE(record);
  EXPECT_TRUE(record.value().entitlement);
  EXPECT_EQ(library.calls, 1U);
  EXPECT_EQ(OldServiceRig::token_count(library), 0);
  auto view = player.snapshot();
  EXPECT_EQ(number(view, "totals", "exchanges"), 1);
  ASSERT_EQ(JsonTree::count(view["tokens"]), 1U);
  EXPECT_EQ(JsonTree::text(first_token(view, "label")), "ps now commerce");
  EXPECT_EQ(JsonTree::number(first_token(view, "uses")), 3);
}

TEST_F(OldServiceCheckoutToken, IsFetchedOnceWhenFirstNeeded) {
  peer.steps = {
      OldServiceRig::oauth("access_token", "COMMERCE", "response_type=token"),
      OldServiceRig::full_steps(false)[5], OldServiceRig::full_steps(false)[4]};
  peer.steps[1].check = OldServiceRig::bearer("COMMERCE");
  auto http = OldServiceRig::client(peer);
  auto checkout = users::PlayCalls{*player.user}.old_service_checkout(http);
  ASSERT_TRUE(checkout);
  EXPECT_EQ(peer.calls, 0U);
  const auto status =
      checkout.value().licence_status(OldServiceRig::entitlement);
  ASSERT_TRUE(status);
  EXPECT_EQ(status.value(), 200);
  EXPECT_EQ(peer.calls, 2U);
  EXPECT_TRUE(checkout.value().account_attributes());
  EXPECT_EQ(peer.calls, 3U);
  EXPECT_EQ(OldServiceRig::token_count(peer), 1);
  auto moved = std::move(checkout.value());
  peer.steps.push_back(OldServiceRig::full_steps(false)[5]);
  EXPECT_TRUE(moved.licence_status(OldServiceRig::entitlement));
  EXPECT_EQ(peer.calls, 4U);
  EXPECT_EQ(OldServiceRig::token_count(peer), 1);
}

TEST_F(OldServiceCheckoutToken, IsNotFetchedForAnUnusedCheckout) {
  auto http = OldServiceRig::client(peer);
  const auto unused = users::PlayCalls{*player.user}.old_service_checkout(http);
  EXPECT_TRUE(unused);
  EXPECT_EQ(peer.calls, 0U);
}

TEST_F(OldServiceCheckoutToken,
       RejectedOnceRefusesEveryLaterStepWithoutAsking) {
  auto denied =
      OldServiceRig::oauth("access_token", "COMMERCE", "response_type=token");
  denied.status = 200;
  peer.steps = {denied};
  auto http = OldServiceRig::client(peer);
  auto checkout = users::PlayCalls{*player.user}.old_service_checkout(http);
  ASSERT_TRUE(checkout);
  const auto first = checkout.value().account_attributes();
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error().step(), Step::checkout_token);
  EXPECT_EQ(peer.calls, 1U);
  ASSERT_TRUE(first.error().authorisation());
  EXPECT_EQ(first.error().authorisation()->kind(),
            psn::auth::AuthErrorKind::rejected);
  EXPECT_EQ(first.error().http_status(), 200);
  psn::rest::KamajiClient store{http};
  const auto second =
      checkout.value().licence_status(OldServiceRig::entitlement);
  const auto third = checkout.value().preview(store, "SKU-A");
  const auto fourth = checkout.value().buy(store, "SKU-A");
  ASSERT_FALSE(second);
  EXPECT_FALSE(third);
  ASSERT_FALSE(fourth);
  EXPECT_EQ(peer.calls, 1U);
  EXPECT_EQ(second.error().step(), Step::checkout_token);
  EXPECT_EQ(fourth.error().http_status(), 200);
  EXPECT_TRUE(player.user->is_expired());
}

}  // namespace
}  // namespace users_test
