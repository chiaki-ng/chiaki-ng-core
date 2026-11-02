#include "src/core/net/protocol/takion/detail/feedback_record_codec.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
namespace feedback_event = remote_play::feedback_event;
using remote_play::ActiveSessionError;
using Led = std::array<std::uint8_t, 3>;

class ActiveSessionFeedback : public ::testing::Test {
 protected:
  auto session() -> remote_play::ActiveSession& { return *rig.owner; }

  auto take_all() -> std::vector<remote_play::ActiveFeedbackEvent> {
    std::vector<remote_play::ActiveFeedbackEvent> events;
    while (auto event = session().take_feedback_event())
      events.push_back(std::move(*event));
    return events;
  }

  auto confirmed() -> std::vector<feedback_event::PadConfirmed> {
    std::vector<feedback_event::PadConfirmed> result;
    for (const auto& event : take_all())
      if (const auto* pad = std::get_if<feedback_event::PadConfirmed>(&event))
        result.push_back(*pad);
    return result;
  }

  auto pad_row(std::uint8_t index, std::uint8_t red)
      -> std::vector<feedback_event::PadConfirmed> {
    rig.enqueue_data(
        9, {std::byte{index}, std::byte{red}, std::byte{2}, std::byte{3},
            std::byte{0}, std::byte{0}, std::byte{0}});
    EXPECT_TRUE(session().pump_once());
    return confirmed();
  }

  void rumble(std::uint8_t pad) {
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(
        7, {std::byte{pad}, std::byte{0x40}, std::byte{0x20}}));
    ASSERT_TRUE(session().pump_once());
  }

  void trigger(std::uint8_t pad) {
    std::vector<std::byte> payload(25);
    payload[0] = std::byte{pad};
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(11, std::move(payload)));
    ASSERT_TRUE(session().pump_once());
  }

  auto rumble_and_trigger_pads()
      -> std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> rumbles;
    std::vector<std::uint8_t> triggers;
    for (const auto& event : take_all()) {
      if (const auto* value = std::get_if<takion::client_event::Rumble>(&event))
        rumbles.push_back(value->player_index);
      if (const auto* value =
              std::get_if<takion::client_event::TriggerEffects>(&event))
        triggers.push_back(value->player_index);
    }
    return {rumbles, triggers};
  }

  ActiveSessionRig rig{9};
};

TEST_F(ActiveSessionFeedback, IsDeliveredBeforeTheSessionIsReady) {
  std::vector<std::byte> trigger_payload(25);
  trigger_payload[1] = std::byte{0x26};
  std::vector<std::byte> ungated_trigger(25);
  ungated_trigger[0] = std::byte{1};
  ASSERT_NO_FATAL_FAILURE(
      rig.make_ready({}, H264Bits::header(),
                     {{7, {std::byte{0x02}, std::byte{0x01}, std::byte{0x01}}},
                      {7, {std::byte{0x00}, std::byte{0x40}, std::byte{0x80}}},
                      {11, ungated_trigger},
                      {11, trigger_payload}}));
  ASSERT_EQ(rig.early_feedback.size(), 2U);
  const auto* early_rumble =
      std::get_if<takion::client_event::Rumble>(&rig.early_feedback[0]);
  ASSERT_NE(early_rumble, nullptr);
  EXPECT_EQ(early_rumble->player_index, 0U);
  EXPECT_EQ(early_rumble->left, 0x40U);
  EXPECT_EQ(early_rumble->right, 0x80U);
  const auto* effects =
      std::get_if<takion::client_event::TriggerEffects>(&rig.early_feedback[1]);
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 0U);
  EXPECT_EQ(effects->type_left, 0x26U);
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_data(7, {std::byte{0x00}, std::byte{1}, std::byte{2}}));
  ASSERT_TRUE(session().pump_once());
  const auto feedback = session().take_feedback_event();
  ASSERT_TRUE(feedback);
  EXPECT_EQ(std::get<takion::client_event::Rumble>(*feedback).left, 1U);
  EXPECT_TRUE(session().request_stop());
  EXPECT_FALSE(session().take_feedback_event());
}

TEST_F(ActiveSessionFeedback, ReceivedBeforeAFailureIsKept) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_data(7, {std::byte{0x00}, std::byte{0x10}, std::byte{0x20}}));
  rig.state->fail_when_drained = true;
  const auto failed = session().pump_once();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ActiveSessionError::pump_failed);
  const auto feedback = session().take_feedback_event();
  ASSERT_TRUE(feedback);
  const auto* kept = std::get_if<takion::client_event::Rumble>(&*feedback);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->left, 0x10U);
  EXPECT_EQ(kept->right, 0x20U);
  EXPECT_FALSE(session().take_feedback_event());
}

