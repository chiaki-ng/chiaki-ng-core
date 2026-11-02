#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_data_route_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

namespace holepunch = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::play::RouteFailure;

class RemoteDataRouteHandoff : public data_route_rig::DataRouteRig {
 protected:
  static auto accepted(internet::holepunch::AcceptWait& wait,
                       std::uint64_t attempt, std::uint16_t req_id) -> bool {
    holepunch::SessionMessage accept{};
    accept.action = holepunch::Action::accept;
    accept.req_id = req_id;
    if (!wait.begin(attempt)) return false;
    wait.on_message(attempt, accept);
    return true;
  }

  static void make_ready(internet::holepunch::AcceptWait& wait) {
    ASSERT_TRUE(accepted(wait, 1, 7));
    ASSERT_TRUE(wait.take_ack().has_value());
    ASSERT_TRUE(wait.ack_sent(1));
  }

  const platform::SocketEndpoint peer{platform::IpAddress::loopback_ipv4(),
                                      9295};
};

TEST_F(RemoteDataRouteHandoff, WaitsUntilAnAcceptArrives) {
  internet::holepunch::AcceptWait idle;
  auto not_ready = InternetStart::hand_off_data_route(
      network, selected(network, peer), idle);
  ASSERT_FALSE(not_ready);
  EXPECT_TRUE(not_ready.error().is<RouteFailure::AcceptNotReady>());
}

TEST_F(RemoteDataRouteHandoff, WaitsUntilTheAcceptIsAcknowledged) {
  internet::holepunch::AcceptWait pending;
  ASSERT_TRUE(accepted(pending, 2, 3));
  auto before_ack = InternetStart::hand_off_data_route(
      network, selected(network, peer), pending);
  ASSERT_FALSE(before_ack);
  EXPECT_TRUE(before_ack.error().is<RouteFailure::AcceptNotReady>());
}

TEST_F(RemoteDataRouteHandoff, RefusesAnUnspecifiedPeer) {
  internet::holepunch::AcceptWait ready;
  ASSERT_NO_FATAL_FAILURE(make_ready(ready));
  auto invalid = InternetStart::hand_off_data_route(
      network, selected(network, {platform::IpAddress::any_ipv4(), 9295}),
      ready);
  ASSERT_FALSE(invalid);
  const auto* adoption = invalid.error().as<RouteFailure::Adoption>();
  ASSERT_NE(adoption, nullptr);
  EXPECT_EQ(*adoption, RouteFailure::Adoption::invalid_peer);
}

TEST_F(RemoteDataRouteHandoff, HandsTheSocketOverOnceTheAcceptIsAcknowledged) {
  internet::holepunch::AcceptWait ready;
  ASSERT_NO_FATAL_FAILURE(make_ready(ready));
  const auto receiver = receiver_endpoint();
  ASSERT_TRUE(receiver);
  auto route = InternetStart::hand_off_data_route(
      network, selected(network, *receiver), ready);
  ASSERT_TRUE(route.has_value());
  EXPECT_TRUE(route.value().transport);
  EXPECT_EQ(route.value().kind, remote_play::RouteKind::remote_rudp);
  EXPECT_EQ(route.value().remote_endpoint, *receiver);
}

}  // namespace
}  // namespace play_test
