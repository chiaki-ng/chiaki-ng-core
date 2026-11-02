#include "src/core/psn/gk/measurement.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;

class GkAllocateRequest : public ::testing::Test {
 protected:
  void SetUp() override {
    selected.name = "dc-a";
    selected.rtt_ms = 20;
    selected.mtu_in = 1454;
    selected.mtu_out = 1254;
  }

  auto send(std::string_view spec,
            std::string_view session_id = "synthetic-session",
            std::string_view key = "synthetic-key")
      -> libchiaking::Expected<gk::AllocateState, gk::Error> {
    http.reply_body = R"({"queued":true})";
    Gk client{http};
    return client.allocate(GkRig::session_id(session_id),
                           GkRig::config_key(key), spec, selected, 2053, 20000);
  }

  RecordingHttp http;
  gk::Ping selected;
};

TEST_F(GkAllocateRequest, PostsTheSpecWithTheMeasuredNetwork) {
  ASSERT_TRUE(send("{}"));
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.allocate");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(
      request.url.view(),
      "https://cc.prod.gaikai.com/v1/sessions/synthetic-session/allocate");
  ASSERT_EQ(request.headers.size(), std::size_t{5});
  EXPECT_EQ(request.headers[0].name, "Content-Type");
  EXPECT_EQ(request.headers[0].value.view(), "application/json");
  EXPECT_EQ(request.headers[1].name, "Accept");
  EXPECT_EQ(request.headers[1].value.view(), "*/*");
  EXPECT_EQ(request.headers[2].name, "User-Agent");
  EXPECT_EQ(request.headers[2].value.view(),
            "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
  EXPECT_EQ(request.headers[3].name, "X-Gaikai-SessionId");
  EXPECT_EQ(request.headers[3].value.view(), "synthetic-session");
  EXPECT_EQ(request.headers[4].name, "X-Gaikai-Session");
  EXPECT_EQ(request.headers[4].value.view(), "synthetic-key");
  EXPECT_EQ(request.body.view(),
            "{\"requestGameSpecification\":{},\"dataCenter\":\"dc-a\","
            "\"network\":{\"bwKbpsSent\":20000,\"bwLoss\":0.001,"
            "\"mtu\":1454,\"rtt\":20,\"port\":2053,\"bwKbpsReceived\":20000,"
            "\"bwLossUpstream\":0,\"mtuUpstream\":1254},"
            "\"stateExecutionTime\":5974.7632,\"streamTestTime\":11262.8423}");
  EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
  EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
}

TEST_F(GkAllocateRequest, SendsTheSignedSpecAsItIs) {
  constexpr std::string_view secret_spec =
      R"({"gkCloudAuthCode":"synthetic-code"})";
  constexpr std::string_view prefix = "{\"requestGameSpecification\":";
  ASSERT_TRUE(send(secret_spec));
  EXPECT_EQ(http.last.body.view().substr(prefix.size(), secret_spec.size()),
            secret_spec);
}

TEST_F(GkAllocateRequest, RefusesASessionIdOrKeyThatBreaksTheRequest) {
  EXPECT_TRUE(
      GkRig::fails_for(send("{}", "bad\nid"), Reason::invalid_session_id));
  EXPECT_TRUE(GkRig::fails_for(send("{}", "synthetic-session", "bad\r\nkey"),
                               Reason::invalid_config_key));
}

TEST_F(GkAllocateRequest, RefusesASpecThatIsNotAnObjectOrTooLarge) {
  EXPECT_TRUE(GkRig::fails_for(send("[]"), Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send(std::string(gk::Client::body_limit, 'x')),
                               Reason::body_too_large));
}

TEST_F(GkAllocateRequest, RefusesADatacenterNameWithANul) {
  selected.name = std::string{"bad\0name", 8};
  EXPECT_TRUE(GkRig::fails_for(send("{}"), Reason::invalid_allocate_body));
}