TEST_F(ActiveSessionFeedback, PadInfoBecomesFeedbackEvents) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  std::vector<std::byte> row(0x19);
  row[9] = std::byte{0x10};
  row[10] = std::byte{0x20};
  row[11] = std::byte{0x30};
  row[12] = std::byte{1};
  row[13] = std::byte{1};
  row[14] = std::byte{1};
  row[15] = std::byte{1};
  row[20] = std::byte{1};
  row[21] = std::byte{3};
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(9, row));
  ASSERT_TRUE(session().pump_once());
  const auto events = take_all();
  ASSERT_EQ(events.size(), 5U);
  EXPECT_TRUE(std::holds_alternative<feedback_event::MotionReset>(events[0]));
  ASSERT_TRUE(
      std::holds_alternative<feedback_event::MotionSettings>(events[1]));
  EXPECT_EQ(std::get<feedback_event::MotionSettings>(events[1]).values,
            (std::array<std::uint8_t, 2>{1, 1}));
  ASSERT_TRUE(
      std::holds_alternative<feedback_event::TriggerIntensity>(events[2]));
  EXPECT_EQ(std::get<feedback_event::TriggerIntensity>(events[2]).value, 3U);
  ASSERT_TRUE(std::holds_alternative<feedback_event::LedColor>(events[3]));
  EXPECT_EQ(std::get<feedback_event::LedColor>(events[3]).rgb,
            (Led{0x10, 0x20, 0x30}));
  ASSERT_TRUE(std::holds_alternative<feedback_event::PadConfirmed>(events[4]));
  const auto& pad = std::get<feedback_event::PadConfirmed>(events[4]);
  EXPECT_EQ(pad.index, 0U);
  EXPECT_EQ(pad.led[2], 0x30U);
  row[12] = std::byte{0};
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(9, row));
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().take_feedback_event());
  row[13] = std::byte{0};
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(9, row));
  ASSERT_TRUE(session().pump_once());
  const auto changed = session().take_feedback_event();
  ASSERT_TRUE(changed);
  ASSERT_TRUE(std::holds_alternative<feedback_event::MotionSettings>(*changed));
  EXPECT_EQ(std::get<feedback_event::MotionSettings>(*changed).values,
            (std::array<std::uint8_t, 2>{0, 1}));
  EXPECT_FALSE(session().take_feedback_event());
  std::vector<std::byte> compact(0x11);
  compact[0] = std::byte{1};
  compact[1] = std::byte{9};
  compact[7] = std::byte{1};
  compact[12] = std::byte{2};
  compact[13] = std::byte{3};
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_data(9, compact));
  ASSERT_TRUE(session().pump_once());
  auto event = session().take_feedback_event();
  ASSERT_TRUE(event);
  ASSERT_TRUE(std::holds_alternative<feedback_event::HapticIntensity>(*event));
  EXPECT_EQ(std::get<feedback_event::HapticIntensity>(*event).value, 2U);
  EXPECT_FALSE(session().take_feedback_event());
  session().admit_pad(1, true);
  event = session().take_feedback_event();
  ASSERT_TRUE(event);
  ASSERT_TRUE(std::holds_alternative<feedback_event::PadConfirmed>(*event));
  EXPECT_EQ(std::get<feedback_event::PadConfirmed>(*event).index, 1U);
  EXPECT_EQ(std::get<feedback_event::PadConfirmed>(*event).led[0], 9U);
  EXPECT_FALSE(session().take_feedback_event());
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionFeedback, PadInfoIsHeldUntilThePadIsAdmitted) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(ActiveSessionRig::console_units()));
  EXPECT_EQ(pad_row(0, 9).size(), 1U);
  EXPECT_TRUE(pad_row(2, 9).empty());
  EXPECT_TRUE(pad_row(2, 8).empty());
  EXPECT_EQ(session().pad_led(2), Led{});
  session().admit_pad(2, true);
  const auto flushed = confirmed();
  ASSERT_EQ(flushed.size(), 1U);
  EXPECT_EQ(flushed[0].index, 2U);
  EXPECT_EQ(flushed[0].led[0], 8U);
  EXPECT_EQ(session().couch_pad_count(), 3U);
  session().admit_pad(2, true);
  EXPECT_TRUE(confirmed().empty());
  EXPECT_TRUE(pad_row(2, 8).empty());
  const auto changed = pad_row(2, 7);
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0].index, 2U);
  EXPECT_EQ(changed[0].led[0], 7U);
  EXPECT_EQ(pad_row(1, 5).size(), 1U);
  session().release_pad(2);
  EXPECT_EQ(session().couch_pad_count(), 3U);
  EXPECT_EQ(session().pad_led(2), Led{});
  EXPECT_EQ(pad_row(2, 7).size(), 1U);
  EXPECT_TRUE(pad_row(3, 4).empty());
  session().admit_pad(3, false);
  EXPECT_TRUE(confirmed().empty());
  EXPECT_EQ(session().pad_led(3), (Led{4, 2, 3}));
  EXPECT_TRUE(pad_row(3, 4).empty());
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionFeedback, RumbleAndTriggersFollowThePadGate) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(rumble(0));
  ASSERT_NO_FATAL_FAILURE(rumble(1));
  ASSERT_NO_FATAL_FAILURE(trigger(1));
  ASSERT_NO_FATAL_FAILURE(trigger(0));
  const auto [first_rumble, first_triggers] = rumble_and_trigger_pads();
  EXPECT_EQ(first_rumble, std::vector<std::uint8_t>{0});
  EXPECT_EQ(first_triggers, std::vector<std::uint8_t>{0});
  session().admit_pad(1, true);
  ASSERT_NO_FATAL_FAILURE(rumble(1));
  ASSERT_NO_FATAL_FAILURE(trigger(1));
  ASSERT_NO_FATAL_FAILURE(trigger(5));
  const auto [second_rumble, second_triggers] = rumble_and_trigger_pads();
  EXPECT_EQ(second_rumble, std::vector<std::uint8_t>{1});
  EXPECT_EQ(second_triggers, std::vector<std::uint8_t>{1});
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionFeedback, InitialAnnouncementCoversOnlyThePrimaryPad) {
  auto control = ActiveSessionRig::console_units();
  control.pad_count = 3;
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(control));
  const auto connections = rig.controller_connections(0);
  ASSERT_EQ(connections.size(), 1U);
  EXPECT_EQ(connections[0].controller_id, 0);
  EXPECT_EQ(connections[0].connected, true);
  EXPECT_TRUE(connections[0].controller_type ==
              takion::ControllerConnectionTypeValue{
                  takion::ControllerConnectionType::dualsense});
  EXPECT_EQ(session().couch_pad_count(), 1U);
  const auto before = rig.state->sent.size();
  session().set_pad_controller_type(3, 2);
  EXPECT_TRUE(session().announce_pad(3, true));
  EXPECT_TRUE(session().disconnect_pad(2));
  session().admit_pad(2, true);
  session().release_pad(2);
  EXPECT_EQ(session().couch_pad_count(), 3U);
  EXPECT_TRUE(session().announce_pads(1));
  for (int i = 0; i < 4; ++i) {
    rig.acknowledge_all();
    ASSERT_TRUE(session().pump_once()) << i;
  }
  const auto later = rig.controller_connections(before);
  ASSERT_EQ(later.size(), 5U);
  EXPECT_EQ(later[0].controller_id, 3);
  EXPECT_TRUE(later[0].controller_type ==
              takion::ControllerConnectionTypeValue{
                  takion::ControllerConnectionType::dualshock4});
  EXPECT_EQ(later[1].controller_id, 2);
  EXPECT_EQ(later[1].connected, false);
  EXPECT_TRUE(later[1].controller_type ==
              takion::ControllerConnectionTypeValue{
                  takion::ControllerConnectionType::not_set});
  EXPECT_EQ(later[2].controller_id, 0);
  EXPECT_EQ(later[3].controller_id, 1);
  EXPECT_EQ(later[4].controller_id, 2);
  EXPECT_TRUE(session().request_stop());

  ActiveSessionRig single{9};
  ASSERT_NO_FATAL_FAILURE(single.make_ready());
  const auto one = single.controller_connections(0);
  ASSERT_EQ(one.size(), 1U);
  EXPECT_FALSE(one[0].controller_id);
  EXPECT_TRUE(single.owner->request_stop());
}

