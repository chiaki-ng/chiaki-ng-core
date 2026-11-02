#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::SessionErrorKind;
using session_client_rig::ScriptedHttp;

class SessionClientStart : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::start;
    http.response.status_code = 200;
  }

  auto start(const rest::CreatedSession& created,
             const platform::StopToken& stop = {}) {
    return client.start_ps5_session(token, created, uid, data, data, stop);
  }
};

TEST_F(SessionClientStart, SendsThePs5StartCommand) {
  EXPECT_TRUE(start(created_session()));
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientStart, ReportsARefusalStatus) {
  http.response.status_code = 403;
  const auto forbidden = start(created_session());
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(forbidden.error().status, 403);
}

TEST_F(SessionClientStart, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = start(created_session());
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientStart, RefusesAnInvalidSessionWithoutSending) {
  const rest::CreatedSession invalid{"", "wrong", 123};
  const auto invalid_request = start(invalid);
  ASSERT_FALSE(invalid_request);
  EXPECT_EQ(invalid_request.error().kind, SessionErrorKind::invalid_request);
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(SessionClientStart, RefusesAStoppedStartWithoutSending) {
  const auto cancelled = start(created_session(), stopped());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
  EXPECT_EQ(http.calls, 0U);
}

class SessionClientStartDiagnostics : public ::testing::Test {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";
  static constexpr std::array<std::byte, 32> uid{};
  static constexpr std::array<std::byte, 16> data{};

  static auto status_url() -> std::string {
    return std::string{url} +
           "?platform=PS5&commandIds=PRIVATE_RECEIPT&includeFields=actions,"
           "commandDetail";
  }

  auto start() {
    return client.start_ps5_session(token, created, uid, data, data, {});
  }

  auto never_logged(std::string_view text) const -> ::testing::AssertionResult {
    for (const auto& record : log.messages)
      if (record.find(text) != std::string::npos)
        return ::testing::AssertionFailure()
               << "logged " << text << ": " << record;
    return ::testing::AssertionSuccess();
  }

  void expect_private_status(test_support::psn::Reply reply,
                             std::string_view summary) {
    http.on("POST", std::string{url},
            {{202, R"({"commandId":"PRIVATE_RECEIPT"})"}});
    http.on("GET", status_url(), {std::move(reply)});
    EXPECT_TRUE(start());
    client.diagnose_start_command(token, created.session_id);
    EXPECT_EQ(http.seen.size(), 2U);
    ASSERT_FALSE(log.messages.empty());
    EXPECT_NE(log.messages.back().find(summary), std::string::npos);
    EXPECT_TRUE(never_logged("PRIVATE_"));
  }

  const rest::CreatedSession created{"00000000-0000-4000-8000-000000000000",
                                     "01234567-89ab-cdef-0123-456789abcdef",
                                     123};
  session_client_rig::SwitchableRandom random;
  test_support::psn::FakeHttp http;
  test_support::FakeLog log;
  psn_remote_play::SessionClient client{
      http, test_support::LiveProviders::steady(), random, &log};
};

TEST_F(SessionClientStartDiagnostics,
       AsksAboutTheLatestCommandOfItsOwnSessionOnce) {
  http.on("POST", std::string{url},
          {{202, R"({"commandId":"PRIVATE_RECEIPT"})"},
           {202, R"({"commandId":"PRIVATE_RECEIPT"})"},
           {503, "{}"},
           {202, "{}"}});
  http.on(
      "GET", status_url(),
      {{200,
        R"({"commands":[{"commandStatus":"failed","error":{"code":1234,"message":"PRIVATE_MESSAGE"}}]})"}});
  EXPECT_TRUE(start());
  client.diagnose_start_command(token, "another-session");
  EXPECT_EQ(http.seen.size(), 1U);
  client.diagnose_start_command(
      token, created.session_id,
      platform::StopToken{std::make_shared<test_support::Stopped>()});
  EXPECT_EQ(http.seen.size(), 1U);
  client.diagnose_start_command(token, created.session_id);
  ASSERT_EQ(http.seen.size(), 2U);
  EXPECT_EQ(http.seen.back().total_timeout, std::chrono::milliseconds{2000});
  ASSERT_FALSE(log.messages.empty());
  EXPECT_NE(log.messages.back().find(
                "status=failed error_code_present=1 error_code=1234"),
            std::string::npos);
  client.diagnose_start_command(token, created.session_id);
  EXPECT_EQ(http.seen.size(), 2U);
  EXPECT_TRUE(start());
  EXPECT_FALSE(start());
  client.diagnose_start_command(token, created.session_id);
  EXPECT_EQ(http.seen.size(), 4U);
  EXPECT_TRUE(start());
  client.diagnose_start_command(token, created.session_id);
  EXPECT_EQ(http.seen.size(), 5U);
  for (const auto secret : {token.view(), std::string_view{created.session_id},
                            std::string_view{"PRIVATE_RECEIPT"},
                            std::string_view{"PRIVATE_MESSAGE"}})
    EXPECT_TRUE(never_logged(secret));
}

TEST_F(SessionClientStartDiagnostics, LogsAnUnrecognisedStatusWithoutItsText) {
  expect_private_status(
      {200,
       R"({"commands":[{"commandStatus":"PRIVATE_STATUS","error":{"code":4294967296}}]})"},
      "status=unrecognized error_code_present=0");
}

TEST_F(SessionClientStartDiagnostics, LogsARefusedStatusQueryWithoutItsText) {
  expect_private_status({503, R"({"error":"PRIVATE_MESSAGE PRIVATE_RECEIPT"})"},
                        "status query failed");
}

TEST_F(SessionClientStartDiagnostics, LogsAMalformedStatusWithoutItsText) {
  expect_private_status({200, "malformed PRIVATE_MESSAGE"},
                        "status query failed");
}

}  // namespace
}  // namespace play_test
