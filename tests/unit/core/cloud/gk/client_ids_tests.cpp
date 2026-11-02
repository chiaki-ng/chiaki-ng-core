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

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class GkClientIdsRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view legacy_agent =
      "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
      "Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
      "Safari/537.36 gkApollo";

  static void expect_request(CloudService service, CloudPlatform target,
                             std::string_view url,
                             std::string_view user_agent) {
    RecordingHttp http;
    http.reply_body = R"({"gkClientId":"gk"})";
    Gk client{http};
    EXPECT_TRUE(client.client_ids(service, target));
    ASSERT_EQ(http.calls, 1U);
    const auto& request = http.last;
    EXPECT_EQ(request.label, "gk.client_ids");
    EXPECT_EQ(request.method, platform::HttpMethod::get);
    EXPECT_EQ(request.url.view(), url);
    ASSERT_EQ(request.headers.size(), std::size_t{2});
    EXPECT_EQ(request.headers[0].name, "Accept");
    EXPECT_EQ(request.headers[0].value.view(), "*/*");
    EXPECT_EQ(request.headers[1].name, "User-Agent");
    EXPECT_EQ(request.headers[1].value.view(), user_agent);
    EXPECT_TRUE(request.body.empty());
    EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
    EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
    EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
  }

  static auto refuses(CloudService service, CloudPlatform target)
      -> testing::AssertionResult {
    RecordingHttp http;
    Gk client{http};
    const auto refused =
        GkRig::failed(client.client_ids(service, target), Stage::client_ids,
                      Reason::invalid_service, 0);
    if (!refused) return refused;
    if (http.calls != 0)
      return testing::AssertionFailure() << http.calls << " requests were sent";
    return testing::AssertionSuccess();
  }
};

TEST_F(GkClientIdsRequest, AsksForKonanForAPs3Title) {
  expect_request(CloudService::legacy_cloud_stream, CloudPlatform::ps3,
                 "https://cc.prod.gaikai.com/v1/client_ids?virtType=konan",
                 legacy_agent);
}

TEST_F(GkClientIdsRequest, AsksForKratosForAPs4Title) {
  expect_request(CloudService::legacy_cloud_stream, CloudPlatform::ps4,
                 "https://cc.prod.gaikai.com/v1/client_ids?virtType=kratos",
                 legacy_agent);
}

TEST_F(GkClientIdsRequest, AsksForCronosForAPs5Title) {
  expect_request(CloudService::legacy_cloud_stream, CloudPlatform::ps5,
                 "https://cc.prod.gaikai.com/v1/client_ids?virtType=cronos",
                 legacy_agent);
}

TEST_F(GkClientIdsRequest, AsksForCronosAsTheCloudStreamAgent) {
  expect_request(CloudService::cloud_stream, CloudPlatform::ps5,
                 "https://cc.prod.gaikai.com/v1/client_ids?virtType=cronos",
                 "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
}

TEST_F(GkClientIdsRequest, RefusesAnUnsupportedPairingBeforeSending) {
  EXPECT_TRUE(refuses(CloudService::cloud_stream, CloudPlatform::ps3));
  EXPECT_TRUE(refuses(CloudService::cloud_stream, CloudPlatform::ps4));
  EXPECT_TRUE(refuses(static_cast<CloudService>(99), CloudPlatform::ps3));
  EXPECT_TRUE(refuses(CloudService::legacy_cloud_stream,
                      static_cast<CloudPlatform>(99)));
}

class GkClientIdsResponse : public ::testing::Test {
 protected:
  static auto decode(std::string_view body)
      -> libchiaking::Expected<gk::ClientIds, gk::Error> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    return client.client_ids(CloudService::legacy_cloud_stream,
                             CloudPlatform::ps4);
  }

  static auto malformed(std::string_view body) -> testing::AssertionResult {
    return GkRig::fails_for(decode(body), Reason::malformed);
  }

  static auto lenient(std::string_view body) -> testing::AssertionResult {
    const auto ids = decode(body);
    if (!ids) return testing::AssertionFailure() << "the ids were refused";
    if (ids.value().gk_client_id != "a")
      return testing::AssertionFailure()
             << "the gk client id is " << ids.value().gk_client_id;
    if (!ids.value().ps3_gk_client_id.empty())
      return testing::AssertionFailure()
             << "the ps3 client id is " << ids.value().ps3_gk_client_id;
    if (!ids.value().stream_server_client_id.empty())
      return testing::AssertionFailure() << "the stream server id is "
                                         << ids.value().stream_server_client_id;
    return testing::AssertionSuccess();
  }
};

TEST_F(GkClientIdsResponse, ReadsEveryId) {
  const auto ids = decode(
      R"({"streamServerClientId":"stream","ps3GkClientId":"ps3","gkClientId":"gk"})");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids.value().gk_client_id, "gk");
  EXPECT_EQ(ids.value().ps3_gk_client_id, "ps3");
  EXPECT_EQ(ids.value().stream_server_client_id, "stream");
}

TEST_F(GkClientIdsResponse, LeavesAMissingPs3IdEmpty) {
  const auto ids = decode(R"({"gkClientId":"gk"})");
  ASSERT_TRUE(ids);
  EXPECT_TRUE(ids.value().ps3_gk_client_id.empty());
}