class GkAllocateExtraJson : public ::testing::Test {
 protected:
  void SetUp() override {
    row.name = "us-west-2a";
    row.rtt_ms = 18;
    row.mtu_in = 1454;
    row.mtu_out = 1254;
  }

  auto extra(int port = 2053, int bitrate = 10000)
      -> std::optional<std::string> {
    RecordingHttp http;
    http.reply_body = R"({"queued":true})";
    Gk client{http};
    const auto sent = client.allocate(GkRig::session_id("synthetic-session"),
                                      GkRig::config_key("synthetic-key"), "{}",
                                      row, port, bitrate);
    if (!sent) {
      EXPECT_EQ(sent.error().reason(), Reason::invalid_allocate_body);
      EXPECT_EQ(http.calls, 0U);
      return std::nullopt;
    }
    constexpr std::string_view prefix = "{\"requestGameSpecification\":{},";
    const auto body = http.last.body.view();
    EXPECT_EQ(body.substr(0, prefix.size()), prefix);
    EXPECT_EQ(body.back(), '}');
    return std::string{
        body.substr(prefix.size(), body.size() - prefix.size() - 1)};
  }

  gk::Ping row;
};

TEST_F(GkAllocateExtraJson, WritesTheDatacenterAndNetwork) {
  const auto json = extra();
  ASSERT_TRUE(json);
  EXPECT_EQ(
      *json,
      "\"dataCenter\":\"us-west-2a\",\"network\":{\"bwKbpsSent\":10000,"
      "\"bwLoss\":0.001,\"mtu\":1454,\"rtt\":18,\"port\":2053,"
      "\"bwKbpsReceived\":10000,\"bwLossUpstream\":0,\"mtuUpstream\":1254},"
      "\"stateExecutionTime\":5974.7632,\"streamTestTime\":11262.8423");
}

TEST_F(GkAllocateExtraJson, ReportsAMeasuredRoundTripLessTheAllowance) {
  row.measured = true;
  row.rtt_ms = 38;
  const auto far = extra();
  ASSERT_TRUE(far);
  EXPECT_NE(far->find("\"rtt\":18,"), std::string::npos);
  row.rtt_ms = 9;
  const auto near = extra();
  ASSERT_TRUE(near);
  EXPECT_NE(near->find("\"rtt\":1,"), std::string::npos);
}

TEST_F(GkAllocateExtraJson, FallsBackToDefaultsForAnUnmeasuredRow) {
  row.rtt_ms = 0;
  row.mtu_in = 0;
  row.mtu_out = 0;
  const auto zero = extra(80, 1);
  ASSERT_TRUE(zero);
  EXPECT_NE(zero->find("\"mtu\":1454"), std::string::npos);
  EXPECT_NE(zero->find("\"rtt\":25"), std::string::npos);
  EXPECT_NE(zero->find("\"mtuUpstream\":1254"), std::string::npos);
  EXPECT_NE(zero->find("\"port\":80"), std::string::npos);
  row.rtt_ms = -1;
  row.mtu_in = -1;
  row.mtu_out = -1;
  const auto negative = extra(80, 1);
  ASSERT_TRUE(negative);
  EXPECT_NE(negative->find("\"mtu\":1454"), std::string::npos);
  EXPECT_NE(negative->find("\"rtt\":25"), std::string::npos);
  EXPECT_NE(negative->find("\"mtuUpstream\":1254"), std::string::npos);
}

TEST_F(GkAllocateExtraJson, KeepsAt127CharactersOfTheName) {
  row.name.assign(200, 'x');
  const auto json = extra();
  ASSERT_TRUE(json);
  EXPECT_NE(json->find(std::string(127, 'x')), std::string::npos);
  EXPECT_EQ(json->find(std::string(128, 'x')), std::string::npos);
}

