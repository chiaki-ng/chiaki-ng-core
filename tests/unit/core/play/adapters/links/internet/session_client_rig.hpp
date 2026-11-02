#pragma once

#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/psn/notifications.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::session_client_rig {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;

class Headers {
 public:
  template <class Request>
  static void expect(const Request& request, std::size_t index,
                     std::string_view name, std::string_view value) {
    ASSERT_LT(index, request.headers.size());
    EXPECT_EQ(request.headers[index].name, name);
    EXPECT_EQ(request.headers[index].value.view(), value);
  }
};

class SwitchableRandom final : public platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::PlatformError> override {
    ++calls;
    if (fail)
      return libchiaking::unexpected(platform::PlatformError::io_failure);
    std::ranges::fill(output, std::byte{});
    return {};
  }

  unsigned calls{};
  bool fail{};
};

class ScriptedHttp final : public platform::HttpProvider {
 public:
  enum class Expect {
    create,
    start,
    profile,
    push_fqdn,
    wake,
    release,
    short_message,
    full_message
  };

  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(request.method, method());
    expect_route(request);
    Headers::expect(request, 0, "Authorization", "Bearer SYNTHETIC_TEST_TOKEN");
    if (expect != Expect::profile && expect != Expect::wake &&
        expect != Expect::push_fqdn)
      Headers::expect(request, 1, "Content-Type",
                      "application/json; charset=utf-8");
    EXPECT_EQ(request.connect_timeout,
              libchiaking::net::http::default_connect_timeout);
    EXPECT_EQ(request.total_timeout,
              libchiaking::net::http::default_total_timeout);
    EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.maximum_response_bytes,
              libchiaking::json::max_document_bytes);
    EXPECT_EQ(request.tls_configuration.trust_mode,
              platform::TlsTrustMode::system);
    expect_body(std::string_view{request.body.view()});
    if (stop_after_perform) stop_after_perform->stopped = true;
    if (error) return libchiaking::unexpected(*error);
    return response.clone();
  }

  void set_body(std::string_view body) { response.body.assign(body); }

  Expect expect{Expect::create};
  platform::HttpResponse response;
  std::optional<platform::HttpError> error;
  test_support::StopState* stop_after_perform{};
  unsigned calls{};

 private:
  auto method() const -> platform::HttpMethod {
    switch (expect) {
      case Expect::push_fqdn:
      case Expect::profile:
        return platform::HttpMethod::get;
      case Expect::release:
        return platform::HttpMethod::delete_;
      case Expect::create:
      case Expect::start:
      case Expect::wake:
      case Expect::short_message:
      case Expect::full_message:
        break;
    }
    return platform::HttpMethod::post;
  }

  void expect_route(const platform::HttpRequest& request) const {
    const auto url = request.url.view();
    switch (expect) {
      case Expect::push_fqdn:
        EXPECT_EQ(url,
                  "https://mobile-pushcl.np.communication.playstation.net/np/"
                  "serveraddr?version=2.1&fields=keepAliveStatus&"
                  "keepAliveStatusType=3");
        EXPECT_EQ(request.headers.size(), 1U);
        EXPECT_TRUE(request.body.empty());
        return;
      case Expect::profile:
        EXPECT_EQ(url,
                  "https://asm.np.community.playstation.net/asm/v1/apps/me/"
                  "baseUrls/userProfile");
        EXPECT_EQ(request.headers.size(), 5U);
        expect_client_headers(request, "asm.np.community.playstation.net");
        EXPECT_TRUE(request.body.empty());
        return;
      case Expect::wake:
        EXPECT_EQ(url,
                  "https://profile.example.test/base/v1/users/a%2Fb/"
                  "remoteConsole/wakeUp?platform=PS4");
        EXPECT_EQ(request.headers.size(), 5U);
        expect_client_headers(request, "profile.example.test");
        return;
      case Expect::start:
        EXPECT_EQ(url,
                  "https://web.np.playstation.com/api/cloudAssistedNavigation/"
                  "v2/users/me/commands");
        EXPECT_EQ(request.headers.size(), 3U);
        Headers::expect(request, 2, "User-Agent", "RpNetHttpUtilImpl");
        return;
      case Expect::short_message:
      case Expect::full_message:
        EXPECT_EQ(url,
                  "https://web.np.playstation.com/api/sessionManager/v1/"
                  "remotePlaySessions/01234567-89ab-cdef-0123-456789abcdef/"
                  "sessionMessage");
        EXPECT_EQ(request.headers.size(), 2U);
        return;
      case Expect::release:
        EXPECT_EQ(url,
                  "https://web.np.playstation.com/api/sessionManager/v1/"
                  "remotePlaySessions/01234567-89ab-cdef-0123-456789abcdef/"
                  "members/me");
        EXPECT_EQ(request.headers.size(), 2U);
        return;
      case Expect::create:
        EXPECT_EQ(url,
                  "https://web.np.playstation.com/api/sessionManager/v1/"
                  "remotePlaySessions");
        EXPECT_EQ(request.headers.size(), 2U);
        return;
    }
  }

  static void expect_client_headers(const platform::HttpRequest& request,
                                    std::string_view host) {
    Headers::expect(request, 1, "Host", host);
    Headers::expect(request, 2, "Connection", "Keep-Alive");
    Headers::expect(request, 3, "Content-Type",
                    "application/json; charset=utf-8");
    Headers::expect(request, 4, "User-Agent", "RpNetHttpUtilImpl");
  }

  void expect_body(std::string_view body) const {
    switch (expect) {
      case Expect::push_fqdn:
      case Expect::profile:
      case Expect::release:
        EXPECT_TRUE(body.empty());
        return;
      case Expect::wake:
        EXPECT_EQ(
            body,
            R"json({"data":{"clientType":"Windows","data1":"AAAAAAAAAAAAAAAAAAAAAA==","data2":"AAAAAAAAAAAAAAAAAAAAAA==","roomId":0,"protocolVer":"10.0","sessionId":"01234567-89ab-cdef-0123-456789abcdef"},"dataTypeSuffix":"remotePlay"})json");
        return;
      case Expect::start:
        EXPECT_EQ(
            body,
            std::string{
                R"json({"commandDetail":{"commandType":"remotePlay","duid":")json"} +
                std::string(64, '0') +
                R"json(","messageDestination":"SQS","parameters":{"initialParams":"{\"accountId\":123,\"roomId\":0,\"sessionId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"clientType\":\"Windows\",\"data1\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"data2\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}"},"platform":"PS5"}})json");
        return;
      case Expect::short_message:
        EXPECT_EQ(
            body,
            "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, "
            "body="
            "{\\\"action\\\":\\\"RESULT\\\",\\\"reqId\\\":7,\\\"error\\\":0,"
            "\\\"connRequest\\\":{}}\",\"to\":[{\"accountId\":\"12\","
            "\"deviceUniqueId\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000"
            "\","
            "\"platform\":\"PS5\"}]}");
        return;
      case Expect::full_message:
        EXPECT_EQ(
            body,
            "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, "
            "body="
            "{\\\"action\\\":\\\"OFFER\\\",\\\"reqId\\\":7,\\\"error\\\":0,"
            "\\\"connRequest\\\":{\\\"sid\\\":1,\\\"peerSid\\\":2,"
            "\\\"skey\\\":\\\"AAAAAAAAAAAAAAAAAAAAAA==\\\",\\\"natType\\\":0,"
            "\\\"candidate\\\":[],\\\"defaultRouteMacAddr\\\":\\\"\\\","
            "\\\"localPeerAddr\\\":{\\\"accountId\\\":\\\"12\\\","
            "\\\"platform\\\":\\\"REMOTE_PLAY\\\"},\\\"localHashedId\\\":"
            "\\\"\\\"}}"
            "\",\"to\":[{\"accountId\":\"12\","
            "\"deviceUniqueId\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000"
            "\","
            "\"platform\":\"PS5\"}]}");
        return;
      case Expect::create:
        EXPECT_EQ(
            body,
            R"({"remotePlaySessions":[{"members":[{"accountId":"me","deviceUniqueId":"me","platform":"me","pushContexts":[{"pushContextId":"00000000-0000-4000-8000-000000000000"}]}]}]})");
        return;
    }
  }
};

class StatusHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    last = request.clone();
    ++calls;
    platform::HttpResponse response;
    response.status_code = status;
    return response.clone();
  }

  platform::HttpRequest last;
  unsigned calls{};
  std::uint16_t status{200};
};

class ClosedConnection final : public platform::WebSocketConnection {
 public:
  auto receive(const platform::StopToken&)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    return libchiaking::unexpected(platform::WebSocketError::backend_failure);
  }

  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    return {};
  }
};

class ScriptedWebSocket final : public platform::WebSocketProvider {
 public:
  auto connect(const platform::WebSocketRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<std::unique_ptr<platform::WebSocketConnection>,
                               platform::WebSocketError> override {
    ++calls;
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(request.url.view(),
              "wss://mobile-pushcl.np.communication.playstation.net/"
              "np/pushNotification");
    EXPECT_EQ(request.headers.size(), 8U);
    Headers::expect(request, 0, "Authorization", "Bearer SYNTHETIC_TEST_TOKEN");
    Headers::expect(request, 1, "User-Agent", "WebSocket++/0.8.2");
    Headers::expect(request, 2, "X-PSN-APP-TYPE", "REMOTE_PLAY");
    Headers::expect(request, 3, "X-PSN-APP-VER", "RemotePlay/1.0");
    Headers::expect(request, 4, "X-PSN-KEEP-ALIVE-STATUS-TYPE", "3");
    Headers::expect(request, 5, "X-PSN-OS-VER", "Windows/10.0");
    Headers::expect(request, 6, "X-PSN-PROTOCOL-VERSION", "2.1");
    Headers::expect(request, 7, "X-PSN-RECONNECTION", "false");
    EXPECT_EQ(request.subprotocol, "np-pushpacket");
    EXPECT_EQ(request.connect_timeout, std::chrono::seconds{30});
    EXPECT_EQ(request.ping_interval, std::chrono::seconds{5});
    EXPECT_EQ(request.close_mode, platform::WebSocketCloseMode::transport_only);
    EXPECT_TRUE(request.ping_immediately);
    EXPECT_EQ(request.pong_timeout, std::chrono::seconds{5});
    EXPECT_EQ(request.maximum_message_bytes, 64U * 1024U);
    EXPECT_EQ(request.tls_configuration.trust_mode,
              platform::TlsTrustMode::system);
    if (error) return libchiaking::unexpected(*error);
    std::unique_ptr<platform::WebSocketConnection> connection =
        std::make_unique<ClosedConnection>();
    return connection;
  }

  std::optional<platform::WebSocketError> error;
  unsigned calls{};
};

class QueuedPushConnection final : public platform::WebSocketConnection {
 public:
  void add(platform::WebSocketMessageType type, std::string_view text) {
    platform::WebSocketMessage message;
    message.type = type;
    message.payload.assign(text);
    messages_.push_back(std::move(message));
  }

