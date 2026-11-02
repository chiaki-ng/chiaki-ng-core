#include "tests/unit/core/net/protocol/remote_play/stream/controller_delivery_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

class ControllerDeliveryPads : public ControllerDeliveryRig {
 protected:
  void SetUp() override { second.controller_id = 1; }

  void refresh_both_pads() {
    EXPECT_TRUE(multiple.is_active(0));
    EXPECT_FALSE(multiple.is_active(1));
    ASSERT_TRUE(multiple.submit(second, 0));
    EXPECT_TRUE(sink.calls.empty());
    EXPECT_FALSE(multiple.push_presence(4, true, 0));
    ASSERT_TRUE(multiple.refresh(0));
    EXPECT_TRUE(sink.calls.empty());
    ASSERT_TRUE(multiple.activate(1, second, 0));
    EXPECT_TRUE(multiple.is_active(1));
    EXPECT_TRUE(sink.calls.empty());
    ASSERT_TRUE(multiple.refresh(199'999));
    EXPECT_TRUE(sink.calls.empty());
    ASSERT_TRUE(multiple.refresh(200'000));
    ASSERT_TRUE(multiple.refresh(200'000));
    ASSERT_EQ(sink.calls.size(), 2U);
    EXPECT_EQ(sink.calls[0].id, 0U);
    EXPECT_TRUE(call_is(sink, 0, Kind::state));
    EXPECT_EQ(sink.calls[1].id, 1U);
    EXPECT_TRUE(call_is(sink, 1, Kind::state));
  }

  void deactivate_the_second_pad() {
    ASSERT_NO_FATAL_FAILURE(refresh_both_pads());
    ASSERT_TRUE(multiple.deactivate(1));
    EXPECT_FALSE(multiple.is_active(1));
    second.buttons = 1;
    ASSERT_TRUE(multiple.submit(second, 300'000));
    ASSERT_TRUE(multiple.refresh(400'000));
    ASSERT_TRUE(multiple.refresh(400'001));
    EXPECT_TRUE(std::none_of(sink.calls.begin() + 2, sink.calls.end(),
                             [](const auto& call) { return call.id == 1; }));
  }

  Sink sink;
  Delivery multiple{sink, 0};
  State second;
};

TEST_F(ControllerDeliveryPads, PostNothingForAPadUntilItIsActivated) {
  ASSERT_NO_FATAL_FAILURE(refresh_both_pads());
}

TEST_F(ControllerDeliveryPads, PostNothingForADeactivatedPad) {
  ASSERT_NO_FATAL_FAILURE(deactivate_the_second_pad());
}

TEST_F(ControllerDeliveryPads, LatchTheButtonsHeldWhenAPadIsReactivated) {
  ASSERT_NO_FATAL_FAILURE(deactivate_the_second_pad());
  const auto before_reopen = sink.calls.size();
  ASSERT_TRUE(multiple.activate(1, second, 500'000));
  EXPECT_EQ(sink.calls.size(), before_reopen);
  ASSERT_TRUE(multiple.submit(second, 500'001));
  EXPECT_EQ(sink.calls.size(), before_reopen);
  second.buttons = 0;
  ASSERT_TRUE(multiple.submit(second, 500'002));
  second.buttons = 1;
  ASSERT_TRUE(multiple.submit(second, 500'003));
  ASSERT_FALSE(sink.calls.empty());
  EXPECT_EQ(sink.calls.back().kind, Kind::history);
  EXPECT_EQ(sink.calls.back().id, 1U);
  EXPECT_EQ(sink.calls.back().events, 1U);
}

TEST_F(ControllerDeliveryPads, PostTheStateOfAPadActivatedWhileMoved) {
  State tilted;
  tilted.controller_id = 2;
  tilted.left_x = 9;
  ASSERT_TRUE(multiple.activate(2, tilted, 0));
  EXPECT_TRUE(sink.calls.empty());
  ASSERT_TRUE(multiple.submit(tilted, 1));
  ASSERT_EQ(sink.calls.size(), 1U);
  EXPECT_TRUE(call_is(sink, 0, Kind::state));
  EXPECT_EQ(sink.calls[0].id, 2U);
  EXPECT_EQ(sink.last_state.left_x, 9);
}

TEST_F(ControllerDeliveryPads, ShareOneHistoryWindow) {
  ASSERT_TRUE(multiple.activate(1, second, 0));
  sink.full_history = 100;
  State first;
  first.buttons = 1;
  second.buttons = 2;
  EXPECT_TRUE(multiple.submit(first, 1));
  EXPECT_TRUE(multiple.submit(second, 1));
  first.buttons = 0;
  second.buttons = 0;
  EXPECT_TRUE(multiple.submit(first, 2));
  EXPECT_TRUE(multiple.submit(second, 2));
  sink.full_history = 0;
  EXPECT_TRUE(multiple.refresh(3));
  EXPECT_EQ(sink.history_ids, (std::vector<std::vector<std::uint8_t>>{
                                  {0}, {1, 0}, {0, 1, 0}, {1, 0, 1, 0}}));
  ASSERT_FALSE(sink.histories.empty());
  const auto& last = sink.histories.back();
  for (std::size_t index = 0; index < 4; ++index) {
    const auto* digital = event_at<Digital>(last, index);
    ASSERT_NE(digital, nullptr) << "event " << index;
    EXPECT_EQ(digital->pressed, index >= 2) << "event " << index;
  }
}

TEST_F(ControllerDeliveryPads, LatchButtonsAlreadyHeldAtActivation) {
  State held;
  held.controller_id = 1;
  held.buttons = buttons({Button::moon, Button::cross});
  held.l2 = 255;
  ASSERT_TRUE(multiple.activate(1, held, 0));
  EXPECT_TRUE(sink.calls.empty());
  EXPECT_TRUE(multiple.submit(held, 1));
  EXPECT_TRUE(sink.histories.empty());
  auto next = held;
  next.buttons = buttons({Button::cross});
  next.l2 = 0;
  EXPECT_TRUE(multiple.submit(next, 2));
  EXPECT_TRUE(sink.histories.empty());
  next.buttons = buttons({Button::cross, Button::moon});
  next.l2 = 9;
  EXPECT_TRUE(multiple.submit(next, 3));
  ASSERT_FALSE(sink.histories.empty());
  for (const auto& ids : sink.history_ids)
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(),
                            [](std::uint8_t id) { return id == 1; }));
  bool moon_pressed = false;
  for (const auto& event : sink.histories.back()) {
    const auto* digital = std::get_if<Digital>(&event);
    EXPECT_TRUE(!digital || digital->button != DigitalButton::cross);
    if (digital && digital->button == DigitalButton::moon && digital->pressed)
      moon_pressed = true;
  }
  EXPECT_TRUE(moon_pressed);
}

TEST_F(ControllerDeliveryPads, KeepTheWindowForTheOtherPadsAfterADeactivation) {
  Sink switch_sink;
  Delivery delivery{switch_sink, 0, Policy::switch_console()};
  ASSERT_TRUE(delivery.activate(1, second, 0));
  EXPECT_TRUE(delivery.push_presence(1, true, 1));
  EXPECT_TRUE(delivery.deactivate(1));
  EXPECT_TRUE(delivery.push_presence(1, false, 2));
  State first;
  first.buttons = 1;
  EXPECT_TRUE(delivery.submit(first, 3));
  ASSERT_FALSE(switch_sink.history_ids.empty());
  EXPECT_EQ(switch_sink.history_ids.back(),
            (std::vector<std::uint8_t>{0, 1, 1}));
  const auto& last = switch_sink.histories.back();
  const auto* gone = event_at<Presence>(last, 1);
  const auto* back = event_at<Presence>(last, 2);
  ASSERT_NE(gone, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_FALSE(gone->present);
  EXPECT_TRUE(back->present);
  EXPECT_FALSE(delivery.push_presence(4, true, 4));
}

}  // namespace
}  // namespace remote_play_test
