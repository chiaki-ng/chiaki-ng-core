#include "tests/unit/core/net/protocol/remote_play/stream/controller_delivery_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

class ControllerDeliveryHistory : public ControllerDeliveryRig {
 protected:
  static auto legacy() -> Policy {
    Policy policy{};
    policy.recent_history_events = 4;
    policy.history_repeats = 0;
    policy.per_edge_flush = false;
    return policy;
  }

  static auto chord() -> State {
    State value;
    value.touches[0] = {0, 100, 200};
    value.buttons = buttons({Button::cross, Button::box});
    value.l2 = 255;
    return value;
  }

  void edges_survive_a_full_mailbox_in_order(Policy policy) {
    Sink sink;
    Delivery delivery{sink, 0, policy};
    State state;
    state.buttons = buttons({Button::cross, Button::moon});
    ASSERT_TRUE(delivery.submit(state, 0));
    const auto sent_before = sink.histories.size();
    sink.full_history = 100;
    state.buttons = 0;
    ASSERT_TRUE(delivery.submit(state, 1));
    state.buttons = buttons({Button::cross});
    ASSERT_TRUE(delivery.submit(state, 2));
    EXPECT_EQ(sink.histories.size(), sent_before);
    EXPECT_FALSE(delivery.refresh(3));
    sink.full_history = 0;
    EXPECT_TRUE(delivery.refresh(4));
    std::vector<std::pair<DigitalButton, bool>> newest;
    for (std::size_t index = sent_before; index < sink.histories.size();
         ++index) {
      const auto* digital = event_at<Digital>(sink.histories[index], 0);
      ASSERT_NE(digital, nullptr) << "history " << index;
      newest.emplace_back(digital->button, digital->pressed);
    }
    if (policy.per_edge_flush) {
      EXPECT_EQ(newest, (std::vector<std::pair<DigitalButton, bool>>{
                            {DigitalButton::cross, false},
                            {DigitalButton::moon, false},
                            {DigitalButton::cross, true}}));
    } else {
      ASSERT_EQ(newest.size(), 2U);
      EXPECT_EQ(newest[1].first, DigitalButton::cross);
      EXPECT_TRUE(newest[1].second);
    }
    std::size_t releases = 0;
    for (const auto& event : sink.histories.back())
      if (const auto* digital = std::get_if<Digital>(&event);
          digital && !digital->pressed)
        ++releases;
    EXPECT_EQ(releases, 2U);
    EXPECT_EQ(sink.calls.back().kind, Kind::history);
  }
};

