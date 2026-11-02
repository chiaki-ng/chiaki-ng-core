#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using psn_remote_play::SessionErrorKind;
using psn_remote_play::SessionLease;
using session_client_rig::ScriptedHttp;

class SessionClientRelease : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::release;
    http.response.status_code = 204;
  }
};

TEST_F(SessionClientRelease, LeavesTheSession) {
  EXPECT_TRUE(client.release_session(token, session_id, {}));
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientRelease, CompletesALeaveAsTheStartWasStopped) {
  auto stop_state = std::make_shared<test_support::StopState>();
  http.stop_after_perform = stop_state.get();
  EXPECT_TRUE(client.release_session(token, session_id,
                                     platform::StopToken{stop_state}));
  EXPECT_TRUE(stop_state->stopped);
}

TEST_F(SessionClientRelease, ReportsARefusalStatus) {
  http.response.status_code = 404;
  const auto not_found = client.release_session(token, session_id, {});
  ASSERT_FALSE(not_found);
  EXPECT_EQ(not_found.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(not_found.error().status, 404);
}

TEST_F(SessionClientRelease, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = client.release_session(token, session_id, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientRelease, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = client.release_session(token, session_id, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientRelease, RefusesAnInvalidLeaveWithoutSending) {
  EXPECT_EQ(client
                .release_session(
                    libchiaking::psn::auth::BearerToken{std::string_view{}},
                    session_id, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(client.release_session(token, "bad-session-id", {}).error().kind,
            SessionErrorKind::invalid_request);
  EXPECT_EQ(client.release_session(token, session_id, stopped()).error().kind,
            SessionErrorKind::cancelled);
  EXPECT_EQ(http.calls, 0U);
}

class SessionLeaseLifecycle : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.response.status_code = 201;
    http.set_body(created_body);
  }

  auto create() { return SessionLease::create(client, token.clone(), {}); }

  void expect_release() {
    http.expect = ScriptedHttp::Expect::release;
    http.response.status_code = 204;
    http.set_body("");
  }
};

TEST_F(SessionLeaseLifecycle, ReleasesOnceThroughWhicheverLeaseHoldsIt) {
  {
    auto lease = create();
    ASSERT_TRUE(lease);
    EXPECT_EQ(http.calls, 1U);
    EXPECT_EQ(lease.value().created().session_id, session_id);
    EXPECT_EQ(lease.value().bearer_token().view(), token.view());
    auto moved = std::move(lease.value());
    auto moved_from = lease.value().release({});
    ASSERT_FALSE(moved_from);
    EXPECT_EQ(moved_from.error().kind, SessionErrorKind::invalid_request);
    EXPECT_EQ(http.calls, 1U);
    expect_release();
    EXPECT_TRUE(moved.release({}));
    EXPECT_EQ(http.calls, 2U);
    auto repeat = moved.release({});
    ASSERT_FALSE(repeat);
    EXPECT_EQ(repeat.error().kind, SessionErrorKind::invalid_request);
    EXPECT_EQ(http.calls, 2U);
  }
  EXPECT_EQ(http.calls, 2U);
}

TEST_F(SessionLeaseLifecycle, ReportsARefusedCreate) {
  http.response.status_code = 401;
  auto failed = create();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(http.calls, 1U);
}

TEST_F(SessionLeaseLifecycle, ReleasesOnlyOnceEvenWhenTheReleaseFails) {
  auto uncertain = create();
  ASSERT_TRUE(uncertain);
  EXPECT_EQ(http.calls, 1U);
  expect_release();
  http.error = platform::HttpError::timed_out;
  auto release_failed = uncertain.value().release({});
  ASSERT_FALSE(release_failed);
  EXPECT_EQ(release_failed.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(http.calls, 2U);
  http.error.reset();
  auto repeated = uncertain.value().release({});
  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error().kind, SessionErrorKind::invalid_request);
  EXPECT_EQ(http.calls, 2U);
}

}  // namespace
}  // namespace play_test
