#include "tests/unit/core/net/protocol/remote_play/rudp_switch_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace datagram = libchiaking::net::transport::datagram;
namespace tsd = remote_play::transport_switch_detail;
namespace ctrl = remote_play::ctrl_lifecycle_detail;

class RemoteDataRouteGateTest : public RudpSwitchRig {
 protected:
  std::shared_ptr<RudpSwitchRouteState> state =
      std::make_shared<RudpSwitchRouteState>();
  remote_play::RemoteDataRouteGate gate;
};

TEST_F(RemoteDataRouteGateTest, HandsOverTheRouteOnceAfterItsAck) {
  EXPECT_TRUE(gate.arm(route(state), 10));
  EXPECT_FALSE(gate.take_route());
  EXPECT_FALSE(gate.on_ack(ctrl::PublishSwitchAck{9}));
  EXPECT_EQ(gate.state(), tsd::State::waiting);
  EXPECT_TRUE(gate.on_ack(ctrl::PublishSwitchAck{10}));
  EXPECT_FALSE(gate.on_ack(ctrl::PublishSwitchAck{10}));
  EXPECT_FALSE(gate.timeout(10));
  auto taken = gate.take_route();
  ASSERT_TRUE(taken.has_value());
  EXPECT_TRUE(taken->transport);
  EXPECT_EQ(taken->kind, remote_play::RouteKind::remote_rudp);
  EXPECT_EQ(taken->calibration.takion_version, 7U);
  EXPECT_FALSE(gate.take_route());
  EXPECT_FALSE(gate.on_ack(ctrl::PublishSwitchAck{10}));
  EXPECT_EQ(state->close_count, 0U);
}

TEST_F(RemoteDataRouteGateTest, ClosesARouteItCancelsAndArmsAgain) {
  EXPECT_TRUE(gate.arm(route(state), 20));
  EXPECT_FALSE(gate.timeout(19));
  EXPECT_FALSE(gate.cancel(21));
  EXPECT_TRUE(gate.has_route());
  EXPECT_FALSE(gate.arm(route(std::make_shared<RudpSwitchRouteState>()), 21));
  EXPECT_TRUE(gate.timeout(20));
  EXPECT_EQ(state->close_count, 1U);
  EXPECT_FALSE(gate.on_ack(ctrl::PublishSwitchAck{20}));
  EXPECT_FALSE(gate.arm(route(std::make_shared<RudpSwitchRouteState>()), 20));
  EXPECT_TRUE(gate.arm(route(state), 22));
  EXPECT_TRUE(gate.cancel(22));
  EXPECT_EQ(state->close_count, 2U);
  EXPECT_FALSE(gate.on_ack(ctrl::PublishSwitchAck{22}));
  EXPECT_TRUE(gate.arm(route(state), 23));
  EXPECT_TRUE(gate.on_ack(ctrl::PublishSwitchAck{23}));
  const auto taken = gate.take_route();
  EXPECT_TRUE(taken);
  EXPECT_EQ(state->close_count, 2U);
}

TEST_F(RemoteDataRouteGateTest, RefusesRoutesItCannotSwitchTo) {
  auto no_transport = route(std::make_shared<RudpSwitchRouteState>());
  no_transport.transport.reset();
  EXPECT_FALSE(gate.arm(std::move(no_transport), 1));
  EXPECT_FALSE(gate.arm(route(std::make_shared<RudpSwitchRouteState>(),
                              remote_play::RouteKind::local_direct),
                        2));
  EXPECT_FALSE(gate.arm(route(std::make_shared<RudpSwitchRouteState>()), 0));
  EXPECT_FALSE(gate.has_route());
}

TEST_F(RemoteDataRouteGateTest, KeepsTheLastCloseErrorUntilTheNextArm) {
  state->fail_close = true;
  EXPECT_TRUE(gate.arm(route(state), 30));
  EXPECT_TRUE(gate.timeout(30));
  EXPECT_FALSE(gate.has_route());
  EXPECT_EQ(state->close_count, 1U);
  EXPECT_EQ(gate.close_error(), datagram::TransportError::io_failure);
  EXPECT_FALSE(gate.take_route());

  state->fail_close = false;
  EXPECT_TRUE(gate.arm(route(state), 31));
  EXPECT_FALSE(gate.close_error());
  EXPECT_TRUE(gate.timeout(31));
  EXPECT_EQ(state->close_count, 2U);
  EXPECT_FALSE(gate.close_error());
}

}  // namespace
}  // namespace remote_play_test
