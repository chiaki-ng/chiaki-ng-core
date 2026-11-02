#include "src/core/users/ports/membership_calls.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/cloud/session_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class GrantProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    platform::HttpResponse response;
    std::string_view body;
    response.status_code = 200;
    const auto url = request.url.view();
    switch (calls) {
      case 1:
        EXPECT_NE(url.find("/client_ids"), std::string::npos);
        body =
            R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})";
        break;
      case 2:
        EXPECT_TRUE(url.ends_with("/config")) << url;
        body = R"({"configKey":"synthetic-key"})";
        break;
      case 3:
        EXPECT_NE(url.find("/sessions/start"), std::string::npos);
        body = R"({"sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"started-key"}});
        break;
      case 4:
      case 5:
        EXPECT_EQ(request.method, platform::HttpMethod::get);
        EXPECT_NE(
            url.find(calls == 4 ? "client_id=gk-id" : "client_id=server-id"),
            std::string::npos);
        EXPECT_GT(request.headers.size(), std::size_t{1});
        if (request.headers.size() > 1) {
          EXPECT_EQ(request.headers[1].value.view(), "npsso=synthetic-token");
        }
        response.status_code = fail_second_oauth && calls == 5 ? 500 : 302;
        response.headers.push_back(
            {"Location",
             libchiaking::SecretString{
                 calls == 4
                     ? (request.url.view().find("grc-response") !=
                                std::string_view::npos
                            ? std::string{"https://psnow.playstation.com/app/"
                                          "2.2.0/133/5cdcc037d/"
                                          "grc-response.html"}
                            : std::string{"gaikai://local"}) +
                           "?code=CLIENT"
                     : (request.url.view().find("grc-response") !=
                                std::string_view::npos
                            ? std::string{"https://psnow.playstation.com/app/"
                                          "2.2.0/133/5cdcc037d/"
                                          "grc-response.html"}
                            : std::string{"gaikai://local"}) +
                           "?code=SERVER"}});
        break;
      case 6:
        EXPECT_NE(url.find("/authorize"), std::string::npos);
        EXPECT_EQ(GatewayProvider::session_key(request), "started-key");
        response.status_code = psplus ? 403 : 200;
        if (psplus)
          response.headers.push_back(
              {"x-gaikai-event", libchiaking::SecretString{"002.2001"}});
        else
          response.headers.push_back(
              {"X-Gaikai-Session",
               libchiaking::SecretString{"authorized-key"}});
        break;
      case 7:
        EXPECT_NE(url.find("/lock?forceLogout=true"), std::string::npos);
        EXPECT_EQ(GatewayProvider::session_key(request), "authorized-key");
        body = lock_pending ? R"({"lockAcquired":false,"pollFrequency":5})"
                            : R"({"lockAcquired":true,"pollFrequency":5})";
        break;
      default:
        ADD_FAILURE() << "unexpected request " << calls << " to " << url;
    }
    for (char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    if (stop_after_call && calls == stop_at) stop_after_call->stopped = true;
    return response;
  }

  bool fail_second_oauth{};
  bool psplus{};
  bool lock_pending{};
  std::shared_ptr<test_support::StopState> stop_after_call;
  unsigned stop_at{};
  unsigned calls{};
};

class ReservationGrant : public ::testing::Test {
 protected:
  static constexpr std::string_view duid =
      "000000070041008000112233445566778899aabbccddeeff";

  static auto preauth() -> gk::LaunchSpec {
    return gk::LaunchSpec::cloud_stream(
               "synthetic-entitlement", gk::Language{"en-US"},
               gk::TimeZone::parse("UTC+00:00").value(), 1080)
        .value();
  }

  auto stopping_after(unsigned call) -> platform::StopToken {
    provider.stop_after_call = std::make_shared<test_support::StopState>();
    provider.stop_at = call;
    return platform::StopToken{provider.stop_after_call};
  }

  const platform::StopToken running;
  GrantProvider provider;
};

TEST_F(ReservationGrant, SignsInAuthorizesAndLocks) {
  Chain granted{provider, running};
  EXPECT_TRUE(granted.grant(preauth(), duid));
  EXPECT_EQ(provider.calls, 7U);
  EXPECT_EQ(granted.session().id().view(), "synthetic-session");
  EXPECT_EQ(granted.session().config_key().view(), "authorized-key");
  ASSERT_TRUE(granted.session().lock_key());
  EXPECT_EQ(granted.session().lock_key()->view(), "authorized-key");
  EXPECT_NE(granted.session().authorized_spec().view().find("CLIENT"),
            std::string_view::npos);
  EXPECT_NE(granted.session().authorized_spec().view().find("SERVER"),
            std::string_view::npos);
}

