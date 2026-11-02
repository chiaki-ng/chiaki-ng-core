#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::SessionErrorKind;
using session_client_rig::ScriptedHttp;

class SessionClientProfile : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::profile;
    http.response.status_code = 200;
    http.set_body(R"({"url":"https://profile.example.test/base"})");
  }
};

TEST_F(SessionClientProfile, FetchesTheProfileBaseUrl) {
  const auto ok = client.get_ps4_profile_base_url(token, {});
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok.value(), "https://profile.example.test/base");
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientProfile, ReportsARefusalStatus) {
  http.response.status_code = 503;
  const auto status = client.get_ps4_profile_base_url(token, {});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(status.error().status, 503);
}

TEST_F(SessionClientProfile, RefusesAnInsecureBaseUrl) {
  http.set_body(R"({"url":"http://insecure.test"})");
  const auto invalid = client.get_ps4_profile_base_url(token, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, SessionErrorKind::invalid_response);
}

TEST_F(SessionClientProfile, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = client.get_ps4_profile_base_url(token, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientProfile, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = client.get_ps4_profile_base_url(token, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientProfile, RefusesAStoppedOrTokenlessLookupWithoutSending) {
  EXPECT_EQ(client.get_ps4_profile_base_url(token, stopped()).error().kind,
            SessionErrorKind::cancelled);
  EXPECT_EQ(client
                .get_ps4_profile_base_url(
                    libchiaking::psn::auth::BearerToken{std::string_view{}}, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(http.calls, 0U);
}

class SessionClientWake : public session_client_rig::ClientRig {
 protected:
  static constexpr std::string_view base = "https://profile.example.test/base";

  void SetUp() override {
    http.expect = ScriptedHttp::Expect::wake;
    http.response.status_code = 200;
  }

  auto wake(std::string_view base_url, std::string_view id,
            const platform::StopToken& stop = {}) {
    return client.wake_ps4_session(token, session_id, base_url, id, data, data,
                                   stop);
  }
};

TEST_F(SessionClientWake, SendsThePs4Wake) {
  EXPECT_TRUE(wake(base, "a/b"));
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientWake, ReportsARefusalStatus) {
  http.response.status_code = 404;
  const auto not_found = wake(base, "a/b");
  ASSERT_FALSE(not_found);
  EXPECT_EQ(not_found.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(not_found.error().status, 404);
}

TEST_F(SessionClientWake, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = wake(base, "a/b");
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
}

TEST_F(SessionClientWake, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = wake(base, "a/b");
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientWake, RefusesAnInvalidWakeWithoutSending) {
  EXPECT_EQ(wake("http://profile.example.test/base", "a/b").error().kind,
            SessionErrorKind::invalid_request);
  EXPECT_EQ(wake(base, "").error().kind, SessionErrorKind::invalid_request);
  EXPECT_EQ(client
                .wake_ps4_session(token, "bad-session-id", base, "a/b", data,
                                  data, {})
                .error()
                .kind,
            SessionErrorKind::invalid_request);
  EXPECT_EQ(wake(base, "a/b", stopped()).error().kind,
            SessionErrorKind::cancelled);
  EXPECT_EQ(http.calls, 0U);
}

}  // namespace
}  // namespace play_test
