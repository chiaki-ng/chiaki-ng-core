#include "src/core/psn/gk/launch_spec.hpp"
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

class GkDatacentersRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view prefix = "{\"requestGameSpecification\":";

  void SetUp() override {
    const auto preauth = gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+00:00").value(), 1080);
    ASSERT_TRUE(preauth);
    auto signed_spec = preauth.value().authorized(
        GkRig::codes("synthetic-client-code", "synthetic-server-code"));
    ASSERT_TRUE(signed_spec);
    authorized.emplace(std::move(signed_spec.value()));
  }

  auto send(std::string_view spec, std::string_view session_id,
            std::string_view key)
      -> libchiaking::Expected<gk::DatacenterList, gk::Error> {
    http.reply_body = R"([{"dataCenter":"dc"}])";
    Gk client{http};
    return client.datacenters(GkRig::session_id(session_id),
                              GkRig::config_key(key), spec);
  }

  auto send(std::string_view session_id, std::string_view key)
      -> libchiaking::Expected<gk::DatacenterList, gk::Error> {
    return send(authorized->view(), session_id, key);
  }

  RecordingHttp http;
  std::optional<libchiaking::SecretString> authorized;
};

TEST_F(GkDatacentersRequest, PostsTheSignedSpecWithTheSessionHeaders) {
  ASSERT_TRUE(send("synthetic-session", "synthetic-key"));
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.datacenters");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(
      request.url.view(),
      "https://cc.prod.gaikai.com/v1/sessions/synthetic-session/datacenters");
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
  ASSERT_EQ(request.body.size(), prefix.size() + authorized->size() + 1);
  const auto body = request.body.view();
  EXPECT_EQ(body.substr(0, prefix.size()), prefix);
  EXPECT_EQ(body.substr(prefix.size(), authorized->size()), authorized->view());
  EXPECT_EQ(body.back(), '}');
  EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
  EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
}

TEST_F(GkDatacentersRequest, RefusesAnEmptySessionIdOrOneThatBreaksAPath) {
  EXPECT_TRUE(GkRig::fails_for(send("bad\nid", "synthetic-key"),
                               Reason::invalid_session_id));
  EXPECT_TRUE(
      GkRig::fails_for(send("", "synthetic-key"), Reason::invalid_session_id));
}

TEST_F(GkDatacentersRequest, AcceptsALongSessionId) {
  EXPECT_TRUE(send(std::string(127, 'a'), "synthetic-key"));
  EXPECT_TRUE(send(std::string(128, 'a'), "synthetic-key"));
}

TEST_F(GkDatacentersRequest, EscapesTheSessionIdInThePathOnly) {
  ASSERT_TRUE(send("a/b c\xC3\xA9", "key with space"));
  EXPECT_NE(http.last.url.view().find("/sessions/a%2Fb%20c%C3%A9/"),
            std::string::npos);
  ASSERT_EQ(http.last.headers.size(), std::size_t{5});
  EXPECT_EQ(http.last.headers[3].value.view(), "a/b c\xC3\xA9");
  EXPECT_EQ(http.last.headers[4].value.view(), "key with space");
}

TEST_F(GkDatacentersRequest, RefusesAConfigKeyThatBreaksAHeader) {
  EXPECT_TRUE(GkRig::fails_for(send("synthetic-session", "bad\r\nkey"),
                               Reason::invalid_config_key));
}

TEST_F(GkDatacentersRequest, RefusesASpecThatIsNotAnObject) {
  EXPECT_TRUE(
      GkRig::fails_for(send("bad-json", "synthetic-session", "synthetic-key"),
                       Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send("[]", "synthetic-session", "synthetic-key"),
                               Reason::invalid_spec));
}

TEST_F(GkDatacentersRequest, RefusesASpecOverTheBodyLimit) {
  EXPECT_TRUE(GkRig::fails_for(send(std::string(gk::Client::body_limit, 'x'),
                                    "synthetic-session", "synthetic-key"),
                               Reason::body_too_large));
}

class GkDatacentersResponse : public ::testing::Test {
 protected:
  static auto decode(std::string_view body)
      -> std::optional<std::vector<gk::Datacenter>> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    auto list = client.datacenters(GkRig::session_id("synthetic-session"),
                                   GkRig::config_key("synthetic-key"), "{}");
    if (!list) {
      EXPECT_EQ(list.error().reason(), Reason::malformed);
      return std::nullopt;
    }
    return std::move(list.value().rows);
  }
};

TEST_F(GkDatacentersResponse, ReadsEveryRowInOrder) {
  const auto rows = decode(
      R"([{"dataCenter":"dc-b","publicIp":"1.2.3.4","port":443,"maxBandwidth":20000},)"
      R"({"dataCenter":"dc-a","publicIp":"5.6.7.8","port":8443,"maxBandwidth":"1000"}])");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{2});
  EXPECT_EQ((*rows)[0].name, "dc-b");
  EXPECT_EQ((*rows)[0].public_ip, "1.2.3.4");
  EXPECT_EQ((*rows)[0].port, 443);
  EXPECT_EQ((*rows)[0].max_bandwidth, 20000);
  EXPECT_EQ((*rows)[1].name, "dc-a");
  EXPECT_EQ((*rows)[1].port, 8443);
  EXPECT_EQ((*rows)[1].max_bandwidth, 1000);
}

