#include "tests/unit/core/net/protocol/remote_play/stream/controller_delivery_rig.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace remote_play_test {
namespace {

class ControllerDeliveryPresence : public ControllerDeliveryRig {};

TEST_F(ControllerDeliveryPresence, TravelsInTheHistoryWithTheButtonEdges) {
  Sink sink;
  Delivery presence{sink, 0};
  EXPECT_TRUE(presence.push_presence(0, true, 0));
  ASSERT_EQ(sink.calls.size(), 1U);
  EXPECT_TRUE(call_is(sink, 0, Kind::history));
  EXPECT_EQ(sink.calls[0].events, 1U);
  const auto* arrived = event_at<Presence>(sink.histories[0], 0);
  ASSERT_NE(arrived, nullptr);
  EXPECT_TRUE(arrived->present);
  State moved;
  moved.left_x = 5;
  EXPECT_TRUE(presence.submit(moved, 1));
  EXPECT_EQ(sink.calls.size(), 2U);
  EXPECT_TRUE(call_is(sink, 1, Kind::state));
  EXPECT_TRUE(presence.push_presence(0, false, 2));
  EXPECT_EQ(sink.calls.size(), 3U);
  ASSERT_FALSE(sink.histories.empty());
  EXPECT_EQ(sink.histories.back().size(), 2U);
  EXPECT_TRUE(presence.submit(moved, 3));
  EXPECT_EQ(sink.calls.size(), 3U);
  moved.buttons = 1;
  EXPECT_TRUE(presence.submit(moved, 4));
  EXPECT_EQ(sink.calls.back().kind, Kind::history);
  const auto& combined = sink.histories.back();
  ASSERT_EQ(combined.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<Digital>(combined[0]));
  const auto* left = event_at<Presence>(combined, 1);
  const auto* returned = event_at<Presence>(combined, 2);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(returned, nullptr);
  EXPECT_FALSE(left->present);
  EXPECT_TRUE(returned->present);
}

TEST_F(ControllerDeliveryPresence, IsPostedAloneUnderTheSwitchPolicy) {
  Sink sink;
  Delivery switch_presence{sink, 0, Policy::switch_console()};
  EXPECT_TRUE(switch_presence.push_presence(0, true, 0));
  ASSERT_EQ(sink.calls.size(), 1U);
  EXPECT_TRUE(call_is(sink, 0, Kind::history));
  EXPECT_EQ(sink.calls[0].events, 1U);
}

}  // namespace
}  // namespace remote_play_test