  void add(std::string_view text) {
    add(platform::WebSocketMessageType::text, text);
  }

  auto receive(const platform::StopToken&)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    ++receive_calls;
    if (error) return libchiaking::unexpected(*error);
    if (index_ >= messages_.size()) {
      ADD_FAILURE() << "the push connection ran out of messages";
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    }
    return messages_[index_++].clone();
  }

  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    return {};
  }

  std::optional<platform::WebSocketError> error;
  unsigned receive_calls{};

 private:
  std::vector<platform::WebSocketMessage> messages_;
  std::size_t index_{};
};

class Notifications {
 public:
  using Type = rest::NotificationType;

  static constexpr std::string_view created_a =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","sessionId":"01234567-89ab-cdef-0123-456789abcdef","accountId":123456789,"to":{"onlineId":"userA"}})";
  static constexpr std::string_view created_b =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","sessionId":"01234567-89ab-cdef-0123-456789abcdef","accountId":987654321,"to":{"onlineId":"userB"}})";
  static constexpr std::string_view data_zero =
      "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09";
  static constexpr std::string_view data_one =
      "RVJFUkVSRVJFUkVSRVJFUkVSRVJFUT09";

  static auto typed(Type type, std::string_view body) -> rest::Notification {
    return test_support::psn::Notifications::typed(type, body);
  }

  static auto member_uid_body(std::string_view hex64) -> std::string {
    return R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":")" +
           std::string(hex64) + R"("}]}}})";
  }

  static auto custom_data1_body(std::string_view encoded) -> std::string {
    return R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":")" +
           std::string(encoded) + R"("}}})";
  }
};

class ClientRig : public ::testing::Test {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";
  static constexpr std::array<std::byte, 32> uid{};
  static constexpr std::array<std::byte, 16> data{};
  static constexpr std::string_view created_body =
      R"({"remotePlaySessions":[{"sessionId":"01234567-89ab-cdef-0123-456789abcdef","members":[{"accountId":"123456789"}]}]})";

  static auto stopped() -> platform::StopToken {
    return platform::StopToken{std::make_shared<test_support::Stopped>()};
  }

  static auto created_session() -> rest::CreatedSession {
    return {"00000000-0000-4000-8000-000000000000",
            "01234567-89ab-cdef-0123-456789abcdef", 123};
  }

  SwitchableRandom random;
  ScriptedHttp http;
  psn_remote_play::SessionClient client{
      http, test_support::LiveProviders::steady(), random};
};

}  // namespace play_test::session_client_rig