TEST_F(GkClientIdsResponse, KeepsAtMost127Characters) {
  const std::string long_id(128, 'x');
  const auto ids = decode(std::string{"{\"gkClientId\":\""} + long_id +
                          "\",\"ps3GkClientId\":\"" + long_id + "\"}");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids.value().gk_client_id.size(), std::size_t{127});
  EXPECT_EQ(ids.value().ps3_gk_client_id.size(), std::size_t{127});
}

TEST_F(GkClientIdsResponse, KeepsEscapedNuls) {
  const auto ids =
      decode(R"({"gkClientId":"gk\u0000tail","ps3GkClientId":"p\u0000tail"})");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids.value().gk_client_id, std::string_view("gk\0tail", 7));
  EXPECT_EQ(ids.value().ps3_gk_client_id, std::string_view("p\0tail", 6));
}

TEST_F(GkClientIdsResponse, RefusesAReplyWithoutAGkClientId) {
  EXPECT_TRUE(malformed("{"));
  EXPECT_TRUE(malformed("[]"));
  EXPECT_TRUE(malformed("{}"));
  EXPECT_TRUE(malformed(R"({"gkClientId":""})"));
  EXPECT_TRUE(malformed(R"({"gkClientId":4})"));
  EXPECT_TRUE(malformed(R"({"gkClientId":null})"));
}

TEST_F(GkClientIdsResponse, IgnoresOtherIdsOfTheWrongType) {
  EXPECT_TRUE(lenient(
      R"({"gkClientId":"a","ps3GkClientId":4,"streamServerClientId":null})"));
  EXPECT_TRUE(lenient(
      R"({"gkClientId":"a","ps3GkClientId":null,"streamServerClientId":false})"));
  EXPECT_TRUE(lenient(
      R"({"gkClientId":"a","ps3GkClientId":{"x":1},"streamServerClientId":[1]})"));
}

TEST_F(GkClientIdsResponse, TakesTheLastOfRepeatedIds) {
  const auto ids = decode(R"({"gkClientId":"a","gkClientId":"b"})");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids.value().gk_client_id, "b");
}

TEST_F(GkClientIdsResponse, RefusesABlankReplyOverTheLimit) {
  EXPECT_TRUE(malformed(std::string(16 * 1024 + 1, ' ')));
}

TEST_F(GkClientIdsResponse, ReadsTheIdBesideLongPadding) {
  const auto ids = decode(std::string{"{\"gkClientId\":\"gk\",\"padding\":\""} +
                          std::string(16 * 1024, 'x') + "\"}\r\n");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids.value().gk_client_id, "gk");
}

class ClientIdsProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::get);
    EXPECT_TRUE(request.url.view().ends_with("virtType=konan") ||
                request.url.view().ends_with("virtType=cronos"))
        << request.url.view();
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response{};
    response.status_code = status;
    for (const auto character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{
      R"({"gkClientId":"a","ps3GkClientId":"b","streamServerClientId":"c"})"};
  unsigned calls{};
};

class GkClientIdsClient : public ::testing::Test {
 protected:
  auto fetch(CloudPlatform target = CloudPlatform::ps3)
      -> libchiaking::Expected<gk::ClientIds, gk::Error> {
    Gk client{provider, running};
    return client.client_ids(CloudService::legacy_cloud_stream, target);
  }

  static auto failed(
      const libchiaking::Expected<gk::ClientIds, gk::Error>& result,
      Reason reason, std::uint32_t detail) -> testing::AssertionResult {
    return GkRig::failed(result, Stage::client_ids, reason, detail);
  }

  ClientIdsProvider provider;
  const platform::StopToken running;
};

TEST_F(GkClientIdsClient, FetchesTheIdsInOneRequest) {
  const auto result = fetch();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().gk_client_id, "a");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, CallsAnEmptyReplyMalformed) {
  provider.body.clear();
  EXPECT_TRUE(failed(fetch(), Reason::malformed, 4));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "{not json";
  EXPECT_TRUE(failed(fetch(), Reason::malformed, 4));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, IsRejectedByAnErrorStatus) {
  provider.status = 401;
  EXPECT_TRUE(failed(fetch(), Reason::rejected, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(failed(fetch(), Reason::transport, 2));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = fetch();
  EXPECT_TRUE(failed(result, Reason::cancelled, 1));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkClientIdsClient, IsNotAskedOnceStopped) {
  const platform::StopToken stopped{std::make_shared<test_support::Stopped>()};
  Gk stopped_client{provider, stopped};
  auto waiter = GkRig::recording_waiter();
  const auto opened = play::ServerReservation::open(
      stopped_client, {waiter},
      gk::LaunchSpec::prebuilt(CloudService::legacy_cloud_stream,
                               std::string{GkRig::blank_spec}),
      CloudPlatform::ps3);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().stage(), Stage::client_ids);
  EXPECT_TRUE(opened.error().is_cancelled());
  EXPECT_EQ(opened.error().detail(), 1U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(GkClientIdsClient, FetchesTheIdsForAPs5Title) {
  const auto result = fetch(CloudPlatform::ps5);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().gk_client_id, "a");
  EXPECT_EQ(provider.calls, 1U);
}

}  // namespace
}  // namespace cloud_test
