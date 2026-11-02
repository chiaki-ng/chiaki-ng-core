#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace play_test {
namespace {

namespace holepunch = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::SessionErrorKind;
using session_client_rig::ScriptedHttp;

class SessionClientShortMessage : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::short_message;
    http.response.status_code = 204;
  }

  auto send(const platform::StopToken& stop = {}) {
    return client.send_short_message(token, session_id,
                                     holepunch::Action::result, 7, 0, 12, uid,
                                     rest::ConsoleFamily::ps5, stop);
  }
};

TEST_F(SessionClientShortMessage, PostsTheMessageToTheConsole) {
  EXPECT_TRUE(send());
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientShortMessage, ReportsARefusalStatus) {
  http.response.status_code = 403;
  const auto forbidden = send();
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(forbidden.error().status, 403);
}

TEST_F(SessionClientShortMessage, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = send();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
  EXPECT_EQ(timed_out.error().transport, platform::HttpError::timed_out);
}

TEST_F(SessionClientShortMessage, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = send();
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientShortMessage, RefusesAnInvalidMessageWithoutSending) {
  EXPECT_EQ(client
                .send_short_message(
                    libchiaking::psn::auth::BearerToken{std::string_view{}},
                    session_id, holepunch::Action::result, 7, 0, 12, uid,
                    rest::ConsoleFamily::ps5, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(client
                .send_short_message(token, "bad-session-id",
                                    holepunch::Action::result, 7, 0, 12, uid,
                                    rest::ConsoleFamily::ps5, {})
                .error()
                .kind,
            SessionErrorKind::invalid_request);
  EXPECT_FALSE(send(stopped()));
  EXPECT_EQ(http.calls, 0U);
}

class SessionClientFullMessage : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.expect = ScriptedHttp::Expect::full_message;
    http.response.status_code = 204;
    conn_request.sid = 1;
    conn_request.peer_sid = 2;
  }

  auto send(const platform::StopToken& stop = {},
            holepunch::Action action = holepunch::Action::offer) {
    return client.send_full_message(token, session_id, action, 7, 0,
                                    conn_request, 12, uid,
                                    rest::ConsoleFamily::ps5, stop);
  }

  holepunch::ConnRequest conn_request{};
};

TEST_F(SessionClientFullMessage, PostsTheOfferToTheConsole) {
  EXPECT_TRUE(send());
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientFullMessage, ReportsARefusalStatus) {
  http.response.status_code = 403;
  const auto forbidden = send();
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().kind, SessionErrorKind::http_status);
  EXPECT_EQ(forbidden.error().status, 403);
}

TEST_F(SessionClientFullMessage, ReportsATransportFailure) {
  http.error = platform::HttpError::timed_out;
  const auto timed_out = send();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, SessionErrorKind::transport_failure);
}

TEST_F(SessionClientFullMessage, ReportsACancelledTransfer) {
  http.error = platform::HttpError::cancelled;
  const auto cancelled = send();
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, SessionErrorKind::cancelled);
}

TEST_F(SessionClientFullMessage, RefusesAnInvalidMessageWithoutSending) {
  EXPECT_EQ(client
                .send_full_message(
                    libchiaking::psn::auth::BearerToken{std::string_view{}},
                    session_id, holepunch::Action::offer, 7, 0, conn_request,
                    12, uid, rest::ConsoleFamily::ps5, {})
                .error()
                .kind,
            SessionErrorKind::invalid_token);
  EXPECT_EQ(client
                .send_full_message(token, "bad-session-id",
                                   holepunch::Action::offer, 7, 0, conn_request,
                                   12, uid, rest::ConsoleFamily::ps5, {})
                .error()
                .kind,
            SessionErrorKind::invalid_request);
  EXPECT_EQ(send({}, holepunch::Action::result).error().kind,
            SessionErrorKind::invalid_request);
  conn_request.candidates.push_back(
      {holepunch::CandidateType::local, std::string(46U, 'a'), "", 0, 0});
  EXPECT_EQ(send().error().kind, SessionErrorKind::invalid_request);
  EXPECT_EQ(send(stopped()).error().kind, SessionErrorKind::cancelled);
  EXPECT_EQ(http.calls, 0U);
}

class SessionClientOfferEffects : public session_client_rig::ClientRig {
 protected:
  void SetUp() override {
    http.response.status_code = 204;
    local_offer.sid = 1;
    local_offer.peer_sid = 2;
  }

  auto execute(const internet::holepunch::OfferEffect& effect,
               const platform::StopToken& stop = {}) {
    return internet::holepunch::Offer::send_effect(
        effect, 7, client, token, session_id, 12, uid, rest::ConsoleFamily::ps5,
        local_offer, stop);
  }

  holepunch::ConnRequest local_offer{};
};

TEST_F(SessionClientOfferEffects, SendsTheAckThenTheOwnOffer) {
  const auto effects =
      internet::holepunch::Offer::make_initial_effects(7, 7, 7);
  ASSERT_TRUE(effects.has_value());
  http.expect = ScriptedHttp::Expect::short_message;
  EXPECT_TRUE(execute(effects.value()[0]));
  http.expect = ScriptedHttp::Expect::full_message;
  EXPECT_TRUE(execute(effects.value()[1]));
  EXPECT_EQ(http.calls, 2U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(SessionClientOfferEffects, RefusesAnEffectItCannotSendWithoutSending) {
  const auto stale = execute(internet::holepunch::OfferEffect{
      internet::holepunch::AckPeerOffer{6, 7, 0}});
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().kind(),
            internet::holepunch::OfferSendErrorKind::stale_attempt);
  const auto invalid_req = execute(internet::holepunch::OfferEffect{
      internet::holepunch::SendOwnOffer{7, 65536}});
  ASSERT_FALSE(invalid_req.has_value());
  EXPECT_EQ(invalid_req.error().kind(),
            internet::holepunch::OfferSendErrorKind::invalid_req_id);
  const auto invalid_error = execute(internet::holepunch::OfferEffect{
      internet::holepunch::AckPeerOffer{7, 7, 65536}});
  ASSERT_FALSE(invalid_error.has_value());
  EXPECT_EQ(invalid_error.error().kind(),
            internet::holepunch::OfferSendErrorKind::invalid_error);
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(SessionClientOfferEffects, ReportsTheSessionFailureBehindASend) {
  const auto effects =
      internet::holepunch::Offer::make_initial_effects(7, 7, 7);
  ASSERT_TRUE(effects.has_value());
  http.expect = ScriptedHttp::Expect::short_message;
  http.response.status_code = 403;
  const auto failed = execute(effects.value()[0]);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().kind(),
            internet::holepunch::OfferSendErrorKind::session_failure);
  EXPECT_EQ(failed.error().session()->kind, SessionErrorKind::http_status);
  EXPECT_EQ(failed.error().session()->status, 403U);
}

TEST_F(SessionClientOfferEffects, ReportsAStoppedSendWithoutSending) {
  const auto effects =
      internet::holepunch::Offer::make_initial_effects(7, 7, 7);
  ASSERT_TRUE(effects.has_value());
  const auto cancelled = execute(effects.value()[0], stopped());
  ASSERT_FALSE(cancelled.has_value());
  EXPECT_EQ(cancelled.error().kind(),
            internet::holepunch::OfferSendErrorKind::session_failure);
  EXPECT_EQ(cancelled.error().session()->kind, SessionErrorKind::cancelled);
  EXPECT_EQ(http.calls, 0U);
}

}  // namespace
}  // namespace play_test
