#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::PushOpenError;
using psn_remote_play::SessionErrorKind;
using session_client_rig::ScriptedHttp;

class SessionClientPushFqdn : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::push_fqdn;
    http.response.status_code = 200;
    http.set_body(
        R"({"fqdn":"mobile-pushcl.np.communication.playstation.net"})");
  }
};

TEST_F(SessionClientPushFqdn, FetchesThePushServer) {
  const auto ok = client.get_push_fqdn(token, {});
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok.value(), "mobile-pushcl.np.communication.playstation.net");
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientPushFqdn, ReportsARefusalStatus) {
  http.response.status_code = 503;
  const auto status = client.get_push_fqdn(token, {});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(status.error().status, 503);
}

TEST_F(SessionClientPushFqdn, RefusesAServerNameWithAPath) {
  http.set_body(R"({"fqdn":"push.example.test/evil"})");
  const auto invalid = client.get_push_fqdn(token, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, SessionErrorKind::invalid_response);
}

TEST_F(SessionClientPushFqdn, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = client.get_push_fqdn(token, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientPushFqdn, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = client.get_push_fqdn(token, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientPushFqdn, RefusesAStoppedOrTokenlessLookupWithoutSending) {
  EXPECT_EQ(client.get_push_fqdn(token, stopped()).error().kind,
            SessionErrorKind::cancelled);
  EXPECT_EQ(client
                .get_push_fqdn(
                    libchiaking::psn::auth::BearerToken{std::string_view{}}, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(http.calls, 0U);
}

class PushConnectionOpen : public SessionClientPushFqdn {
 protected:
  auto open(const libchiaking::psn::auth::BearerToken& bearer,
            const platform::StopToken& stop = {}) {
    return psn_remote_play::PushConnection::open(client, push, bearer, stop);
  }

  session_client_rig::ScriptedWebSocket websocket;
  psn_remote_play::PushClient push{websocket};
};

TEST_F(PushConnectionOpen, LooksUpThePushServerThenConnects) {
  auto opened = open(token);
  ASSERT_TRUE(opened);
  EXPECT_TRUE(opened.value());
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(websocket.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(PushConnectionOpen, StopsWhenThePushServerLookupFails) {
  http.response.status_code = 503;
  auto no_fqdn = open(token);
  ASSERT_FALSE(no_fqdn);
  EXPECT_EQ(no_fqdn.error().kind(), PushOpenError::Kind::get_fqdn_failed);
  EXPECT_EQ(websocket.calls, 0U);
}

TEST_F(PushConnectionOpen, ReportsAFailedConnection) {
  websocket.error = platform::WebSocketError::timed_out;
  auto no_connection = open(token);
  ASSERT_FALSE(no_connection);
  EXPECT_EQ(no_connection.error().kind(), PushOpenError::Kind::connect_failed);
  EXPECT_EQ(websocket.calls, 1U);
}

TEST_F(PushConnectionOpen, RefusesAnEmptyTokenOrAStoppedStartWithoutContact) {
  auto empty = open(libchiaking::psn::auth::BearerToken{std::string_view{}});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().kind(), PushOpenError::Kind::invalid_token);
  auto cancelled = open(token, stopped());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind(), PushOpenError::Kind::cancelled);
  EXPECT_EQ(http.calls, 0U);
  EXPECT_EQ(websocket.calls, 0U);
}

}  // namespace
}  // namespace play_test
