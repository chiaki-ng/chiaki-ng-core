#pragma once

#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::psn_start_rig {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;

class Bodies {
 public:
  static constexpr std::string_view token = "SYNTHETIC_TEST_TOKEN";
  static constexpr std::string_view created =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":"userA"}})";
  static constexpr std::string_view members =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created"})";
  static constexpr std::string_view member_deleted =
      R"({"dataType":"psn:sessionManager:sys:rps:members:deleted","body":{"data":{"sessionId":"another-session"}}})";
  static constexpr std::string_view own_session_deleted =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:deleted","body":{"data":{"sessionId":"01234567-89ab-cdef-0123-456789abcdef"}}})";
  static constexpr std::string_view ready_members =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}}})";
  static constexpr std::string_view ready_custom =
      R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}}})";
};

class FlowHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_FALSE(request.headers.empty());
    if (!request.headers.empty()) {
      EXPECT_EQ(request.headers[0].value.view(), "Bearer SYNTHETIC_TEST_TOKEN");
    }
    platform::HttpResponse response;
    switch (request.method) {
      case platform::HttpMethod::get:
        answer_get(request, response);
        break;
      case platform::HttpMethod::post:
        answer_post(request, response);
        break;
      case platform::HttpMethod::delete_:
        order.push_back("release");
        response.status_code = 204;
        break;
      case platform::HttpMethod::head:
      case platform::HttpMethod::put:
      case platform::HttpMethod::patch:
        ADD_FAILURE() << "unexpected HTTP method";
        break;
    }
    return response.clone();
  }

  std::uint16_t check_status{200};
  std::function<void()> on_check;
  std::uint16_t create_status{201};
  std::function<void()> on_create;
  std::uint16_t start_status{200};
  std::uint16_t profile_status{200};
  std::uint16_t wake_status{200};
  std::vector<std::string> order;

 private:
  void answer_get(const platform::HttpRequest& request,
                  platform::HttpResponse& response) {
    const auto url = request.url.view();
    if (url.find("/remotePlaySessions") != std::string_view::npos) {
      EXPECT_EQ(request.headers.size(), 2U);
      if (request.headers.size() == 2) {
        EXPECT_EQ(request.headers[1].name, "X-PSN-SESSION-MANAGER-SESSION-IDS");
        EXPECT_EQ(request.headers[1].value.view(),
                  "01234567-89ab-cdef-0123-456789abcdef");
      }
      order.push_back(url.ends_with("?view=v1.0") ? "view" : "status");
      if (on_check) on_check();
      response.status_code = check_status;
    } else if (url.find("baseUrls/userProfile") != std::string_view::npos) {
      order.push_back("profile");
      response.status_code = profile_status;
      response.body.assign(
          std::string_view{R"({"url":"https://profile.example.test/base"})"});
    } else {
      order.push_back("fqdn");
      response.status_code = 200;
      response.body.assign(std::string_view{
          R"({"fqdn":"mobile-pushcl.np.communication.playstation.net"})"});
    }
  }

  void answer_post(const platform::HttpRequest& request,
                   platform::HttpResponse& response) {
    const auto url = request.url.view();
    if (url.find("profile.example.test") != std::string_view::npos) {
      order.push_back("wake");
      response.status_code = wake_status;
    } else if (url.find("/commands") != std::string_view::npos) {
      order.push_back("start");
      response.status_code = start_status;
    } else {
      order.push_back("create");
      if (on_create) on_create();
      response.status_code = create_status;
      response.body.assign(std::string_view{
          R"({"remotePlaySessions":[{"sessionId":"01234567-89ab-cdef-0123-456789abcdef","members":[{"accountId":"123456789"}]}]})"});
    }
  }
};

struct PushScript {
  unsigned close{};
  bool fail_close{};
  unsigned receive{};
  unsigned connect{};
  std::function<void()> on_exhaustion;
  std::function<void()> on_drain;
  std::vector<std::string_view> after_exhaustion;
  std::function<void()> on_receive;
  std::function<void()> on_close;
};

class FlowConnection final : public platform::WebSocketConnection {
 public:
  FlowConnection(std::shared_ptr<PushScript> script,
                 std::vector<std::string_view> lines)
      : script_(std::move(script)), lines_(std::move(lines)) {}

  auto receive(const platform::StopToken& stop)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    EXPECT_FALSE(stop.stop_requested());
    ++script_->receive;
    std::string_view text;
    if (index_ == lines_.size()) {
      if (script_->on_exhaustion && !exhausted_) {
        exhausted_ = true;
        script_->on_exhaustion();
        EXPECT_TRUE(stop.stop_requested());
        return libchiaking::unexpected(platform::WebSocketError::cancelled);
      }
      if (after_index_ == script_->after_exhaustion.size()) {
        if (script_->on_drain) {
          script_->on_drain();
          EXPECT_TRUE(stop.stop_requested());
          return libchiaking::unexpected(platform::WebSocketError::cancelled);
        }
        return libchiaking::unexpected(
            platform::WebSocketError::backend_failure);
      }
      text = script_->after_exhaustion[after_index_++];
    } else {
      text = lines_[index_++];
    }
    if (script_->on_receive) script_->on_receive();
    platform::WebSocketMessage message;
    message.payload.assign(text);
    return message;
  }

  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    ++script_->close;
    if (script_->on_close) script_->on_close();
    if (script_->fail_close)
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    return {};
  }

 private:
  std::shared_ptr<PushScript> script_;
  std::vector<std::string_view> lines_;
  std::size_t index_{};
  std::size_t after_index_{};
  bool exhausted_{};
};

