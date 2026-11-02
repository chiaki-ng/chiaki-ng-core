#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Stage = remote_play::FailureStage;

class InternetLinkFailure : public ::testing::Test {
 protected:
  static constexpr std::array holepunch_stages{Stage::control_holepunch,
                                               Stage::data_holepunch};

  template <class Route>
  static auto described(Stage stage, const Route& route) -> play::LinkFailure {
    play::LinkFailure failure{stage, remote_play::StreamError::route_failed,
                              play::RouteFailure{route}};
    failure.reached_through(play::Venue::internet());
    return failure;
  }

  static auto push_failure() -> internet::holepunch::OfferPsnError {
    psn_remote_play::BoundedPushReceiveError push{};
    push.kind = psn_remote_play::BoundedPushReceiveErrorKind::push_failure;
    push.push.kind = psn_remote_play::PushErrorKind::websocket_failure;
    push.push.backend = platform::WebSocketError::timed_out;
    return internet::holepunch::OfferPsnError::received(push);
  }

  static auto invalid_notification() -> internet::holepunch::OfferPsnError {
    psn_remote_play::BoundedPushReceiveError push{};
    push.kind = psn_remote_play::BoundedPushReceiveErrorKind::push_failure;
    push.push.kind = psn_remote_play::PushErrorKind::invalid_notification;
    return internet::holepunch::OfferPsnError::received(push);
  }
};

TEST_F(InternetLinkFailure, NamesATimedOutAcceptWait) {
  const internet::holepunch::CtrlAcceptTransportError failure{
      internet::holepunch::SelectedAcceptExchangeError{
          internet::holepunch::AcceptPsnError{
              internet::holepunch::AcceptPsnErrorKind::timed_out}}};
  EXPECT_EQ(play::RouteFailure::of(failure).describe(),
            "timed out waiting for the console ACCEPT");
}

TEST_F(InternetLinkFailure, KeepsTheHttpStatusOfAFailedAcceptAcknowledgement) {
  psn_remote_play::SessionError session{};
  session.status = 503;
  const internet::holepunch::CtrlAcceptTransportError failure{
      internet::holepunch::SelectedAcceptExchangeError{
          internet::holepunch::AcceptPsnError::sent(
              internet::holepunch::AcceptPsnErrorKind::send_failure,
              internet::holepunch::AcceptAckError::refused(session))}};
  EXPECT_EQ(play::RouteFailure::of(failure).http_status(), 503);
  EXPECT_EQ(play::RouteFailure::of(failure).describe(),
            "acknowledging the console ACCEPT through PSN failed");
}

TEST_F(InternetLinkFailure, NamesAMalformedLocalOfferInAcceptPreparation) {
  const internet::holepunch::CtrlAcceptTransportError failure{
      internet::holepunch::SelectedAcceptExchangeError{
          internet::holepunch::SelectedAcceptError::malformed_local_offer}};
  EXPECT_EQ(
      play::RouteFailure::of(failure).describe(),
      "ACCEPT preparation failed: local offer candidate layout is invalid");
}

TEST_F(InternetLinkFailure, NamesAFailedRudpAdoptionOfTheAcceptedSocket) {
  const internet::holepunch::CtrlAcceptTransportError failure{
      internet::holepunch::CtrlRudpHandoffError::adoption_failed};
  EXPECT_EQ(play::RouteFailure::of(failure).describe(),
            "adopting the accepted control socket into RUDP failed");
}

TEST_F(InternetLinkFailure, KeepsTheStageAndVenueOfAnOfferPushFailure) {
  for (const auto stage : holepunch_stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    const auto detail = described(stage, push_failure());
    EXPECT_EQ(detail.stage(), stage);
    EXPECT_EQ(detail.route(), play::Venue::internet());
    EXPECT_EQ(detail.describe(), "the PSN notification WebSocket timed out");
  }
}

TEST_F(InternetLinkFailure, NamesAnInvalidPushNotificationInAnOffer) {
  for (const auto stage : holepunch_stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    const auto failure = invalid_notification();
    EXPECT_EQ(described(stage, failure).describe(),
              "a PSN notification was invalid");
  }
}

TEST_F(InternetLinkFailure, NamesADisconnectedPushWhileWaitingForAccept) {
  for (const auto stage : holepunch_stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    psn_remote_play::BoundedPushReceiveError push{};
    push.kind = psn_remote_play::BoundedPushReceiveErrorKind::push_failure;
    push.push.kind = psn_remote_play::PushErrorKind::websocket_failure;
    push.push.backend = platform::WebSocketError::disconnected;
    const internet::holepunch::SelectedAcceptExchangeError accept{
        internet::holepunch::AcceptPsnError::received(push)};
    EXPECT_EQ(described(stage, accept).describe(),
              "the PSN notification WebSocket disconnected");
  }
}

TEST_F(InternetLinkFailure, KeepsTheHttpStatusOfARejectedOfferSend) {
  for (const auto stage : holepunch_stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    psn_remote_play::SessionError session{};
    session.kind = psn_remote_play::SessionErrorKind::http_status;
    session.status = 503;
    const auto failure = internet::holepunch::OfferPsnError::sent(
        internet::holepunch::OfferPsnErrorKind::send_failure,
        internet::holepunch::OfferSendError::refused(session));
    const auto detail = described(stage, failure);
    EXPECT_EQ(detail.stage(), stage);
    EXPECT_EQ(detail.http_status(), 503);
    EXPECT_EQ(detail.describe(), "PSN rejected the request");
  }
}

}  // namespace
}  // namespace play_test
