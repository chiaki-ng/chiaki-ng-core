#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::SessionErrorKind;

class SessionClientCreate : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.response.status_code = 201;
    http.set_body(
        R"({"remotePlaySessions":[{"sessionId":"12345678-90ab-4cde-8f01-23456789abcd","members":[{"accountId":"123456789"}]}]})");
  }
};

TEST_F(SessionClientCreate, CreatesASessionWithAFreshPushContext) {
  const auto created = client.create_session(token, {});
  ASSERT_TRUE(created);
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(created.value().push_context_id,
            "00000000-0000-4000-8000-000000000000");
  EXPECT_EQ(created.value().session_id, "12345678-90ab-4cde-8f01-23456789abcd");
  EXPECT_EQ(created.value().account_id, 123456789);
}

TEST_F(SessionClientCreate, KeepsASessionCreatedAsTheStartWasStopped) {
  auto stop_state = std::make_shared<test_support::StopState>();
  http.stop_after_perform = stop_state.get();
  const auto raced_stop =
      client.create_session(token, platform::StopToken{stop_state});
  EXPECT_TRUE(stop_state->stopped);
  ASSERT_TRUE(raced_stop);
  EXPECT_EQ(raced_stop.value().session_id,
            "12345678-90ab-4cde-8f01-23456789abcd");
}

TEST_F(SessionClientCreate, ReportsARefusalStatus) {
  http.response.status_code = 401;
  const auto nonok = client.create_session(token, {});
  ASSERT_FALSE(nonok);
  EXPECT_EQ(nonok.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(nonok.error().status, 401);
}

TEST_F(SessionClientCreate, ReportsAResponseWithoutTheSession) {
  http.response.status_code = 200;
  http.set_body("{}");
  const auto bad_json = client.create_session(token, {});
  ASSERT_FALSE(bad_json);
  EXPECT_EQ(bad_json.error().kind, SessionErrorKind::invalid_response);
}

TEST_F(SessionClientCreate, ReportsAnOversizedResponse) {
  http.response.status_code = 200;
  http.set_body("{}");
  http.response.body.append(std::string(
      libchiaking::json::max_document_bytes + 1 - http.response.body.size(),
      '\0'));
  const auto oversized = client.create_session(token, {});
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(oversized.error().transport,
            platform::HttpError::response_too_large);
}

TEST_F(SessionClientCreate, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = client.create_session(token, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientCreate, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = client.create_session(token, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientCreate, RefusesAnInvalidTokenBeforeDrawingOrSending) {
  EXPECT_EQ(client
                .create_session(
                    libchiaking::psn::auth::BearerToken{std::string_view{}}, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(
      client
          .create_session(libchiaking::psn::auth::BearerToken{std::string_view{
                              "bad\nheader"}},
                          {})
          .error()
          .kind,
      SessionErrorKind::invalid_token);
  EXPECT_EQ(
      client
          .create_session(
              libchiaking::psn::auth::BearerToken{std::string(8193, 'a')}, {})
          .error()
          .kind,
      SessionErrorKind::invalid_token);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(SessionClientCreate, RefusesAStoppedStartBeforeDrawingOrSending) {
  EXPECT_EQ(client.create_session(token, stopped()).error().kind,
            SessionErrorKind::cancelled);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(SessionClientCreate, ReportsARandomFailureBeforeSending) {
  random.fail = true;
  const auto failed_random = client.create_session(token, {});
  ASSERT_FALSE(failed_random);
  EXPECT_EQ(failed_random.error().kind, SessionErrorKind::random_failure);
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(http.calls, 0U);
}

}  // namespace
}  // namespace play_test
