#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/unit/core/cloud/links/services/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
using Cause = play::OldServiceLaunch::Error::Cause;
using LaunchStage = play::OldServiceLaunch::Error::Stage;
using Method = OldServiceRig::Method;
using Rig = OldServiceRig;

class OldServiceSession : public ::testing::Test {
 protected:
  auto resolve(ScriptedPeer& peer) -> Rig::Resolved {
    auto request = Rig::config(peer);
    request.user = player.user;
    return Rig::resolve(request, peer, false);
  }

  auto snapshot() const {
    return libchiaking::psn::SessionInternals{player.session()}.snapshot();
  }

  OldServicePlayer player;
};

TEST_F(OldServiceSession, ReusesTheCommerceToken) {
  ScriptedPeer peer;
  peer.steps = Rig::full_steps(false);
  for (auto& step : Rig::without_token(Rig::full_steps(false)))
    peer.steps.push_back(std::move(step));
  peer.steps.back().check = nullptr;
  EXPECT_TRUE(resolve(peer));
  EXPECT_TRUE(resolve(peer));
  EXPECT_EQ(peer.calls, peer.steps.size());
  EXPECT_EQ(Rig::token_count(peer), 1);
  EXPECT_EQ(Rig::code_count(peer), 4);
  const auto view = snapshot();
  EXPECT_EQ(view.exchanges, 1U);
  EXPECT_EQ(view.uses, 4U);
  EXPECT_EQ(view.refusals, 0U);
  ASSERT_EQ(view.tokens.size(), std::size_t{1});
  EXPECT_EQ(view.tokens.front().label, "ps now commerce");
  EXPECT_EQ(view.tokens.front().client_id,
            "dc523cc2-b51b-4190-bff0-3397c06871b3");
  EXPECT_EQ(view.tokens.front().usage.last_exchange_status, 302);
  EXPECT_EQ(view.tokens.front().usage.last_use_status, 200);
  const auto json = player.user->snapshot_json().value();
  EXPECT_EQ(json.find("COMMERCE"), std::string::npos);
  EXPECT_EQ(json.find("SYNTHETIC"), std::string::npos);
}

TEST(OldServiceSessionless, ExchangesATokenForEveryLaunch) {
  ScriptedPeer plain;
  plain.steps = Rig::full_steps(false);
  for (auto& step : Rig::full_steps(false))
    plain.steps.push_back(std::move(step));
  const auto request = Rig::config(plain);
  EXPECT_TRUE(Rig::resolve(request, plain, false));
  EXPECT_TRUE(Rig::resolve(request, plain, false));
  EXPECT_EQ(Rig::token_count(plain), 2);
}

TEST(OldServiceSessionWire, MatchesTheSessionlessWire) {
  for (const bool acquire : {false, true}) {
    SCOPED_TRACE(acquire);
    OldServicePlayer player;
    ScriptedPeer with;
    with.steps = Rig::full_steps(acquire);
    auto held = Rig::config(with);
    held.user = player.user;
    ScriptedPeer without;
    without.steps = Rig::full_steps(acquire);
    EXPECT_TRUE(Rig::resolve(held, with, false));
    EXPECT_TRUE(Rig::resolve(Rig::config(without), without, false));
    EXPECT_EQ(with.wire, without.wire);
  }
}

class OldServiceRefusedToken : public OldServiceSession {
 protected:
  void SetUp() override {
    ScriptedPeer first;
    first.steps = Rig::full_steps(false);
    ASSERT_TRUE(resolve(first));
  }

  auto retries_the_refused_token() -> testing::AssertionResult {
    ScriptedPeer peer;
    peer.steps = Rig::without_token(Rig::full_steps(false));
    peer.steps[3].status = 401;
    peer.steps[3].check = Rig::bearer("COMMERCE");
    auto fresh = Rig::oauth("access_token", "COMMERCE2", "response_type=token");
    auto attributes = Rig::full_steps(false)[4];
    attributes.check = Rig::bearer("COMMERCE2");
    peer.steps.insert(peer.steps.begin() + 4, {fresh, attributes});
    peer.steps[6].check = Rig::bearer("COMMERCE2");
    if (!resolve(peer))
      return testing::AssertionFailure() << "the retry failed";
    if (peer.calls != peer.steps.size())
      return testing::AssertionFailure() << peer.calls << " steps ran";
    if (Rig::token_count(peer) != 1)
      return testing::AssertionFailure()
             << Rig::token_count(peer) << " tokens were exchanged";
    return testing::AssertionSuccess();
  }

