#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace play_test {
namespace {

using Order = libchiaking::play::InternetStart::RouteOrder;
using State = Order::State;

class HolepunchRouteOrder : public ::testing::Test {
 protected:
  static void open_control(Order& order) {
    ASSERT_TRUE(order.begin(7));
    order.on_start_ready(7);
    order.on_start_ready(7);
    order.on_data_established(7);
    EXPECT_EQ(order.state(), State::control_in_progress);
    auto control = order.take_effects();
    ASSERT_EQ(control.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<Order::StartControlRoute>(control[0]));
    EXPECT_EQ(std::get<Order::StartControlRoute>(control[0]).attempt, 7U);
    EXPECT_TRUE(order.take_effects().empty());
  }

  static void open_data(Order& order) {
    ASSERT_NO_FATAL_FAILURE(open_control(order));
    order.on_control_established(8);
    EXPECT_EQ(order.state(), State::control_in_progress);
    order.on_control_established(7);
    order.on_control_established(7);
    EXPECT_EQ(order.state(), State::data_in_progress);
    auto data = order.take_effects();
    ASSERT_EQ(data.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<Order::StartDataRoute>(data[0]));
    EXPECT_EQ(std::get<Order::StartDataRoute>(data[0]).attempt, 7U);
  }

  static void complete(Order& order) {
    ASSERT_NO_FATAL_FAILURE(open_data(order));
    order.on_data_established(8);
    EXPECT_EQ(order.state(), State::data_in_progress);
    order.on_data_established(7);
    order.on_data_established(7);
    EXPECT_EQ(order.state(), State::complete);
    auto complete = order.take_effects();
    ASSERT_EQ(complete.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<Order::RoutesReady>(complete[0]));
    EXPECT_EQ(std::get<Order::RoutesReady>(complete[0]).attempt, 7U);
  }
};

TEST_F(HolepunchRouteOrder, RefusesToEndAnAttemptBeforeOneBegins) {
  Order order;
  EXPECT_FALSE(order.cancel(0));
  EXPECT_FALSE(order.timeout(0));
  EXPECT_FALSE(order.begin(0));
  EXPECT_EQ(order.state(), State::idle);
}

TEST_F(HolepunchRouteOrder, BeginsOneAttemptAtATime) {
  Order order;
  EXPECT_TRUE(order.begin(7));
  EXPECT_FALSE(order.begin(8));
  EXPECT_EQ(order.state(), State::waiting_start);
}

TEST_F(HolepunchRouteOrder, WaitsForTheStartBeforeOpeningAnyRoute) {
  Order order;
  ASSERT_TRUE(order.begin(7));
  order.on_control_established(7);
  order.on_data_established(7);
  order.on_start_ready(8);
  EXPECT_EQ(order.state(), State::waiting_start);
  EXPECT_TRUE(order.take_effects().empty());
}

TEST_F(HolepunchRouteOrder, OpensTheControlRouteOnceTheStartIsReady) {
  Order order;
  open_control(order);
}

TEST_F(HolepunchRouteOrder, OpensTheDataRouteOnceControlIsEstablished) {
  Order order;
  open_data(order);
}

TEST_F(HolepunchRouteOrder, ReportsTheRoutesReadyOnceDataIsEstablished) {
  Order order;
  complete(order);
}

TEST_F(HolepunchRouteOrder, CannotEndOrBeginAgainOnceComplete) {
  Order order;
  ASSERT_NO_FATAL_FAILURE(complete(order));
  EXPECT_FALSE(order.timeout(7));
  EXPECT_FALSE(order.cancel(7));
  EXPECT_FALSE(order.begin(9));
}

TEST_F(HolepunchRouteOrder, FailsWhenItsOwnAttemptTimesOut) {
  Order timed;
  ASSERT_TRUE(timed.begin(3));
  timed.on_start_ready(3);
  EXPECT_FALSE(timed.timeout(4));
  EXPECT_TRUE(timed.timeout(3));
  EXPECT_EQ(timed.state(), State::failed);
  EXPECT_TRUE(timed.take_effects().empty());
  timed.on_control_established(3);
  EXPECT_EQ(timed.state(), State::failed);
  EXPECT_FALSE(timed.timeout(3));
  EXPECT_FALSE(timed.begin(5));
}

TEST_F(HolepunchRouteOrder, IgnoresTheStartAfterACancel) {
  Order cancelled;
  ASSERT_TRUE(cancelled.begin(4));
  EXPECT_TRUE(cancelled.cancel(4));
  cancelled.on_start_ready(4);
  EXPECT_EQ(cancelled.state(), State::cancelled);
  EXPECT_TRUE(cancelled.take_effects().empty());
}

}  // namespace
}  // namespace play_test
