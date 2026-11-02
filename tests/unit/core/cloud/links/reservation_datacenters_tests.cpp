#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class RotatingDatacentersProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_NE(request.url.view().find("/synthetic-session/datacenters"),
              std::string::npos);
    EXPECT_EQ(request.headers.size(), std::size_t{5});
    if (request.headers.size() > 4) seen_key = request.headers[4].value.view();
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    if (rotate)
      response.headers.push_back(
          {"X-Gaikai-Session", libchiaking::SecretString{"rotated-key"}});
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::uint16_t status{200};
  std::optional<platform::HttpError> error;
  bool rotate{true};
  unsigned calls{};
  std::string seen_key;
  std::string body{
      R"([{"dataCenter":"dc1","publicIp":"127.0.0.1","port":2053,"maxBandwidth":10}])"};
};

class ReservationDatacenters : public ::testing::Test {
 protected:
  void SetUp() override { seed.config_key = "initial-key"; }

  static auto failed(
      const libchiaking::Expected<std::vector<gk::Datacenter>, gk::Error>& rows,
      Reason reason, std::uint32_t detail) -> testing::AssertionResult {
    return GkRig::failed(rows, Stage::datacenters, reason, detail);
  }

  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  SessionSeed seed;
  RotatingDatacentersProvider provider;
};

TEST_F(ReservationDatacenters, ListsTheRowsWithTheHeldKeyAndRotatesIt) {
  SessionHarness rotating{provider, waiter, seed};
  const auto rows = rotating.session().datacenters();
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows.value().size(), std::size_t{1});
  EXPECT_EQ(rows.value()[0].name, "dc1");
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(provider.seen_key, "initial-key");
  EXPECT_EQ(rotating.session().config_key().view(), "rotated-key");
}

TEST_F(ReservationDatacenters, KeepsTheKeyWhenTheReplyDoesNotRotateIt) {
  provider.rotate = false;
  SessionHarness retained{provider, waiter, seed};
  EXPECT_TRUE(retained.session().datacenters());
  EXPECT_EQ(retained.session().config_key().view(), "initial-key");
}

TEST_F(ReservationDatacenters, KeepsTheKeyWhenRejected) {
  provider.status = 500;
  SessionHarness refused{provider, waiter, seed};
  EXPECT_TRUE(failed(refused.session().datacenters(), Reason::rejected, 1));
  EXPECT_EQ(refused.session().config_key().view(), "initial-key");
}

TEST_F(ReservationDatacenters, KeepsTheKeyWhenTheListIsMalformed) {
  provider.body = "[]";
  SessionHarness unreadable{provider, waiter, seed};
  EXPECT_TRUE(failed(unreadable.session().datacenters(), Reason::malformed, 1));
  EXPECT_EQ(unreadable.session().config_key().view(), "initial-key");
}

TEST_F(ReservationDatacenters, KeepsTheKeyWhenTheRequestIsCancelled) {
  provider.error = platform::HttpError::cancelled;
  SessionHarness interrupted{provider, waiter, seed};
  const auto rows = interrupted.session().datacenters();
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().stage(), Stage::datacenters);
  EXPECT_TRUE(rows.error().is_cancelled());
  EXPECT_EQ(rows.error().detail(), 0U);
  EXPECT_EQ(interrupted.session().config_key().view(), "initial-key");
}

TEST_F(ReservationDatacenters, RefusesAnUnauthorizedSpecBeforeSending) {
  seed.until = SessionSeed::Until::started;
  SessionHarness unauthorized{provider, waiter, seed};
  const auto rows = unauthorized.session().datacenters();
  EXPECT_TRUE(failed(rows, Reason::invalid_spec, 1));
  ASSERT_FALSE(rows);
  EXPECT_TRUE(rows.error().is_invalid_request());
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationDatacenters, SendsNothingOnceStopped) {
  SessionHarness stopped{provider, waiter, seed};
  stopped.stop();
  const auto rows = stopped.session().datacenters();
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().stage(), Stage::datacenters);
  EXPECT_TRUE(rows.error().is_cancelled());
  EXPECT_EQ(rows.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(stopped.session().config_key().view(), "initial-key");
}

}  // namespace
}  // namespace cloud_test
