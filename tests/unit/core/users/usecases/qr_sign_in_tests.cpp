#include "src/core/users/signed_in_users_core.hpp"
#include "src/core/users/usecases/qr_sign_in.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace platform = libchiaking::platform;
using test_support::psn::Reply;
using test_support::psn::Seen;
using users::QrSignInOptions;
using users::QrSignInState;
using users::QrSignInStep;
using users::SignInErrorKind;

class QrSignInRig {
 public:
  static constexpr std::string_view page_url =
      "https://my.account.sony.com/sonyacct/signin/";
  static constexpr std::string_view bundle_url =
      "https://my.account.sony.com/sonyacct/signin/dsb_signin_1/assets/"
      "app.bundle.js";
  static constexpr std::string_view ticket_url =
      "https://ca.account.sony.com/api/authn/v3/sso/ticket/webSignin";
  static constexpr std::string_view poll_url =
      "https://ca.account.sony.com/api/authn/v3/sso";
  static constexpr std::string_view page =
      R"(<script src="dsb_signin_1/assets/app.bundle.js"></script>)";
  static constexpr std::string_view bundle =
      R"(x={passwordFreeSignin:{clientId:"PT1BWnAxQ2R1Vldhc05XTGpsR2RsaEdkdWwzYw==",)"
      R"(clientSecret:"PVFYWnlOV1p6MUNkdVZXYXNOV0xqbEdkbGhHZHVsM2M=",)"
      R"(grantType:"client_credentials",)"
      R"(scopes:["oauth:create_authn_ticket_for_web_signin"]}})";
  static constexpr std::array<std::string_view, 6> secrets{
      "synthetic-client-secret", "synthetic-app-token", "synthetic-app-token-2",
      "synthetic-handle-1",      "synthetic-handle-2",  "synthetic-npsso"};

  static auto application_token(std::string_view token) -> Reply {
    return Reply{200, std::string{R"({"access_token":")"} + std::string{token} +
                          R"(","token_type":"bearer","expires_in":3599})"};
  }

  static auto ticket(std::string_view handle, std::string_view code,
                     int expires_in = 300) -> Reply {
    Reply reply{201, std::string{R"({"authentication_ticket":")"} +
                         std::string{handle} + R"(","user_code":")" +
                         std::string{code} + R"(","interval":5,"expires_in":)" +
                         std::to_string(expires_in) + "}"};
    reply.headers.push_back({"X-Psn-Correlation-Id", "synthetic-correlation"});
    return reply;
  }

  static auto pending() -> Reply {
    return Reply{400, R"({"error_code":5104})"};
  }

  static auto refused(int code) -> Reply {
    return Reply{400, R"({"error_code":)" + std::to_string(code) + "}"};
  }

  static auto approved() -> Reply {
    return Reply{200, R"({"npsso":"synthetic-npsso"})"};
  }

  static auto handle_of(const Seen& seen) -> std::string {
    const auto at = seen.body.find("synthetic-handle-");
    if (at == std::string::npos) return {};
    return seen.body.substr(at, 18);
  }

  QrSignInRig() { wall.now_us = 1'900'000'000'000'000; }

  void registered(std::vector<Reply> tokens) {
    http.on("GET", std::string{page_url}, {Reply{200, std::string{page}}});
    http.on("GET", std::string{bundle_url}, {Reply{200, std::string{bundle}}});
    tokens.push_back(Reply{
        200,
        R"({"access_token":"remote-access","refresh_token":"remote-refresh","expires_in":3600,"token_type":"bearer"})"});
    tokens.push_back(Reply{
        200,
        R"({"access_token":"mobile-access","refresh_token":"mobile-refresh","expires_in":7200})"});
    http.on("POST", std::string{Psn::token_url}, std::move(tokens));
    http.on("GET", std::string{Psn::remote_authorize_url},
            {Psn::redirect("https://remoteplay.dl.playstation.net/remoteplay/"
                           "redirect?code=remote%2Bcode")});
    http.on(
        "GET", std::string{Psn::account_url},
        {Reply{
            200,
            R"({"user_id":"578437695752307201","online_id":"Tokyo Player"})"}});
  }

  void tickets(std::vector<Reply> replies) {
    http.on("POST", std::string{ticket_url}, std::move(replies));
  }

  void polled(std::vector<Reply> replies) {
    http.on("POST", std::string{poll_url}, std::move(replies));
  }

  auto run(platform::StopToken stop = {}, const QrSignInOptions& options = {})
      -> libchiaking::Expected<std::shared_ptr<users::PsUser>,
                               users::SignInError> {
    return qr.run(
        Psn::own_device(), Psn::correlation, stop,
        [this](const QrSignInState& state) { states.push_back(state.clone()); },
        options);
  }

  auto sent_to(std::string_view url) const -> std::vector<Seen> {
    std::vector<Seen> found;
    std::ranges::copy_if(http.seen, std::back_inserter(found),
                         [url](const Seen& seen) { return seen.url == url; });
    return found;
  }

  auto polls() const -> std::vector<Seen> { return sent_to(poll_url); }

  auto leaked() const -> bool {
    return std::ranges::any_of(secrets, [this](std::string_view secret) {
      return log.contains(secret);
    });
  }

  test_support::FakeClock steady;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp http;
  test_support::FakeLog log;
  test_support::FakeWaiter waiter{&steady};
  test_support::users::SignedIn signed_in{http, steady, wall};
  users::usecases::QrSignIn qr{
      http, steady, wall, signed_in.users.core().sign_in(), waiter.factory(),
      &log};
  std::vector<QrSignInState> states;
};

