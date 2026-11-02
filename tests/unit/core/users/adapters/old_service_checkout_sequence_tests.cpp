#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/adapters/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace users_test {
namespace {

namespace psn = libchiaking::psn;
namespace users = libchiaking::users;
using Step = OldServiceRig::Step;

static_assert(!std::is_copy_constructible_v<users::OldServiceCheckout>);
static_assert(std::is_move_constructible_v<users::OldServiceCheckout>);
static_assert(
    !std::is_constructible_v<users::OldServiceCheckout, users::PsUser&,
                             libchiaking::net::http::Client&>);

TEST(OldServiceCheckoutSequence, IsAskedWholeAsTheUser) {
  for (const bool acquire : {false, true}) {
    SCOPED_TRACE(acquire ? "acquiring" : "already licensed");
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps(acquire);
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_TRUE(launched.finished());
    ASSERT_TRUE(launched.licence);
    EXPECT_EQ(*launched.licence, acquire ? 404 : 200);
    EXPECT_EQ(peer.calls, peer.steps.size());
    EXPECT_FALSE(player.user->is_expired());
    EXPECT_TRUE(player.user->is_signed_in());
  }
}

TEST(OldServiceCheckoutSequence, AcceptsALargeTitleContainer) {
  OldServicePlayer player;
  ScriptedPeer peer;
  peer.steps = OldServiceRig::full_steps(false);
  std::string padded{OldServiceRig::container};
  padded.pop_back();
  padded += ",\"long_desc\":\"" + std::string(48 * 1024, 'd') +
            "\",\"playable_platform\":[\"PS4\"]}\n";
  peer.steps[2].body = padded;
  peer.steps[2].check = [](const OldServiceRig::Request& request) {
    EXPECT_GE(request.maximum_response_bytes, 2U * 1024U * 1024U);
  };
  EXPECT_TRUE(OldServiceRig::launch(peer, *player.user).finished());
}

TEST(OldServiceCheckoutSequence, NeedsOnlyTheAccountSessionForAnOwnedTitle) {
  for (const bool owned : {true, false}) {
    SCOPED_TRACE(owned ? "owned" : "not owned");
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    if (owned) peer.steps.erase(peer.steps.begin(), peer.steps.end() - 2);
    const auto launched =
        OldServiceRig::launch(peer, *player.user, {.owned = owned});
    EXPECT_TRUE(launched.finished());
    EXPECT_EQ(launched.licence.has_value(), !owned);
    EXPECT_EQ(peer.calls, peer.steps.size());
  }
}

TEST(OldServiceCheckoutSequence, StopsWhereItIsCancelled) {
  for (std::size_t stop_at = 0; stop_at < OldServiceRig::full_steps().size();
       ++stop_at) {
    SCOPED_TRACE(stop_at);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.cancel_after = stop_at;
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_TRUE(launched.cancelled());
    EXPECT_EQ(peer.calls, stop_at + 1);
    EXPECT_FALSE(player.user->is_expired());
    EXPECT_TRUE(player.user->is_signed_in());
  }
}

TEST(OldServiceCheckoutSequence, StopsWhereTheTransportFails) {
  for (std::size_t fail_at = 0; fail_at < OldServiceRig::full_steps().size();
       ++fail_at) {
    SCOPED_TRACE(fail_at);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.fail_at = fail_at;
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_FALSE(launched.cancelled());
    EXPECT_EQ(peer.calls, fail_at + 1);
    if (launched.refusal) {
      EXPECT_TRUE(launched.refusal->failure() ||
                  launched.refusal->authorisation());
      if (launched.refusal->failure()) {
        EXPECT_EQ(launched.refusal->failure()->kind(),
                  libchiaking::net::http::Failure::Kind::transport);
      }
      if (launched.refusal->authorisation()) {
        EXPECT_EQ(launched.refusal->authorisation()->kind(),
                  psn::auth::AuthErrorKind::transport);
      }
    }
    EXPECT_FALSE(player.user->is_expired());
    EXPECT_TRUE(player.user->is_signed_in());
  }
}

TEST(OldServiceCheckoutSequence, SendsNothingWhenAlreadyStopped) {
  OldServicePlayer player;
  ScriptedPeer peer;
  peer.stop->stopped = true;
  const auto launched = OldServiceRig::launch(peer, *player.user);
  EXPECT_FALSE(launched.finished());
  EXPECT_TRUE(launched.cancelled());
  EXPECT_EQ(peer.calls, 0U);
  ASSERT_TRUE(launched.refusal);
  EXPECT_EQ(launched.refusal->step(), Step::title_code);
}

TEST(OldServiceCheckoutSequence, AnnouncesEveryRequestBeforeItIsSent) {
  OldServicePlayer player;
  ScriptedPeer peer;
  peer.steps = OldServiceRig::full_steps();
  std::vector<Step> announced;
  peer.steps[0].check = [&](const OldServiceRig::Request&) {
    EXPECT_EQ(announced, std::vector<Step>{Step::title_code});
  };
  peer.steps[1].check = [&](const OldServiceRig::Request&) {
    EXPECT_EQ(announced,
              (std::vector<Step>{Step::title_code, Step::title_store}));
  };
  peer.steps[8].check = [&](const OldServiceRig::Request&) {
    ASSERT_EQ(announced.size(), 3U);
    EXPECT_EQ(announced.back(), Step::account_code);
  };
  peer.steps[9].check = [&](const OldServiceRig::Request&) {
    ASSERT_EQ(announced.size(), 4U);
    EXPECT_EQ(announced.back(), Step::account_session);
  };
  EXPECT_TRUE(
      OldServiceRig::launch(peer, *player.user, {.announced = &announced})
          .finished());
  EXPECT_EQ(announced,
            (std::vector<Step>{Step::title_code, Step::title_store,
                               Step::account_code, Step::account_session}));
}

TEST(OldServiceCheckoutSequence, SignsAHomeUserOut) {
  OldServicePlayer player;
  const auto added = player.signed_in.users.add_home_user(
      libchiaking::AccountId{90}, "Living Room Player");
  ASSERT_TRUE(added);
  ScriptedPeer peer;
  auto http = OldServiceRig::client(peer);
  users::PlayCalls calls{*added.value()};
  std::vector<Step> announced;
  const std::function<void(Step)> announce = [&announced](Step step) {
    announced.push_back(step);
  };
  const auto store = calls.old_service_title_store(http, announce);
  ASSERT_FALSE(store);
  EXPECT_TRUE(store.error().is_signed_out());
  EXPECT_EQ(store.error().step(), Step::title_code);
  EXPECT_FALSE(store.error().is_cancelled());
  EXPECT_FALSE(store.error().expires_sign_in());
  EXPECT_EQ(store.error().http_status(), 0);
  psn::rest::KamajiClient unopened{
      http.limited(libchiaking::json::max_document_bytes)};
  const auto account = calls.old_service_account_session(
      http, unopened, psn::StreamingPlatform::ps4, announce);
  ASSERT_FALSE(account);
  EXPECT_TRUE(account.error().is_signed_out());
  EXPECT_EQ(account.error().step(), Step::account_code);
  const auto checkout = calls.old_service_checkout(http);
  ASSERT_FALSE(checkout);
  EXPECT_TRUE(checkout.error().is_signed_out());
  EXPECT_EQ(checkout.error().step(), Step::checkout_token);
  EXPECT_EQ(peer.calls, 0U);
  EXPECT_TRUE(announced.empty());
  EXPECT_EQ(added.value()->sign_in_state(), users::SignInState::signed_out);
}

}  // namespace
}  // namespace users_test