TEST_F(ControllerDeliveryHistory, KeepsTheNewestEventsUnderTheLegacyPolicy) {
  Sink sink;
  Delivery desktop{sink, 0, legacy()};
  State toggle;
  for (int i = 0; i < 6; ++i) {
    toggle.buttons = (i % 2 == 0) ? 1U : 0U;
    EXPECT_TRUE(desktop.submit(toggle, static_cast<std::uint64_t>(i)));
    const auto history = std::find_if(
        sink.calls.rbegin(), sink.calls.rend(),
        [](const auto& call) { return call.kind == Kind::history; });
    ASSERT_NE(history, sink.calls.rend()) << "sample " << i;
    EXPECT_EQ(history->events, static_cast<std::size_t>(std::min(i + 1, 5)))
        << "sample " << i;
  }
  ASSERT_EQ(sink.last_history.size(), 5U);
  const auto* oldest = event_at<Digital>(sink.last_history, 0);
  ASSERT_NE(oldest, nullptr);
  EXPECT_FALSE(oldest->pressed);
  const auto desktop_calls = sink.calls.size();
  EXPECT_TRUE(desktop.refresh(40'000));
  EXPECT_EQ(sink.calls.size(), desktop_calls);
}

TEST_F(ControllerDeliveryHistory, RepeatsAFullWindowUnderTheSwitchPolicy) {
  Sink sink;
  Delivery sw{sink, 0, Policy::switch_console()};
  State switch_state;
  for (int i = 0; i < 17; ++i) {
    switch_state.buttons = (i % 2 == 0) ? 1U : 0U;
    EXPECT_TRUE(sw.submit(switch_state, static_cast<std::uint64_t>(i)))
        << "sample " << i;
  }
  ASSERT_FALSE(sink.calls.empty());
  EXPECT_TRUE(sink.calls.back().kind == Kind::history ||
              sink.calls.back().kind == Kind::state);
  EXPECT_EQ(sink.last_history.size(), 16U);
  const auto before_repeat = sink.calls.size();
  EXPECT_TRUE(sw.refresh(20'015));
  EXPECT_EQ(sink.calls.size(), before_repeat);
  EXPECT_TRUE(sw.refresh(20'016));
  EXPECT_EQ(sink.calls.size(), before_repeat + 1);
  EXPECT_EQ(sink.calls.back().events, 16U);
  EXPECT_TRUE(sw.refresh(20'016));
  EXPECT_EQ(sink.calls.size(), before_repeat + 1);
  sink.fail_history = true;
  EXPECT_FALSE(sw.refresh(40'016));
  EXPECT_EQ(sink.calls.size(), before_repeat + 1);
  EXPECT_TRUE(sw.refresh(40'016));
  EXPECT_EQ(sink.calls.size(), before_repeat + 2);
  EXPECT_TRUE(sw.refresh(60'016));
  EXPECT_EQ(sink.calls.size(), before_repeat + 2);
}

TEST_F(ControllerDeliveryHistory, FlushesEachEdgeOfAChordUnderTheSwitchPolicy) {
  Sink sink;
  Delivery edges{sink, 0, Policy::switch_console()};
  EXPECT_TRUE(edges.submit(chord(), 0));
  ASSERT_EQ(sink.histories.size(), 4U);
  for (std::size_t index = 0; index < 4; ++index)
    EXPECT_EQ(sink.histories[index].size(), index + 1);
  const auto& newest = sink.histories.back();
  ASSERT_EQ(newest.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<Analog>(newest[0]));
  const auto* box = event_at<Digital>(newest, 1);
  const auto* cross = event_at<Digital>(newest, 2);
  ASSERT_NE(box, nullptr);
  ASSERT_NE(cross, nullptr);
  EXPECT_EQ(box->button, DigitalButton::box);
  EXPECT_EQ(cross->button, DigitalButton::cross);
  EXPECT_TRUE(std::holds_alternative<Touch>(newest[3]));
}

TEST_F(ControllerDeliveryHistory, ResendsEdgesWhoseFlushFailed) {
  Sink sink;
  Delivery partial{sink, 0, Policy::switch_console()};
  State pair;
  pair.buttons = buttons({Button::cross, Button::moon});
  EXPECT_TRUE(partial.submit(pair, 0));
  EXPECT_EQ(sink.histories.size(), 2U);
  pair.buttons = 0;
  sink.fail_history = true;
  EXPECT_TRUE(partial.submit(pair, 1));
  EXPECT_EQ(sink.histories.size(), 2U);
  EXPECT_TRUE(partial.submit(pair, 2));
  ASSERT_EQ(sink.histories.size(), 4U);
  EXPECT_EQ(sink.histories[3].size(), 4U);
  const auto* released = event_at<Digital>(sink.histories[3], 0);
  ASSERT_NE(released, nullptr);
  EXPECT_FALSE(released->pressed);
}

TEST_F(ControllerDeliveryHistory, CoalescesAChordUnderTheLegacyPolicy) {
  Sink sink;
  Delivery coalesced{sink, 0, legacy()};
  EXPECT_TRUE(coalesced.submit(chord(), 0));
  ASSERT_EQ(sink.histories.size(), 1U);
  EXPECT_EQ(sink.histories[0].size(), 4U);
}

TEST_F(ControllerDeliveryHistory, KeepsEdgesInOrderThroughAFullMailboxPerEdge) {
  ASSERT_NO_FATAL_FAILURE(
      edges_survive_a_full_mailbox_in_order(Policy::switch_console()));
}

TEST_F(ControllerDeliveryHistory, KeepsEdgesInOrderThroughAFullMailbox) {
  ASSERT_NO_FATAL_FAILURE(edges_survive_a_full_mailbox_in_order(Policy{}));
}

TEST_F(ControllerDeliveryHistory, ResendsKeepTheirNumberAndShedEventsCarryOn) {
  Sink sink;
  Delivery delivery{sink, 0};
  const auto cross = buttons({Button::cross});
  State state;
  state.buttons = cross;
  ASSERT_TRUE(delivery.submit(state, 0));
  ASSERT_TRUE(delivery.refresh(libchiaking::net::protocol::remote_play::
                                   default_history_repeat_interval_us));
  EXPECT_EQ(sink.freshes, (std::vector<std::size_t>{1, 0}));

  sink.full_history = 1'000;
  sink.freshes.clear();
  constexpr std::size_t capacity =
      libchiaking::net::protocol::remote_play::queue_capacity;
  constexpr std::size_t toggles = capacity + 5;
  for (std::size_t index = 0; index < toggles; ++index) {
    state.buttons ^= cross;
    ASSERT_TRUE(delivery.submit(state, 1 + index));
  }
  sink.full_history = 0;
  ASSERT_TRUE(delivery.refresh(1 + toggles));
  ASSERT_EQ(sink.freshes.size(), capacity);
  EXPECT_EQ(sink.freshes.front(), 6U);
  EXPECT_TRUE(std::all_of(sink.freshes.begin() + 1, sink.freshes.end(),
                          [](std::size_t fresh) { return fresh == 1; }));
}

TEST_F(ControllerDeliveryHistory,
       AnEventPushedAtOnceSharesTheNumberingAndResends) {
  Sink sink;
  Delivery delivery{sink, 0};
  State state;
  state.buttons = buttons({Button::cross});
  ASSERT_TRUE(delivery.submit(state, 0));
  ASSERT_TRUE(delivery.push_event(0, Presence{true}, 1));
  ASSERT_EQ(sink.histories.size(), 2U);
  ASSERT_EQ(sink.histories.back().size(), 2U);
  EXPECT_NE(event_at<Presence>(sink.histories.back(), 0), nullptr);
  EXPECT_NE(event_at<Digital>(sink.histories.back(), 1), nullptr);
  ASSERT_TRUE(delivery.refresh(1 + libchiaking::net::protocol::remote_play::
                                       default_history_repeat_interval_us));
  EXPECT_EQ(sink.freshes, (std::vector<std::size_t>{1, 1, 0}));
  ASSERT_EQ(sink.histories.back().size(), 2U);
  EXPECT_NE(event_at<Presence>(sink.histories.back(), 0), nullptr);
  EXPECT_NE(event_at<Digital>(sink.histories.back(), 1), nullptr);
  EXPECT_FALSE(delivery.push_event(4, Presence{true}, 2));
}

TEST_F(ControllerDeliveryHistory, ReportsWindowsResendsShedsAndSeats) {
  Sink sink;
  Delivery delivery{sink, 0};
  const auto cross = buttons({Button::cross});
  State state;
  state.buttons = cross;
  ASSERT_TRUE(delivery.submit(state, 0));
  libchiaking::net::protocol::remote_play::StreamInputDiagnostics input;
  delivery.report(input);
  EXPECT_EQ(input.windows_queued, 1U);
  EXPECT_EQ(input.history_events, 1U);
  EXPECT_EQ(input.windows_waiting, 0U);
  EXPECT_EQ(input.held_buttons[0], cross);
  EXPECT_EQ(input.active_seats, 0b1);
  ASSERT_TRUE(delivery.refresh(libchiaking::net::protocol::remote_play::
                                   default_history_repeat_interval_us));
  delivery.report(input);
  EXPECT_EQ(input.resends, 1U);

  State other;
  other.controller_id = 2;
  other.buttons = cross;
  ASSERT_TRUE(delivery.submit(other, 1));
  delivery.report(input);
  EXPECT_EQ(input.ignored_samples, 1U);
  ASSERT_TRUE(delivery.activate(2, other, 2));
  delivery.report(input);
  EXPECT_EQ(input.active_seats, 0b101);
  EXPECT_EQ(input.latched_buttons[2], cross);

  sink.full_history = 1'000;
  const auto queued = input.windows_queued;
  constexpr std::size_t capacity =
      libchiaking::net::protocol::remote_play::queue_capacity;
  for (std::size_t index = 0; index < capacity + 5; ++index) {
    state.buttons ^= cross;
    ASSERT_TRUE(delivery.submit(state, 3 + index));
  }
  delivery.report(input);
  EXPECT_EQ(input.windows_queued, queued + capacity + 5);
  EXPECT_EQ(input.windows_waiting, capacity);
  EXPECT_EQ(input.windows_shed, 5U);
}

}  // namespace
}  // namespace remote_play_test