TEST_F(ReservationGrant, StopsAtARefusedServerCode) {
  provider.fail_second_oauth = true;
  Chain chain{provider, running};
  const auto grant = chain.grant(preauth(), duid);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 5U);
  EXPECT_EQ(grant.error().stage(), Stage::oauth);
  EXPECT_EQ(grant.error().detail(), 3U);
}

TEST_F(ReservationGrant, StopsAtAPlusRefusal) {
  provider.psplus = true;
  Chain chain{provider, running};
  EXPECT_TRUE(GkRig::failed(chain.grant(preauth(), duid), Stage::authorize,
                            Reason::psplus_rejected, 2));
  EXPECT_EQ(provider.calls, 6U);
}

class ReservationGrantMembership : public ReservationGrant {
 protected:
  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;
  static constexpr std::string_view subscriptions_url =
      "https://web.np.playstation.com/api/graphql/v1/"
      "op?operationName=getSubscriptions&variables=%7B%7D"
      "&extensions=%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%"
      "22sha256Hash%22%3A%22"
      "36c5aa5049115a8b37008e45815c3cd31414d52660cd94a45fcdb17294800db1%22%"
      "7D%7D";

  void SetUp() override {
    wall.now_us = static_cast<std::uint64_t>(wall_start_ms) * 1000U;
    auto saved =
        test_support::play::SignedInPlayer::saved("synthetic-token", duid);
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app-a"},
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{wall_start_ms + 3'600'000}});
    member.emplace(ps, monotonic, wall, std::move(saved));
    ps.on(
        "GET", std::string{subscriptions_url},
        {test_support::psn::Reply{
            200,
            R"({"data":{"fetchSubscriptions":{"subscriptions":[{"active":true,"endDate":"2027-01-01T00:00:00.000Z",)"
            R"("id":"IP9101-NPIA90006_00-PLUS3T12M","productName":"PlayStation Plus","subscriptionType":"PS_PLUS",)"
            R"("tierLabel":"TIER_30"}]}}})"}});
    const auto premium = member->user->get_my_membership();
    ASSERT_TRUE(premium);
    EXPECT_EQ(premium.value().membership.tier(),
              libchiaking::users::Tier::premium);
    EXPECT_TRUE(remembered());
  }

  auto remembered() -> bool {
    return static_cast<bool>(libchiaking::users::MembershipCalls{*member->user}
                                 .remembered_membership());
  }

  test_support::FakeClock monotonic;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp ps{&monotonic};
  std::optional<test_support::play::SignedInPlayer> member;
};

TEST_F(ReservationGrantMembership, KeepsTheMembershipWhenGranted) {
  Chain chain{provider, running};
  EXPECT_TRUE(chain.grant(preauth(), *member->user));
  EXPECT_EQ(provider.calls, 7U);
  EXPECT_TRUE(remembered());
}

TEST_F(ReservationGrantMembership, ForgetsTheMembershipOnAPlusRefusal) {
  provider.psplus = true;
  Chain chain{provider, running};
  const auto grant = chain.grant(preauth(), *member->user);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 6U);
  EXPECT_EQ(grant.error().stage(), Stage::authorize);
  EXPECT_EQ(grant.error().reason(), Reason::psplus_rejected);
  EXPECT_FALSE(remembered());
  EXPECT_FALSE(member->user->membership().tier());
  EXPECT_EQ(member->user->sign_in_state(),
            libchiaking::users::SignInState::signed_in);
}

TEST_F(ReservationGrant, IsCancelledWhenThePendingLockCannotWait) {
  provider.lock_pending = true;
  Chain chain{provider, running};
  const auto grant = chain.grant(preauth(), duid);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 7U);
  EXPECT_EQ(grant.error().stage(), Stage::lock);
  EXPECT_TRUE(grant.error().is_cancelled());
  EXPECT_EQ(grant.error().detail(), 1U);
}

TEST_F(ReservationGrant, StopsAtTheCodesWhenStoppedAfterOpening) {
  Chain chain{provider, stopping_after(3)};
  const auto grant = chain.grant(preauth(), duid);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(grant.error().stage(), Stage::oauth);
}

TEST_F(ReservationGrant, StopsAtTheAuthorizationWhenStoppedAfterTheCodes) {
  Chain chain{provider, stopping_after(5)};
  const auto grant = chain.grant(preauth(), duid);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 5U);
  EXPECT_EQ(grant.error().stage(), Stage::authorize);
}

TEST_F(ReservationGrant, SendsNothingOnceStopped) {
  Chain chain{provider,
              platform::StopToken{std::make_shared<test_support::Stopped>()}};
  const auto grant = chain.grant(preauth(), duid);
  ASSERT_FALSE(grant);
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(grant.error().stage(), Stage::client_ids);
}

}  // namespace
}  // namespace cloud_test