class QrSignIn : public ::testing::Test {
 protected:
  void starts_with_one_code(std::string_view code) {
    rig.registered({QrSignInRig::application_token("synthetic-app-token")});
    rig.tickets({QrSignInRig::ticket("synthetic-handle-1", code)});
  }

  QrSignInRig rig;
};

TEST_F(QrSignIn, ApprovesAfterPendingAndKeepsTheUser) {
  starts_with_one_code("004211");
  rig.polled({QrSignInRig::pending(), QrSignInRig::approved()});
  const auto user = rig.run();
  ASSERT_TRUE(user);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_EQ(user.value()->online_id(), "Tokyo Player");
  EXPECT_EQ(rig.signed_in.users.find(user.value()->account_id()), user.value());
  const auto kept = rig.signed_in.credentials.saved(user.value()->account_id());
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "synthetic-npsso");
  ASSERT_TRUE(kept->device);
  EXPECT_EQ(kept->device->view(), Psn::duid);
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{5000, 5000}));

  const auto shown_until =
      std::chrono::system_clock::time_point{
          std::chrono::microseconds{rig.wall.now_us}} +
      std::chrono::seconds{300};
  std::vector<QrSignInState> expected;
  expected.emplace_back(1, QrSignInStep::preparing, libchiaking::SecretString{},
                        libchiaking::SecretString{}, std::nullopt,
                        std::nullopt);
  expected.emplace_back(
      1, QrSignInStep::awaiting_approval,
      libchiaking::SecretString{
          "https://id.sonyentertainmentnetwork.com/id/PSsignin/"
          "?code=004211&cid=synthetic-correlation"
          "&service_entity=urn%3Aservice-entity%3Apsn"
          "&elements_visibility=psportal"},
      libchiaking::SecretString{"004211"}, shown_until, std::nullopt);
  expected.emplace_back(
      1, QrSignInStep::signing_in, libchiaking::SecretString{},
      libchiaking::SecretString{}, std::nullopt, std::nullopt);
  expected.emplace_back(1, QrSignInStep::signed_in, libchiaking::SecretString{},
                        libchiaking::SecretString{}, std::nullopt,
                        std::nullopt);
  EXPECT_EQ(rig.states, expected);
  const auto polls = rig.polls();
  ASSERT_EQ(polls.size(), 2U);
  EXPECT_EQ(polls[0].header("Authorization"), "Bearer synthetic-app-token");
  EXPECT_FALSE(rig.leaked());
}

TEST_F(QrSignIn, WaitsAsLongAsARateLimitAsks) {
  starts_with_one_code("123456");
  Reply slow{429, ""};
  slow.headers.push_back({"Retry-After", "12"});
  Reply brief{429, ""};
  brief.headers.push_back({"Retry-After", "2"});
  rig.polled({slow, brief, QrSignInRig::approved()});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{5000, 12000, 5000}));
}

TEST_F(QrSignIn, FailsWhenARateLimitStatesNoWait) {
  starts_with_one_code("123456");
  rig.polled({Reply{429, ""}});
  const auto limited = rig.run();
  ASSERT_FALSE(limited);
  EXPECT_EQ(limited.error().kind(), SignInErrorKind::rate_limited);
  ASSERT_FALSE(rig.states.empty());
  EXPECT_EQ(rig.states.back().step(), QrSignInStep::failed);
  EXPECT_EQ(rig.states.back().failure(), SignInErrorKind::rate_limited);
}

