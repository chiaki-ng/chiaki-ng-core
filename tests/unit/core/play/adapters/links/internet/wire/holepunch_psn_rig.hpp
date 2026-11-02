#pragma once

#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/session_client.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/psn/notifications.hpp"
#include "tests/support/random.hpp"
#include "tests/support/websocket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/websocket/provider.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::holepunch {

namespace pf = libchiaking::platform;

class PushQueue final : public pf::WebSocketConnection {
 public:
  void push(std::string_view text) {
    pf::WebSocketMessage message{};
    message.payload.assign(text);
    messages_.push_back(std::move(message));
  }

  void fail() { fail_ = true; }

  [[nodiscard]] auto calls() const -> std::size_t { return calls_; }

  auto receive(const pf::StopToken& stop)
      -> libchiaking::Expected<pf::WebSocketMessage,
                               pf::WebSocketError> override {
    EXPECT_FALSE(stop.stop_requested());
    if (fail_) {
      ++calls_;
      return libchiaking::unexpected(pf::WebSocketError::backend_failure);
    }
    if (calls_ >= messages_.size())
      return libchiaking::unexpected(pf::WebSocketError::backend_failure);
    return std::move(messages_[calls_++]);
  }

  auto close() -> libchiaking::Expected<void, pf::WebSocketError> override {
    return {};
  }

 private:
  std::vector<pf::WebSocketMessage> messages_;
  std::size_t calls_{};
  bool fail_{};
};

class SessionMessagePost final : public pf::HttpProvider {
 public:
  explicit SessionMessagePost(test_support::FakeClock& clock) noexcept
      : clock_(clock) {}

  [[nodiscard]] auto bodies() const -> const std::vector<std::string>& {
    return bodies_;
  }
  [[nodiscard]] auto checks() const -> unsigned { return checks_; }

  void fail_every_post() { fail_ = true; }
  void fail_post(std::size_t index) { fail_post_index_ = index; }
  void reject_post(std::size_t index) { reject_post_index_ = index; }
  void answer_checks_with(std::uint16_t status) {
    answers_checks_ = true;
    check_status_ = status;
  }
  void expire_on_check() { expire_check_ = true; }
  void expire_and_cancel() { expire_and_cancel_ = true; }
  void after_second_post(std::function<void()> work) {
    after_second_post_ = std::move(work);
  }

  auto perform(const pf::HttpRequest& request, const pf::StopToken& stop)
      -> libchiaking::Expected<pf::HttpResponse, pf::HttpError> override {
    EXPECT_FALSE(stop.stop_requested());
    if (request.method == pf::HttpMethod::get) {
      EXPECT_TRUE(answers_checks_);
      EXPECT_TRUE(
          request.url.view().ends_with("/remotePlaySessions?view=v1.0"));
      EXPECT_GE(bodies_.size(), 2U);
      ++checks_;
      if (expire_check_) clock_.now_us = expiry;
      pf::HttpResponse response;
      response.status_code = check_status_;
      return response;
    }
    EXPECT_EQ(request.method, pf::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/sessionMessage"), std::string::npos);
    EXPECT_EQ(request.headers.front().value.view(),
              "Bearer SYNTHETIC_TEST_TOKEN");
    bodies_.emplace_back(request.body.view());
    if (expire_and_cancel_) {
      clock_.now_us = expiry;
      return libchiaking::unexpected(pf::HttpError::cancelled);
    }
    const auto index = bodies_.size() - 1;
    if (fail_ || index == fail_post_index_)
      return libchiaking::unexpected(pf::HttpError::backend_failure);
    if (bodies_.size() == 2 && after_second_post_) after_second_post_();
    pf::HttpResponse response{};
    response.status_code = index == reject_post_index_ ? 400 : 204;
    return response;
  }

  static constexpr std::uint64_t expiry = 10000;

 private:
  static constexpr auto never = std::numeric_limits<std::size_t>::max();

  test_support::FakeClock& clock_;
  std::vector<std::string> bodies_;
  bool fail_{};
  std::size_t fail_post_index_{never};
  std::size_t reject_post_index_{never};
  bool answers_checks_{};
  unsigned checks_{};
  std::uint16_t check_status_{200};
  bool expire_check_{};
  bool expire_and_cancel_{};
  std::function<void()> after_second_post_;
};

class PsnRig {
  test_support::FixedRandom random_{std::byte{}};

 public:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";
  static constexpr std::int64_t account_id = 12;
  static constexpr std::array<std::byte, 32> uid{};
  static constexpr auto family = libchiaking::psn::rest::ConsoleFamily::ps5;
  static constexpr std::uint64_t deadline = SessionMessagePost::expiry;

  static auto envelope(std::string_view payload) -> std::string {
    return test_support::psn::Notifications::envelope(payload, account_id, uid,
                                                      family);
  }

  test_support::FakeClock clock{100};
  SessionMessagePost http{clock};
  test_support::RefusingWebSocket websocket{
      libchiaking::platform::WebSocketError::backend_failure};
  PushQueue connection;
  libchiaking::psn::remote_play::SessionClient session{
      http, test_support::LiveProviders::steady(), random_};
  libchiaking::psn::remote_play::PushClient push{websocket};
};

}  // namespace play_test::holepunch
