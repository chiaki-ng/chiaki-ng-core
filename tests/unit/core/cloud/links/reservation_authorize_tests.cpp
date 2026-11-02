#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class AuthorizeThenLockProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.headers.size(), std::size_t{5});
    if (request.headers.size() > 4)
      seen_keys.emplace_back(request.headers[4].value.view());
    platform::HttpResponse response;
    response.status_code = 200;
    if (calls == 1) {
      EXPECT_NE(request.url.view().find("/authorize"), std::string::npos);
      if (psplus) {
        response.status_code = 403;
        response.headers.push_back(
            {"x-gaikai-event", libchiaking::SecretString{"002.2001"}});
      } else if (entitlement) {
        response.status_code = 403;
        constexpr std::string_view body =
            R"({"name":"noGameForEntitlementId"})";
        for (const char character : body)
          response.body.push_back(static_cast<std::byte>(character));
      } else if (rotate) {
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"rotated-key"}});
      }
    } else {
      EXPECT_EQ(calls, 2U);
      EXPECT_NE(request.url.view().find("/lock?forceLogout=true"),
                std::string::npos);
      if (lock_reject) response.status_code = 500;
      const std::string_view body =
          lock_pending ? R"({"lockAcquired":false,"pollFrequency":5})"
                       : R"({"lockAcquired":true,"pollFrequency":5})";
      for (const char character : body)
        response.body.push_back(static_cast<std::byte>(character));
    }
    return response;
  }

  bool rotate{true};
  bool psplus{};
  bool entitlement{};
  bool lock_reject{};
  bool lock_pending{};
  unsigned calls{};
  std::vector<std::string> seen_keys;
};

class ReservationAuthorize : public ::testing::Test {
 protected:
  void SetUp() override {
    waiter.answer = false;
    seed.config_key = "initial-key";
    seed.until = SessionSeed::Until::started;
  }

  static auto authorize_and_lock(play::ServerReservation& session)
      -> libchiaking::Expected<void, gk::Error> {
    auto authorized = session.authorize(GkRig::codes());
    if (!authorized) return authorized;
    return session.lock();
  }

  test_support::FakeWaiter waiter;
  SessionSeed seed;
  AuthorizeThenLockProvider provider;
};

TEST_F(ReservationAuthorize, LocksWithTheKeyTheAuthorizationRotated) {
  SessionHarness rotating{provider, waiter, seed};
  EXPECT_TRUE(authorize_and_lock(rotating.session()));
  ASSERT_TRUE(rotating.session().lock_key());
  EXPECT_EQ(rotating.session().lock_key()->view(), "rotated-key");
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_EQ(provider.seen_keys.size(), std::size_t{2});
  EXPECT_EQ(provider.seen_keys[0], "initial-key");
  EXPECT_EQ(provider.seen_keys[1], "rotated-key");
  EXPECT_EQ(rotating.session().config_key().view(), "rotated-key");
}

TEST_F(ReservationAuthorize, LocksWithTheHeldKeyWhenNotRotated) {
  provider.rotate = false;
  SessionHarness retained{provider, waiter, seed};
  EXPECT_TRUE(authorize_and_lock(retained.session()));
  ASSERT_TRUE(retained.session().lock_key());
  EXPECT_EQ(retained.session().lock_key()->view(), "initial-key");
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(retained.session().config_key().view(), "initial-key");
}

TEST_F(ReservationAuthorize, StopsAtAPlusRefusalWithoutLocking) {
  provider.psplus = true;
  SessionHarness refused{provider, waiter, seed};
  EXPECT_TRUE(GkRig::failed(authorize_and_lock(refused.session()),
                            Stage::authorize, Reason::psplus_rejected, 2));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(refused.session().config_key().view(), "initial-key");
  EXPECT_FALSE(refused.session().lock_key());
}

TEST_F(ReservationAuthorize, AsksForAnEntitlementRetryWithoutLocking) {
  provider.entitlement = true;
  SessionHarness unentitled{provider, waiter, seed};
  EXPECT_TRUE(GkRig::failed(authorize_and_lock(unentitled.session()),
                            Stage::authorize, Reason::entitlement_retry, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationAuthorize, KeepsTheRotatedKeyButNoLockWhenTheLockIsRefused) {
  provider.lock_reject = true;
  SessionHarness unlocked{provider, waiter, seed};
  EXPECT_TRUE(GkRig::failed(authorize_and_lock(unlocked.session()), Stage::lock,
                            Reason::rejected, 5));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(unlocked.session().config_key().view(), "rotated-key");
  EXPECT_FALSE(unlocked.session().lock_key());
}

TEST_F(ReservationAuthorize, IsCancelledWhenThePendingLockCannotWait) {
  provider.lock_pending = true;
  SessionHarness waiting{provider, waiter, seed};
  const auto result = authorize_and_lock(waiting.session());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::lock);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 1U);
  EXPECT_EQ(provider.calls, 2U);
}

TEST_F(ReservationAuthorize, SendsNothingOnceStopped) {
  SessionHarness stopped{provider, waiter, seed};
  stopped.stop();
  const auto result = authorize_and_lock(stopped.session());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::authorize);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 1U);
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