  auto retries_a_refused_entitlement() -> testing::AssertionResult {
    ScriptedPeer peer;
    peer.steps = Rig::without_token(Rig::full_steps(false));
    peer.steps[3].check = Rig::bearer("COMMERCE2");
    peer.steps[4].status = 403;
    peer.steps[4].check = Rig::bearer("COMMERCE2");
    auto fresh = Rig::oauth("access_token", "COMMERCE3", "response_type=token");
    auto entitlement = Rig::full_steps(false)[5];
    entitlement.check = Rig::bearer("COMMERCE3");
    peer.steps.insert(peer.steps.begin() + 5, {fresh, entitlement});
    if (!resolve(peer))
      return testing::AssertionFailure() << "the retry failed";
    if (peer.calls != peer.steps.size())
      return testing::AssertionFailure() << peer.calls << " steps ran";
    if (Rig::token_count(peer) != 1)
      return testing::AssertionFailure()
             << Rig::token_count(peer) << " tokens were exchanged";
    return testing::AssertionSuccess();
  }
};

TEST_F(OldServiceRefusedToken, ExchangesItOnceAndRetries) {
  EXPECT_TRUE(retries_the_refused_token());
  const auto view = snapshot();
  EXPECT_EQ(view.exchanges, 2U);
  EXPECT_EQ(view.refusals, 1U);
  EXPECT_EQ(view.uses, 2U + 3U);
  ASSERT_FALSE(view.tokens.empty());
  EXPECT_EQ(view.tokens.front().usage.retries, 1U);
}

TEST_F(OldServiceRefusedToken, RetriesARefusedEntitlementLookup) {
  ASSERT_TRUE(retries_the_refused_token());
  EXPECT_TRUE(retries_a_refused_entitlement());
}

TEST_F(OldServiceRefusedToken, StopsWhenTheFreshTokenIsRefusedToo) {
  ASSERT_TRUE(retries_the_refused_token());
  ASSERT_TRUE(retries_a_refused_entitlement());
  ScriptedPeer peer;
  peer.steps = Rig::without_token(Rig::full_steps(false));
  peer.steps.resize(4);
  peer.steps[3].status = 401;
  peer.steps[3].check = Rig::bearer("COMMERCE3");
  peer.steps.push_back(
      Rig::oauth("access_token", "COMMERCE4", "response_type=token"));
  auto attributes = Rig::full_steps(false)[4];
  attributes.status = 401;
  attributes.body = "{}";
  attributes.check = Rig::bearer("COMMERCE4");
  peer.steps.push_back(attributes);
  const auto result = resolve(peer);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause(), Cause::privacy_required);
  EXPECT_EQ(result.error().http_status(), 401);
  EXPECT_EQ(peer.calls, peer.steps.size());
  EXPECT_EQ(Rig::token_count(peer), 1);
}

TEST(OldServiceFreshToken, StopsWhenItIsRefused) {
  ScriptedPeer peer;
  peer.steps = Rig::full_steps(false);
  peer.steps[4].status = 401;
  peer.steps[4].body = "{}";
  peer.steps.resize(5);
  OldServicePlayer another;
  auto request = Rig::config(peer);
  request.user = another.user;
  const auto result = Rig::resolve(request, peer, false);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause(), Cause::privacy_required);
  EXPECT_EQ(peer.calls, std::size_t{5});
  EXPECT_EQ(Rig::token_count(peer), 1);
}

TEST_F(OldServiceSession, KeepsNoFailedToken) {
  ScriptedPeer peer;
  peer.steps = Rig::full_steps(false);
  peer.steps[3].headers[0].value =
      "https://psnow.playstation.com/#error=login_required";
  peer.steps.resize(4);
  const auto result = resolve(peer);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause(), Cause::invalid_response);
  EXPECT_EQ(result.error().stage(), LaunchStage::commerce_oauth);
  ScriptedPeer again;
  again.steps = Rig::full_steps(false);
  EXPECT_TRUE(resolve(again));
  EXPECT_EQ(Rig::token_count(again), 1);
}

TEST_F(OldServiceSession, LendsTheCommerceTokenToTheCatalog) {
  ScriptedPeer peer;
  peer.steps = Rig::full_steps(false);
  ASSERT_TRUE(resolve(peer));
  ScriptedPeer catalog;
  catalog.steps = {{Method::get,
                    "/internal_entitlements/"
                    "EP0000-CUSA12345_00-STREAM?fields=game_meta%2Csku_meta",
                    200,
                    R"({"id":"EP0000-CUSA12345_00-STREAM"})",
                    {},
                    Rig::bearer("COMMERCE")}};
  const libchiaking::platform::StopToken stop{catalog.stop};
  libchiaking::platform::SteadyClock clock;
  libchiaking::net::http::Client library{catalog, clock, stop};
  const auto outcome =
      libchiaking::users::CatalogCalls{*player.user}.library_record(
          library, "EP0000-CUSA12345_00-STREAM");
  ASSERT_TRUE(outcome);
  EXPECT_TRUE(outcome.value().entitlement);
  EXPECT_EQ(catalog.calls, std::size_t{1});
  EXPECT_EQ(Rig::token_count(catalog), 0);
  const auto view = snapshot();
  EXPECT_EQ(view.exchanges, 1U);
  ASSERT_EQ(view.tokens.size(), std::size_t{1});
  EXPECT_EQ(view.tokens.front().label, "ps now commerce");
  EXPECT_EQ(view.tokens.front().usage.uses, 3U);
}

}  // namespace
}  // namespace cloud_test
