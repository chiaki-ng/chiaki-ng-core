#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;

class GkConfigRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view legacy_agent =
      "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
      "Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
      "Safari/537.36 gkApollo";
  static constexpr std::string_view cloud_stream_agent =
      "PlayStation Portal/6.0.0-rel.444+6a9cea6f5";

  static void expect_request(CloudService service, std::string_view body,
                             std::string_view user_agent) {
    RecordingHttp http;
    http.reply_body = R"({"configKey":"synthetic"})";
    Gk client{http};
    EXPECT_TRUE(client.config(service));
    ASSERT_EQ(http.calls, 1U);
    const auto& request = http.last;
    EXPECT_EQ(request.label, "gk.config");
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.url.view(),
              "https://config.cc.prod.gaikai.com/v1/config");
    ASSERT_EQ(request.headers.size(), std::size_t{3});
    EXPECT_EQ(request.headers[0].name, "Content-Type");
    EXPECT_EQ(request.headers[0].value.view(), "application/json");
    EXPECT_EQ(request.headers[1].name, "Accept");
    EXPECT_EQ(request.headers[1].value.view(), "*/*");
    EXPECT_EQ(request.headers[2].name, "User-Agent");
    EXPECT_EQ(request.headers[2].value.view(), user_agent);
    EXPECT_EQ(request.body.size(), body.size());
    EXPECT_EQ(http.body(), body);
    EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
    EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
    EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
  }
};

TEST_F(GkConfigRequest, AsksForThePsNowProductAsTheDesktopApp) {
  expect_request(CloudService::legacy_cloud_stream,
                 R"({"product":"psnow","platform":"PC","sessionId":""})",
                 legacy_agent);
}

TEST_F(GkConfigRequest, AsksForTheQliteProductAsTheCloudStreamAgent) {
  expect_request(CloudService::cloud_stream,
                 R"({"product":"qlite","platform":"qlite","sessionId":""})",
                 cloud_stream_agent);
}

TEST_F(GkConfigRequest, RefusesAnUnknownServiceBeforeSending) {
  RecordingHttp http;
  Gk client{http};
  const auto invalid = client.config(static_cast<CloudService>(99));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().reason(), Reason::invalid_service);
  EXPECT_EQ(http.calls, 0U);
}

class GkConfigResponse : public ::testing::Test {
 protected:
  static auto decode(std::string_view body)
      -> libchiaking::Expected<gk::ConfigKey, gk::Error> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    return client.config(CloudService::legacy_cloud_stream);
  }

  static auto malformed(std::string_view body) -> testing::AssertionResult {
    const auto result = decode(body);
    if (result)
      return testing::AssertionFailure()
             << "decoded the key " << result.value().view();
    if (result.error().reason() != Reason::malformed)
      return testing::AssertionFailure()
             << "failed for another reason than a malformed reply";
    return testing::AssertionSuccess();
  }
};

TEST_F(GkConfigResponse, ReadsTheConfigKey) {
  const auto result = decode(R"({"configKey":"abc123xyz"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "abc123xyz");
}

TEST_F(GkConfigResponse, KeepsAnEscapedNulInTheKey) {
  const auto result = decode(R"({"configKey":"abc\u0000def"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), std::string_view("abc\0def", 7));
}

TEST_F(GkConfigResponse, RefusesAMissingEmptyOrNonTextKey) {
  EXPECT_TRUE(malformed("{}"));
  EXPECT_TRUE(malformed(R"({"configKey":""})"));
  EXPECT_TRUE(malformed(R"({"configKey":123})"));
  EXPECT_TRUE(malformed(R"({"configKey":null})"));
}

TEST_F(GkConfigResponse, RefusesAReplyThatIsNotAnObject) {
  EXPECT_TRUE(malformed("{"));
  EXPECT_TRUE(malformed("[]"));
  EXPECT_TRUE(malformed(""));
}

TEST_F(GkConfigResponse, TakesTheLastOfRepeatedKeys) {
  const auto result = decode(R"({"configKey":"a","configKey":"b"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "b");
}

TEST_F(GkConfigResponse, ReadsTheKeyBesideLongPadding) {
  const auto result =
      decode(std::string{"{\"configKey\":\"a\",\"padding\":\""} +
             std::string(16 * 1024, 'x') + "\"}");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "a");
}

class ConfigProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    if (request.url.view().find("/client_ids") != std::string_view::npos) {
      ++client_ids_calls;
      if (stop_after_client_ids) stop_after_client_ids->stopped = true;
      platform::HttpResponse ids{};
      ids.status_code = 200;
      for (const auto character : std::string_view{R"({"gkClientId":"a"})"})
        ids.body.push_back(static_cast<std::byte>(character));
      return ids;
    }
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.url.view(),
              "https://config.cc.prod.gaikai.com/v1/config");
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response{};
    response.status_code = status;
    for (const auto character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::shared_ptr<test_support::StopState> stop_after_client_ids;
  unsigned client_ids_calls{};
  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"configKey":"synthetic-key"})"};
  unsigned calls{};
};

class GkConfigClient : public ::testing::Test {
 protected:
  auto fetch() -> libchiaking::Expected<gk::ConfigKey, gk::Error> {
    Gk client{provider, running};
    return client.config(CloudService::legacy_cloud_stream);
  }

  static auto failed(
      const libchiaking::Expected<gk::ConfigKey, gk::Error>& result,
      Reason reason, std::uint32_t detail) -> testing::AssertionResult {
    return GkRig::failed(result, gk::Error::Stage::config, reason, detail);
  }

  ConfigProvider provider;
  const platform::StopToken running;
};

TEST_F(GkConfigClient, FetchesTheKeyInOneRequest) {
  const auto result = fetch();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "synthetic-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, CallsAnEmptyReplyMalformed) {
  provider.body.clear();
  EXPECT_TRUE(failed(fetch(), Reason::malformed, 4));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "{not json";
  EXPECT_TRUE(failed(fetch(), Reason::malformed, 4));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, IsRejectedByAnErrorStatus) {
  provider.status = 401;
  EXPECT_TRUE(failed(fetch(), Reason::rejected, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(failed(fetch(), Reason::transport, 2));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = fetch();
  EXPECT_TRUE(failed(result, Reason::cancelled, 1));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkConfigClient, IsNotAskedAfterAStopDuringTheClientIds) {
  provider.stop_after_client_ids = std::make_shared<test_support::StopState>();
  const platform::StopToken stopping{provider.stop_after_client_ids};
  Gk stopped_client{provider, stopping};
  auto waiter = GkRig::recording_waiter();
  const auto opened = play::ServerReservation::open(
      stopped_client, {waiter},
      gk::LaunchSpec::prebuilt(CloudService::legacy_cloud_stream,
                               std::string{GkRig::blank_spec}),
      CloudPlatform::ps4);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().stage(), gk::Error::Stage::client_ids);
  EXPECT_TRUE(opened.error().is_cancelled());
  EXPECT_EQ(opened.error().detail(), 1U);
  EXPECT_EQ(provider.client_ids_calls, 1U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(GkConfigClient, RefusesAnUnknownServiceBeforeSending) {
  Gk client{provider, running};
  EXPECT_TRUE(failed(client.config(static_cast<CloudService>(99)),
                     Reason::invalid_service, 0));
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