TEST_F(ActiveSessionFeedback, HistoryWindowStampsEachEventWithItsPad) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto before = rig.state->sent.size();
  EXPECT_TRUE(session().post_controller_history_window(
      {takion::FeedbackPresenceEvent{true},
       takion::FeedbackPresenceEvent{false}},
      {2, 1}, 2));
  const auto input = session().diagnostics().input;
  EXPECT_EQ(input.history_posts, 1U);
  EXPECT_EQ(input.history_sequence, 1U);
  EXPECT_EQ(input.history_refused, 0U);
  EXPECT_EQ(input.history_wraps, 0U);
  const auto mismatched = session().post_controller_history_window(
      {takion::FeedbackPresenceEvent{true}}, {}, 1);
  ASSERT_FALSE(mismatched);
  EXPECT_EQ(mismatched.error(), ActiveSessionError::invalid_controller_state);
  const auto out_of_range = session().post_controller_history_window(
      {takion::FeedbackPresenceEvent{true}}, {4}, 1);
  ASSERT_FALSE(out_of_range);
  EXPECT_EQ(out_of_range.error(), ActiveSessionError::invalid_controller_state);
  rig.acknowledge_all();
  ASSERT_TRUE(session().pump_once());
  const auto resend_start = rig.state->sent.size();
  EXPECT_TRUE(session().post_controller_history_window(
      {takion::FeedbackPresenceEvent{true},
       takion::FeedbackPresenceEvent{false}},
      {2, 1}, 0));
  EXPECT_TRUE(session().post_controller_history_window(
      {takion::FeedbackPresenceEvent{true}}, {0}, 3));
  rig.acknowledge_all();
  ASSERT_TRUE(session().pump_once());
  const auto later = rig.feedback_history_payloads(resend_start);
  ASSERT_EQ(later.size(), 2U);
  EXPECT_EQ(later[0][0], std::byte{0});
  EXPECT_EQ(later[0][1], std::byte{1});
  EXPECT_EQ(later[0][2], std::byte{1});
  EXPECT_EQ(later[1][0], std::byte{0});
  EXPECT_EQ(later[1][1], std::byte{4});
  EXPECT_EQ(later[1][2], std::byte{0});
  EXPECT_EQ(session().diagnostics().input.history_sequence, 4U);
  const auto payloads = rig.feedback_history_payloads(before);
  ASSERT_EQ(payloads.size(), 3U);
  ASSERT_EQ(payloads[0].size(), 15U);
  EXPECT_EQ(payloads[0][0], std::byte{0});
  EXPECT_EQ(payloads[0][1], std::byte{1});
  EXPECT_EQ(payloads[0][2], std::byte{1});
  EXPECT_EQ(payloads[0][11], std::byte{0x82});
  EXPECT_EQ(payloads[0][12], std::byte{0xbf});
  EXPECT_EQ(payloads[0][13], std::byte{0x81});
  EXPECT_EQ(payloads[0][14], std::byte{0x9f});
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionFeedback, KeyboardStatesGoOutAsKindZeroHistory) {
  const std::array<std::uint16_t, 2> keys{0x04, 0x105};
  const auto record =
      remote_play::ActiveSession::keyboard_record(0x02, 0x02, keys);
  ASSERT_TRUE(record);
  const std::array<std::byte, 8> expected{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x02}, std::byte{0x42},
      std::byte{0x04}, std::byte{0x00}, std::byte{0x05}, std::byte{0x01}};
  for (const std::uint32_t version : {14U, 20U, 25U}) {
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile) << version;
    const auto wire = takion::FeedbackRecordCodec::encode(
        takion::FeedbackHistoryEvent{*record}, 0, *profile);
    ASSERT_TRUE(wire) << version;
    EXPECT_TRUE(std::ranges::equal(wire.value(), expected)) << version;
  }
  const auto legacy = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(takion::FeedbackRecordCodec::encode(
      takion::FeedbackHistoryEvent{*record}, 0, *legacy));
  const std::array<std::uint16_t, 17> crowded{};
  EXPECT_FALSE(remote_play::ActiveSession::keyboard_record(0, 0, crowded));
  EXPECT_FALSE(remote_play::ActiveSession::keyboard_record(0, 8, {}));

  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto start = rig.state->sent.size();
  const auto refused = session().keyboard_event(0x02, 0x02, keys);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), ActiveSessionError::unsupported_request);
  EXPECT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  EXPECT_TRUE(rig.feedback_history_payloads(start).empty());
  EXPECT_TRUE(session().request_stop());
}

}  // namespace
}  // namespace remote_play_test
