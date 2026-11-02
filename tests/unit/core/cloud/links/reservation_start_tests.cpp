#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;
using Fields = std::vector<std::pair<std::string, std::string>>;

class OpeningProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    const auto url = request.url.view();
    if (url.find("/client_ids") != std::string_view::npos)
      return reply(
          200, {},
          R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})");
    if (url.ends_with("/config")) {
      if (stop_on_config) stop_on_config->stopped = true;
      return reply(200, {}, R"({"configKey":"synthetic-key"})");
    }
    ++calls;
    EXPECT_NE(url.find("/sessions/start"), std::string::npos);
    EXPECT_EQ(request.headers.size(), std::size_t{4});
    if (request.headers.size() > 3) seen_key = request.headers[3].value.view();
    if (error) return libchiaking::unexpected(*error);
    return reply(status, headers, body);
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"sessionId":"synthetic-session"})"};
  Fields headers;
  std::shared_ptr<test_support::StopState> stop_on_config;
  std::string seen_key;
  unsigned calls{};

 private:
  static auto reply(std::uint16_t code, const Fields& fields,
                    std::string_view text) -> platform::HttpResponse {
    platform::HttpResponse response;
    response.status_code = code;
    for (const auto& [name, value] : fields)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    for (const char character : text)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }
};

class ReservationStart : public ::testing::Test {
 protected:
  auto open(gk::Client& client, std::string_view spec = "{}")
      -> libchiaking::Expected<play::ServerReservation, gk::Error> {
    return play::ServerReservation::open(
        client, {waiter},
        gk::LaunchSpec::prebuilt(CloudService::cloud_stream, std::string{spec}),
        CloudPlatform::ps5);
  }

  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  OpeningProvider provider;
};

TEST_F(ReservationStart, OpensTheSessionAndRotatesTheKey) {
  provider.headers = {{"X-Gaikai-Session", "rotated-synthetic-key"}};
  Gk client{provider};
  const auto result = open(client);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().id().view(), "synthetic-session");
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(provider.seen_key, "synthetic-key");
  EXPECT_EQ(result.value().config_key().view(), "rotated-synthetic-key");
}

TEST_F(ReservationStart, KeepsTheConfigKeyWhenNotRotated) {
  Gk client{provider};
  const auto result = open(client);
  ASSERT_TRUE(result);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(result.value().id().view(), "synthetic-session");
  EXPECT_EQ(result.value().config_key().view(), "synthetic-key");
}

TEST_F(ReservationStart, AsksForAnEntitlementRetryWithoutAGame) {
  provider.status = 401;
  provider.body = R"({"name":"noGameForEntitlementId"})";
  provider.headers = {{"X-Gaikai-Session", "must-not-rotate"}};
  Gk client{provider};
  EXPECT_TRUE(
      GkRig::failed(open(client), Stage::start, Reason::entitlement_retry, 1));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationStart, IsRejectedByAnErrorStatus) {
  provider.status = 500;
  provider.body = R"({"name":"genericFailure"})";
  provider.headers = {{"X-Gaikai-Session", "must-not-rotate"}};
  Gk client{provider};
  EXPECT_TRUE(GkRig::failed(open(client), Stage::start, Reason::rejected, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationStart, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "must-not-rotate"}};
  Gk client{provider};
  EXPECT_TRUE(GkRig::failed(open(client), Stage::start, Reason::malformed, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationStart, IsCancelledWithTheRequest) {
  provider.error = platform::HttpError::cancelled;
  Gk client{provider};
  const auto result = open(client);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::start);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationStart, RefusesAnEmptySpecBeforeStarting) {
  Gk client{provider};
  const auto result = open(client, "");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::start);
  EXPECT_TRUE(result.error().is_invalid_request());
  EXPECT_EQ(result.error().detail(), 2U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationStart, StopsAfterTheConfigWhenStoppedThere) {
  const auto stop = std::make_shared<test_support::StopState>();
  provider.stop_on_config = stop;
  Gk client{provider, platform::StopToken{stop}};
  const auto result = open(client);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::config);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 1U);
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
