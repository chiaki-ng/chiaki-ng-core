#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_data_route_rig.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::play::RouteFailure;

class RemoteDataRouteAccept : public data_route_rig::AcceptRig {};

TEST_F(RemoteDataRouteAccept, HandsTheSocketOverOnceTheConsoleAccepts) {
  connection.add(accept_json);
  const auto peer = receiver_endpoint();
  ASSERT_TRUE(peer);
  auto route = accept(selected(network, *peer));
  ASSERT_TRUE(route);
  EXPECT_TRUE(route.value().transport);
  EXPECT_EQ(route.value().remote_endpoint, *peer);
  EXPECT_EQ(route.value().kind, remote_play::RouteKind::remote_rudp);
  EXPECT_EQ(http.bodies().size(), 2U);
  EXPECT_EQ(connection.calls(), 1U);
}

TEST_F(RemoteDataRouteAccept, FailsTheWaitWhenNoAcceptArrives) {
  auto route =
      accept(selected(network, {platform::IpAddress::loopback_ipv4(), 9295}));
  ASSERT_FALSE(route);
  const auto* exchange =
      route.error().as<internet::holepunch::SelectedAcceptExchangeError>();
  ASSERT_NE(exchange, nullptr);
  EXPECT_EQ(exchange->kind(),
            internet::holepunch::SelectedAcceptExchangeErrorKind::wait_failed);
  EXPECT_EQ(http.bodies().size(), 1U);
}

TEST_F(RemoteDataRouteAccept, RefusesAnUnspecifiedPeerAfterTheAccept) {
  connection.add(accept_json);
  auto route =
      accept(selected(network, {platform::IpAddress::any_ipv4(), 9295}));
  ASSERT_FALSE(route);
  const auto* adoption = route.error().as<RouteFailure::Adoption>();
  ASSERT_NE(adoption, nullptr);
  EXPECT_EQ(*adoption, RouteFailure::Adoption::invalid_peer);
}

}  // namespace
}  // namespace play_test
