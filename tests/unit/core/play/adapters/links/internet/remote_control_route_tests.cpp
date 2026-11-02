#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_control_route_rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
using remote_route_rig::HeardRoutes;
using remote_route_rig::Messages;
using Route = HeardRoutes::Route;

class RemoteControlRoute : public remote_route_rig::RouteRig {
 protected:
  void SetUp() override { secrets.local_sid = 1; }

  auto establish(HeardRoutes* routes = nullptr) {
    return InternetStart::establish_control_route(
        routed, session, push, *started, secrets, 1, 2, {}, 20000, false,
        libchiaking::psn::rest::ConsoleFamily::ps5, std::nullopt,
        libchiaking::net::nat::holepunch::default_nat_probe_socket_count,
        routes);
  }
};

TEST_F(RemoteControlRoute, FailsAtTheOfferWhenThePushConnectionFails) {
  ASSERT_NO_FATAL_FAILURE(open());
  HeardRoutes routes;
  auto failure = establish(&routes);
  ASSERT_FALSE(failure);
  EXPECT_TRUE(failure.error().is<internet::holepunch::OfferPsnError>());
  ASSERT_EQ(routes.heard.size(), 1U);
  const auto& heard = routes.heard.front();
  EXPECT_EQ(heard.step, Route::Step::exchanging_offers);
  EXPECT_EQ(heard.purpose, Route::Purpose::control);
  EXPECT_FALSE(heard.chosen);
  ASSERT_TRUE(heard.candidates);
  EXPECT_GE(heard.candidates->total(), 1U);
  EXPECT_GE(heard.candidates->local, 1U);
  EXPECT_EQ(sockets().creates(), 1U);
  EXPECT_EQ(sockets().closes(), 1U);
  EXPECT_EQ(connection().receive_calls(), 1U);
  EXPECT_FALSE(started->opened.lease().release_attempted());
  EXPECT_TRUE(started->opened.lease().release({}));
}

TEST_F(RemoteControlRoute, FailsAtTheProbeWhenTheConsoleOffersNoCandidate) {
  ASSERT_NO_FATAL_FAILURE(open({Messages::offer, Messages::result}));
  HeardRoutes routes;
  auto failure = establish(&routes);
  ASSERT_FALSE(failure);
  EXPECT_TRUE(failure.error().is<internet::holepunch::OfferProbeError>());
  ASSERT_EQ(routes.heard.size(), 2U);
  EXPECT_EQ(routes.heard[0].step, Route::Step::exchanging_offers);
  EXPECT_EQ(routes.heard[1].step, Route::Step::testing_candidates);
  EXPECT_EQ(routes.heard[1].purpose, Route::Purpose::control);
  EXPECT_TRUE(routes.heard[1].candidates);
  EXPECT_FALSE(routes.heard[1].chosen);
  EXPECT_EQ(connection().receive_calls(), 2U);
  EXPECT_EQ(http.psn_bodies.size(), 2U);
  EXPECT_TRUE(body_has(0, "RESULT"));
  EXPECT_TRUE(body_has(1, "OFFER"));
  EXPECT_EQ(sockets().creates(), 1U);
  EXPECT_EQ(sockets().closes(), 1U);
  EXPECT_FALSE(started->opened.lease().release_attempted());
  EXPECT_TRUE(started->opened.lease().release({}));
}

TEST_F(RemoteControlRoute, FailsAtTheAcceptWhenTheConsoleNeverAccepts) {
  const auto offer = Messages::offer_with_candidate();
  ASSERT_NO_FATAL_FAILURE(open({offer, Messages::result}));
  auto failure = establish();
  ASSERT_FALSE(failure);
  EXPECT_TRUE(
      failure.error().is<internet::holepunch::SelectedAcceptExchangeError>());
  EXPECT_EQ(connection().receive_calls(), 3U);
  EXPECT_GE(sockets().probe_sends(), 1U);
  EXPECT_EQ(sockets().creates(), 1U);
  EXPECT_EQ(sockets().closes(), 1U);
  EXPECT_EQ(http.psn_bodies.size(), 3U);
  EXPECT_FALSE(started->opened.lease().release_attempted());
  EXPECT_TRUE(started->opened.lease().release({}));
}

TEST_F(RemoteControlRoute, HandsOverTheProbedSocketOnceTheConsoleAccepts) {
  const auto offer = Messages::offer_with_candidate();
  ASSERT_NO_FATAL_FAILURE(open({offer, Messages::result, Messages::accept}));
  auto established = establish();
  ASSERT_TRUE(established);
  ASSERT_TRUE(established.value().transport);
  const platform::SocketEndpoint local{platform::IpAddress::loopback_ipv4(),
                                       40000};
  EXPECT_EQ(established.value().local_endpoint, local);
  auto endpoint = established.value().transport->local_endpoint();
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint.value(), established.value().local_endpoint);
  EXPECT_EQ(connection().receive_calls(), 3U);
  EXPECT_GE(sockets().probe_sends(), 1U);
  EXPECT_EQ(sockets().creates(), 1U);
  EXPECT_EQ(sockets().closes(), 0U);
  EXPECT_EQ(http.psn_bodies.size(), 4U);
  EXPECT_TRUE(body_has(0, "RESULT"));
  EXPECT_TRUE(body_has(1, "OFFER"));
  EXPECT_TRUE(body_has(1, R"(\"peerSid\":0,)"));
  EXPECT_EQ(established.value().console_sid, 3U);
  EXPECT_TRUE(body_has(2, "ACCEPT"));
  EXPECT_TRUE(body_has(3, "RESULT"));
  established.value().transport.reset();
  EXPECT_EQ(sockets().closes(), 1U);
  EXPECT_FALSE(started->opened.lease().release_attempted());
  EXPECT_TRUE(started->opened.lease().release({}));
}

}  // namespace
}  // namespace play_test