TEST_F(QrSignIn, ReplacesAnExpiredCodeAndForgetsTheOldTicket) {
  rig.registered({QrSignInRig::application_token("synthetic-app-token")});
  rig.tickets({QrSignInRig::ticket("synthetic-handle-1", "111111", 10),
               QrSignInRig::ticket("synthetic-handle-2", "222222")});
  rig.polled({QrSignInRig::pending(), QrSignInRig::approved()});
  EXPECT_TRUE(rig.run());
  const auto polls = rig.polls();
  ASSERT_EQ(polls.size(), 2U);
  EXPECT_EQ(QrSignInRig::handle_of(polls[0]), "synthetic-handle-1");
  EXPECT_EQ(QrSignInRig::handle_of(polls[1]), "synthetic-handle-2");
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{5000, 5000, 5000}));
  std::vector<std::pair<std::uint32_t, QrSignInStep>> steps;
  for (const auto& state : rig.states)
    steps.emplace_back(state.attempt(), state.step());
  const std::vector<std::pair<std::uint32_t, QrSignInStep>> expected{
      {1, QrSignInStep::preparing},  {1, QrSignInStep::awaiting_approval},
      {2, QrSignInStep::preparing},  {2, QrSignInStep::awaiting_approval},
      {2, QrSignInStep::signing_in}, {2, QrSignInStep::signed_in}};
  EXPECT_EQ(steps, expected);
  ASSERT_GE(rig.states.size(), 4U);
  EXPECT_EQ(rig.states[1].user_code(), "111111");
  EXPECT_TRUE(rig.states[2].user_code().empty());
  EXPECT_TRUE(rig.states[2].approval_url().empty());
  EXPECT_EQ(rig.states[3].user_code(), "222222");
  EXPECT_EQ(rig.http.count("webSignin"), 2);
}

TEST_F(QrSignIn, ReplacesAnInvalidatedTicket) {
  rig.registered({QrSignInRig::application_token("synthetic-app-token")});
  rig.tickets({QrSignInRig::ticket("synthetic-handle-1", "111111"),
               QrSignInRig::ticket("synthetic-handle-2", "222222")});
  rig.polled({QrSignInRig::refused(4621), QrSignInRig::approved()});
  EXPECT_TRUE(rig.run());
  const auto polls = rig.polls();
  ASSERT_GE(polls.size(), 2U);
  EXPECT_EQ(QrSignInRig::handle_of(polls[1]), "synthetic-handle-2");
  ASSERT_GE(rig.states.size(), 4U);
  EXPECT_EQ(rig.states[3].attempt(), 2U);
}

TEST_F(QrSignIn, RenewsTheApplicationTokenForVerification) {
  rig.registered({QrSignInRig::application_token("synthetic-app-token"),
                  QrSignInRig::application_token("synthetic-app-token-2")});
  rig.tickets({QrSignInRig::ticket("synthetic-handle-1", "111111"),
               QrSignInRig::ticket("synthetic-handle-2", "222222")});
  rig.polled({QrSignInRig::refused(4097), QrSignInRig::approved()});
  EXPECT_TRUE(rig.run());
  const auto tickets = rig.sent_to(QrSignInRig::ticket_url);
  ASSERT_EQ(tickets.size(), 2U);
  EXPECT_EQ(tickets[0].header("Authorization"), "Bearer synthetic-app-token");
  EXPECT_EQ(tickets[1].header("Authorization"), "Bearer synthetic-app-token-2");
  const auto polls = rig.polls();
  ASSERT_GE(polls.size(), 2U);
  EXPECT_EQ(polls[1].header("Authorization"), "Bearer synthetic-app-token-2");
  EXPECT_FALSE(rig.leaked());
}

TEST_F(QrSignIn, EndsWhenDeclined) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::pending(), QrSignInRig::refused(5106)});
  const auto declined = rig.run();
  ASSERT_FALSE(declined);
  EXPECT_EQ(declined.error().kind(), SignInErrorKind::declined);
  ASSERT_FALSE(rig.states.empty());
  EXPECT_EQ(rig.states.back().step(), QrSignInStep::failed);
  EXPECT_EQ(rig.states.back().failure(), SignInErrorKind::declined);
  EXPECT_TRUE(rig.states.back().approval_url().empty());
  EXPECT_EQ(rig.http.count("authorize"), 0);
  EXPECT_TRUE(rig.signed_in.users.everyone().empty());
}