class FlowWebSocket final : public platform::WebSocketProvider {
 public:
  FlowWebSocket(std::shared_ptr<PushScript> script,
                std::vector<std::string_view> lines, FlowHttp& http)
      : script_(std::move(script)), lines_(std::move(lines)), http_(http) {}

  auto connect(const platform::WebSocketRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<std::unique_ptr<platform::WebSocketConnection>,
                               platform::WebSocketError> override {
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(http_.order, std::vector<std::string>{"fqdn"});
    EXPECT_FALSE(request.headers.empty());
    if (!request.headers.empty()) {
      EXPECT_EQ(request.headers[0].value.view(), "Bearer SYNTHETIC_TEST_TOKEN");
    }
    ++script_->connect;
    std::unique_ptr<platform::WebSocketConnection> connection =
        std::make_unique<FlowConnection>(script_, lines_);
    return connection;
  }

 private:
  std::shared_ptr<PushScript> script_;
  std::vector<std::string_view> lines_;
  FlowHttp& http_;
};

class StartRig {
 public:
  using Opened = libchiaking::Expected<psn_remote_play::StartedSession,
                                       psn_remote_play::StartError>;
  using Woken = libchiaking::Expected<psn_remote_play::ConsoleReady,
                                      libchiaking::play::WakeFailure>;

  static constexpr std::array<std::byte, 32> uid{};
  static constexpr std::array<std::byte, 16> data{};

  explicit StartRig(std::vector<std::string_view> notifications)
      : websocket{script, std::move(notifications), http} {}
  StartRig(const StartRig&) = delete;
  auto operator=(const StartRig&) -> StartRig& = delete;

  auto open(platform::StopToken stop,
            libchiaking::net::RemoteWaitDeadline deadline,
            std::uint64_t attempt = 7) -> Opened {
    return psn_remote_play::StartedSession::open(
        session, push, creation,
        libchiaking::psn::auth::BearerToken{Bodies::token}, attempt, clock,
        std::move(stop), deadline);
  }

  auto wake_ps5(psn_remote_play::StartedSession& opened,
                platform::StopToken stop,
                libchiaking::net::RemoteWaitDeadline deadline) -> Woken {
    return libchiaking::play::InternetStart::wake_ps5(
        opened, session, push, uid, data, data, clock, std::move(stop),
        deadline);
  }

  auto wake_ps4(psn_remote_play::StartedSession& opened,
                platform::StopToken stop,
                libchiaking::net::RemoteWaitDeadline deadline) -> Woken {
    return libchiaking::play::InternetStart::wake_ps4(
        opened, session, push, data, data, clock, std::move(stop), deadline);
  }

  auto releases() const -> std::ptrdiff_t {
    return std::ranges::count(http.order, std::string_view{"release"});
  }

  void expect_psn_failure(const Woken& woken, bool waited) const {
    ASSERT_FALSE(woken);
    const auto* failure = woken.error().as<libchiaking::play::PsnFailure>();
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->as<libchiaking::play::PsnFailure::Wait>() != nullptr,
              waited);
  }

  test_support::FakeClock clock{100};
  FlowHttp http;
  test_support::FixedRandom random{std::byte{}};
  std::shared_ptr<PushScript> script = std::make_shared<PushScript>();
  FlowWebSocket websocket;
  psn_remote_play::SessionClient session{
      http, test_support::LiveProviders::steady(), random};
  psn_remote_play::PushClient push{websocket};
  psn_remote_play::CreationBarrier creation{7};
};

}  // namespace play_test::psn_start_rig