TEST_F(GkAllocateExtraJson, RefusesANulOnlyWithinTheKeptName) {
  row.name.assign(200, 'x');
  row.name[150] = '\0';
  EXPECT_TRUE(extra());
  row.name[10] = '\0';
  EXPECT_FALSE(extra());
}

TEST_F(GkAllocateExtraJson, EscapesTheName) {
  row.name = "a\"b\\c\nd\te";
  const auto escaped = extra(2053, 100);
  ASSERT_TRUE(escaped);
  EXPECT_NE(escaped->find("a\\\"b\\\\c\\nd\\te"), std::string::npos);
  row.name = std::string{"a\x01", 2};
  const auto control = extra(2053, 100);
  ASSERT_TRUE(control);
  EXPECT_NE(control->find("a\\u0001"), std::string::npos);
}

class AllocateProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/allocate"), std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    for (const auto& [name, value] : headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    response.body.assign(std::string_view{body});
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"queued":true,"pollFrequency":5})"};
  std::vector<std::pair<std::string, std::string>> headers{};
  unsigned calls{};
};

class GkAllocateAttemptClient : public ::testing::Test {
 protected:
  static constexpr std::string_view slot_body =
      R"({"launchSlot":{"publicIp":"1.2.3.4","port":8299},"handshakeKey":"hk",)"
      R"("launchSpecification":"spec","sessionId":"sid"})";

  void SetUp() override {
    selected.name = "dc";
    selected.rtt_ms = 20;
  }

  auto attempt(const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::AllocateState, gk::Error> {
    Gk client{provider, stop};
    return client.allocate(GkRig::session_id("synthetic-session"),
                           GkRig::config_key("synthetic-key"), "{}", selected,
                           2053, 10000);
  }

  gk::Ping selected;
  AllocateProvider provider;
};

TEST_F(GkAllocateAttemptClient, ReportsAQueuedSlot) {
  const auto result = attempt();
  ASSERT_TRUE(result);
  ASSERT_TRUE(
      std::holds_alternative<gk::AllocationProgress>(result.value().outcome));
  EXPECT_TRUE(std::get<gk::AllocationProgress>(result.value().outcome).queued);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, ReportsADataMigration) {
  provider.body =
      R"({"dataMigration":true,"dataMigrationPercentageComplete":63})";
  const auto result = attempt();
  ASSERT_TRUE(result);
  ASSERT_TRUE(
      std::holds_alternative<gk::AllocationProgress>(result.value().outcome));
  EXPECT_TRUE(
      std::get<gk::AllocationProgress>(result.value().outcome).migrating);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, ReturnsTheLaunchSlot) {
  provider.body = std::string{slot_body};
  const auto result = attempt();
  ASSERT_TRUE(result);
  ASSERT_TRUE(std::holds_alternative<gk::Allocation>(result.value().outcome));
  const auto& success = std::get<gk::Allocation>(result.value().outcome);
  EXPECT_EQ(success.server_ip, "1.2.3.4");
  EXPECT_EQ(success.server_port, 8299);
  EXPECT_EQ(success.handshake_key.view(), "hk");
  EXPECT_EQ(success.launch_specification.view(), "spec");
  EXPECT_EQ(success.session_id.view(), "sid");
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, RotatesTheKeyFromTheReply) {
  provider.body = std::string{slot_body};
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  const auto result = attempt();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "new-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.body = std::string{slot_body};
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, IsRejectedByAnErrorStatus) {
  provider.body = std::string{slot_body};
  provider.status = 500;
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::transport));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = attempt();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, SendsNothingOnceStopped) {
  const auto result =
      attempt(platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(GkAllocateAttemptClient, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "bad json";
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::malformed));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAllocateAttemptClient, IsMissingASlotWithoutOne) {
  provider.body = R"({"handshakeKey":"hk"})";
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::missing_launch_slot));
  EXPECT_EQ(provider.calls, 1U);
}

}  // namespace
}  // namespace cloud_test
