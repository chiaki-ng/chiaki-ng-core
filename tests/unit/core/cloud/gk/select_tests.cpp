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
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;

class GkSelectRequest : public ::testing::Test {
 protected:
  void SetUp() override {
    gk::Ping ping;
    ping.name = "dc-a";
    ping.rtt_ms = 40;
    ping.rtts_ms = {40};
    ping.mtu_in = 1454;
    ping.mtu_out = 1254;
    ping.port = 2053;
    ping.selected_port = 9999;
    ping.public_ip = "1.2.3.4";
    ping.max_bandwidth = 20000;
    ping.measured = true;
    rows = {ping};
  }

  auto send(std::string_view spec,
            std::string_view session_id = "synthetic-session",
            std::string_view key = "synthetic-key")
      -> libchiaking::Expected<gk::Selected, gk::Error> {
    return send(spec, session_id, key, rows);
  }

  auto send(std::string_view spec, std::string_view session_id,
            std::string_view key, const std::vector<gk::Ping>& sent)
      -> libchiaking::Expected<gk::Selected, gk::Error> {
    Gk client{http};
    return client.select(GkRig::session_id(session_id), GkRig::config_key(key),
                         spec, sent, 0);
  }

  RecordingHttp http;
  std::vector<gk::Ping> rows;
};

TEST_F(GkSelectRequest, PostsTheReportedPingResults) {
  ASSERT_TRUE(send("{}"));
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.select");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(request.url.view(),
            "https://cc.prod.gaikai.com/v1/sessions/synthetic-session/"
            "datacenters/select");
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
  EXPECT_EQ(
      request.body.view(),
      "{\"requestGameSpecification\":{},\"pingResults\":[{\"dataCenter\":\"dc-"
      "a\","
      "\"rtt\":20,\"rtts\":[20],\"mtu_in\":1454,\"mtu_out\":1254,\"port\":2053,"
      "\"publicIp\":\"1.2.3.4\",\"maxBandwidth\":20000,\"measured\":true}]}");
  EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
  EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
}

TEST_F(GkSelectRequest, SendsTheSignedSpecAsItIs) {
  constexpr std::string_view secret_spec =
      R"({"gkCloudAuthCode":"synthetic-code"})";
  constexpr std::string_view prefix = "{\"requestGameSpecification\":";
  ASSERT_TRUE(send(secret_spec));
  EXPECT_EQ(http.last.body.view().substr(prefix.size(), secret_spec.size()),
            secret_spec);
}

TEST_F(GkSelectRequest, RefusesNoPingResults) {
  EXPECT_TRUE(
      GkRig::fails_for(send("{}", "synthetic-session", "synthetic-key", {}),
                       Reason::invalid_ping_results));
}

TEST_F(GkSelectRequest, RefusesASessionIdOrKeyThatBreaksTheRequest) {
  EXPECT_TRUE(
      GkRig::fails_for(send("{}", "bad\nid"), Reason::invalid_session_id));
  EXPECT_TRUE(GkRig::fails_for(send("{}", "synthetic-session", "bad\r\nkey"),
                               Reason::invalid_config_key));
}

TEST_F(GkSelectRequest, RefusesASpecThatIsNotAnObjectOrTooLarge) {
  EXPECT_TRUE(GkRig::fails_for(send("[]"), Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send("bad-json"), Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send(std::string(gk::Client::body_limit, 'x')),
                               Reason::body_too_large));
}

class GkSelectResponse : public ::testing::Test {
 protected:
  static auto port(std::string_view body, int current_port = 100)
      -> std::optional<int> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    gk::Ping ping;
    ping.name = "dc";
    const auto selected = client.select(GkRig::session_id("synthetic-session"),
                                        GkRig::config_key("synthetic-key"),
                                        "{}", {ping}, current_port);
    if (!selected) return std::nullopt;
    return selected.value().port;
  }
};

TEST_F(GkSelectResponse, PrefersTheTopLevelPort) {
  EXPECT_EQ(port(R"({"port":1234,"network":{"port":9999}})"), 1234);
  EXPECT_EQ(port(R"({"port":53})"), 53);
}

TEST_F(GkSelectResponse, FallsBackToTheNetworkPort) {
  EXPECT_EQ(port(R"({"network":{"port":4321}})"), 4321);
  EXPECT_EQ(port(R"({"port":-2,"network":{"port":53}})"), 53);
}

TEST_F(GkSelectResponse, KeepsTheCurrentPortWithoutAUsableOne) {
  EXPECT_EQ(port(R"({"port":0,"network":{"port":-5}})"), 100);
  EXPECT_EQ(port(R"({"port":"abc","network":"x"})"), 100);
  EXPECT_EQ(port(R"({"port":[1],"network":{"port":{}}})"), 100);
  EXPECT_EQ(port(R"({"port":"53"})"), 100);
}

TEST_F(GkSelectResponse, KeepsTheCurrentPortForAnUnreadableReply) {
  EXPECT_EQ(port("{"), 100);
  EXPECT_EQ(port("[1,2]"), 100);
  EXPECT_EQ(port(""), 100);
}

TEST_F(GkSelectResponse, StopsReadingAtARawNul) {
  const std::string with_nul = std::string{R"({"port":77})"} + '\0' + "garbage";
  EXPECT_EQ(port(with_nul), 77);
  const std::string cut = std::string{R"({"port":7)"} + '\0' + "7}";
  EXPECT_EQ(port(cut), 100);
}

class GkSelectClient : public ::testing::Test {
 protected:
  void SetUp() override {
    gk::Ping ping;
    ping.name = "dc1";
    ping.public_ip = "192.0.2.1";
    ping.rtt_ms = 10;
    ping.rtts_ms = {10};
    rows = {ping};
  }

  auto select(int current_port, const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::Selected, gk::Error> {
    Gk client{provider, stop};
    return client.select(GkRig::session_id("synthetic-session"),
                         GkRig::config_key("synthetic-key"), "{}", rows,
                         current_port);
  }

  std::vector<gk::Ping> rows;
  SelectProvider provider;
};

TEST_F(GkSelectClient, ReadsTheSelectedPort) {
  const auto result = select(9295);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().port, 8975);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, ReadsTheNetworkPort) {
  provider.body = R"({"network":{"port":9295}})";
  const auto result = select(1234);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().port, 9295);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, KeepsThePortAndRotatesTheKeyOnAnUnreadableReply) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  const auto result = select(777);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().port, 777);
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "new-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, KeepsThePortAndKeyOnAnUnreadableReply) {
  provider.body = "bad json";
  const auto result = select(777);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().port, 777);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(select(777), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, IsRejectedByAnErrorStatus) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  provider.status = 500;
  EXPECT_TRUE(GkRig::fails_for(select(777), Reason::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  const auto result = select(777);
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_transport());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = select(777);
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkSelectClient, SendsNothingOnceStopped) {
  const auto result = select(
      777, platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(GkSelectClient, KeepsThePortOnAnEmptyReply) {
  provider.body.clear();
  const auto result = select(3210);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().port, 3210);
  EXPECT_EQ(provider.calls, 1U);
}

}  // namespace
}  // namespace cloud_test