TEST_F(GkDatacentersResponse, LeavesMissingFieldsEmpty) {
  const auto rows = decode(R"([{},{"dataCenter":"dc-c"}])");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{2});
  EXPECT_TRUE((*rows)[0].name.empty());
  EXPECT_TRUE((*rows)[0].public_ip.empty());
  EXPECT_EQ((*rows)[0].port, 0);
  EXPECT_EQ((*rows)[0].max_bandwidth, 0);
}

TEST_F(GkDatacentersResponse, RefusesFieldsOfTheWrongType) {
  EXPECT_FALSE(decode(R"([{"dataCenter":"dc","port":"8443"}])"));
  EXPECT_FALSE(
      decode(R"([{"publicIp":123,"dataCenter":true,"port":"x"},{},5])"));
}

TEST_F(GkDatacentersResponse, RefusesAnEmptyListOrAnythingButAList) {
  EXPECT_FALSE(decode("[]"));
  EXPECT_FALSE(decode("{}"));
  EXPECT_FALSE(decode("not json"));
  EXPECT_FALSE(decode(""));
}

TEST_F(GkDatacentersResponse, StopsReadingAtARawNul) {
  std::string with_nul = R"([{"dataCenter":"dc","publicIp":"1.1.1.1"}])";
  with_nul += '\0';
  with_nul += "garbage";
  const auto rows = decode(with_nul);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{1});
  EXPECT_EQ((*rows)[0].name, "dc");
  EXPECT_FALSE(decode(std::string("\0", 1)));
  EXPECT_FALSE(decode(std::string{"["} + '\0' + "]"));
}

TEST_F(GkDatacentersResponse, ReadsEscapesAndTheEdgesOfItsNumbers) {
  const auto rows = decode(
      R"([{"dataCenter":"dc-west","publicIp":"host.example","port":-1,"maxBandwidth":2147483647}])");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{1});
  EXPECT_EQ((*rows)[0].name, "dc-west");
  EXPECT_EQ((*rows)[0].port, -1);
  EXPECT_EQ((*rows)[0].max_bandwidth, 2147483647);
}

TEST_F(GkDatacentersResponse, RefusesABandwidthBeyondAnInt) {
  EXPECT_FALSE(decode(R"([{"dataCenter":"dc","maxBandwidth":2147483648}])"));
}

TEST_F(GkDatacentersResponse, KeepsAnEscapedNulInTheName) {
  const auto rows = decode(
      R"([{"dataCenter":"dc\u0000nul","publicIp":"1.2.3.4","port":443}])");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{1});
  EXPECT_EQ((*rows)[0].name, std::string_view("dc\0nul", 6));
}

TEST_F(GkDatacentersResponse, TakesTheLastOfRepeatedNames) {
  const auto rows = decode(R"([{"dataCenter":"a","dataCenter":"b"}])");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{1});
  EXPECT_EQ((*rows)[0].name, "b");
}

class DatacentersProvider final : public platform::HttpProvider {
 public:
  static constexpr std::string_view one_row =
      R"([{"dataCenter":"dc1","publicIp":"127.0.0.1","port":1,"maxBandwidth":2}])";

  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/datacenters"), std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    for (const auto& [name, value] : headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{one_row};
  std::vector<std::pair<std::string, std::string>> headers{};
  unsigned calls{};
};

class GkDatacentersClient : public ::testing::Test {
 protected:
  auto fetch(const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::DatacenterList, gk::Error> {
    Gk client{provider, stop};
    return client.datacenters(GkRig::session_id("synthetic-session"),
                              GkRig::config_key("synthetic-key"), "{}");
  }

  DatacentersProvider provider;
};

TEST_F(GkDatacentersClient, FetchesTheRowsWithoutRotatingTheKey) {
  const auto result = fetch();
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().rows.size(), std::size_t{1});
  EXPECT_EQ(result.value().rows[0].name, "dc1");
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, RotatesTheKeyFromTheReply) {
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  const auto result = fetch();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "new-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(fetch(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, IsRejectedByAnErrorStatus) {
  provider.status = 500;
  EXPECT_TRUE(GkRig::fails_for(fetch(), Reason::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "bad json";
  EXPECT_TRUE(GkRig::fails_for(fetch(), Reason::malformed));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, ChecksTheRotatedKeyBeforeTheBody) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(fetch(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, CallsAnEmptyListMalformed) {
  provider.body = "[]";
  EXPECT_TRUE(GkRig::fails_for(fetch(), Reason::malformed));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  const auto result = fetch();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_transport());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = fetch();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkDatacentersClient, SendsNothingOnceStopped) {
  const auto result =
      fetch(platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