TEST_F(QrSignIn, BoundsTheReplacementsOfExpiredCodes) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::refused(5102)});
  const auto lapsed = rig.run();
  ASSERT_FALSE(lapsed);
  EXPECT_EQ(lapsed.error().kind(), SignInErrorKind::expired);
  EXPECT_EQ(rig.http.count("webSignin"), 10);
  ASSERT_FALSE(rig.states.empty());
  EXPECT_EQ(rig.states.back().attempt(), 10U);
  EXPECT_EQ(rig.states.back().failure(), SignInErrorKind::expired);
}

TEST_F(QrSignIn, BoundsTheRenewalsForVerification) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::refused(4097)});
  const auto unverified = rig.run();
  ASSERT_FALSE(unverified);
  EXPECT_EQ(unverified.error().kind(), SignInErrorKind::verification_required);
  EXPECT_EQ(rig.http.count("webSignin"), 10);
  EXPECT_EQ(rig.http.count("oauth/token"), 10);
}

TEST_F(QrSignIn, CancelsBetweenPolls) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::pending()});
  auto state = std::make_shared<test_support::StopState>();
  rig.waiter.stop_on_wait = 2;
  rig.waiter.stop_state = state.get();
  const auto stopped = rig.run(platform::StopToken{state});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().kind(), SignInErrorKind::cancelled);
  EXPECT_EQ(rig.polls().size(), 1U);
  EXPECT_EQ(rig.waiter.waits.size(), 2U);
  ASSERT_FALSE(rig.states.empty());
  EXPECT_EQ(rig.states.back().step(), QrSignInStep::failed);
  EXPECT_EQ(rig.states.back().failure(), SignInErrorKind::cancelled);
  EXPECT_EQ(rig.http.count("authorize"), 0);
}

TEST_F(QrSignIn, NamesTheDeviceAsTheWebSignInDoesByDefault) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::approved()});
  EXPECT_TRUE(rig.run());
  const auto tickets = rig.sent_to(QrSignInRig::ticket_url);
  ASSERT_EQ(tickets.size(), 1U);
  EXPECT_EQ(tickets[0].body, R"({"device_name":"my device"})");
}

TEST_F(QrSignIn, SendsTheChosenDeviceName) {
  starts_with_one_code("111111");
  rig.polled({QrSignInRig::approved()});
  EXPECT_TRUE(rig.run({}, QrSignInOptions{}.device_name("Living room Switch")));
  const auto tickets = rig.sent_to(QrSignInRig::ticket_url);
  ASSERT_EQ(tickets.size(), 1U);
  EXPECT_EQ(tickets[0].body, R"({"device_name":"Living room Switch"})");
}

TEST_F(QrSignIn, RefusesAnUnusableDeviceNameBeforeSending) {
  const auto unusable = rig.run({}, QrSignInOptions{}.device_name(""));
  ASSERT_FALSE(unusable);
  EXPECT_EQ(unusable.error().kind(), SignInErrorKind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
  ASSERT_FALSE(rig.states.empty());
  EXPECT_EQ(rig.states.back().step(), QrSignInStep::failed);
  EXPECT_EQ(rig.states.back().failure(), SignInErrorKind::invalid_request);
}

TEST_F(QrSignIn, RefusesAnUnusableCorrelationBeforeSending) {
  const auto unusable = rig.qr.run(Psn::own_device(), "not-a-uuid", {}, {});
  ASSERT_FALSE(unusable);
  EXPECT_EQ(unusable.error().kind(), SignInErrorKind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST(QrSignInThroughTheRegistry, RefusesAnUnusableCorrelationBeforeSending) {
  test_support::users::SignedIn registry;
  std::vector<QrSignInState> states;
  const auto direct =
      registry.users.sign_in_with_qr(Psn::own_device(), "not-a-uuid", {},
                                     [&states](const QrSignInState& state) {
                                       states.push_back(state.clone());
                                     });
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().kind(), SignInErrorKind::invalid_request);
  EXPECT_EQ(registry.offline.requests, 0);
  ASSERT_EQ(states.size(), 2U);
  EXPECT_EQ(states.back().step(), QrSignInStep::failed);
}

}  // namespace
}  // namespace users_test
