#include "tests/unit/core/net/protocol/takion/client_inbound_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

class TakionInboundFeedback : public ::testing::Test {
 protected:
  static auto trigger_payload() -> std::vector<std::byte> {
    std::vector<std::byte> payload(25);
    payload[0] = std::byte{0x02};
    payload[1] = std::byte{0xf1};
    payload[2] = std::byte{0xf2};
    for (std::size_t index = 0; index < 10; ++index) {
      payload[5 + index] = static_cast<std::byte>(index + 1);
      payload[15 + index] = static_cast<std::byte>(index + 11);
    }
    return payload;
  }

  static auto prefixed(const std::vector<std::byte>& body)
      -> std::vector<std::byte> {
    std::vector<std::byte> message(8, std::byte{0xee});
    message.insert(message.end(), body.begin(), body.end());
    return message;
  }

  static constexpr std::byte rumble_type{0x07};
  static constexpr std::byte pad_info_type{0x09};
  static constexpr std::byte trigger_type{0x0b};
  static constexpr std::byte controller_13_type{0x0d};
};

using TakionInboundRumble = TakionInboundFeedback;
using TakionInboundTriggerEffects = TakionInboundFeedback;
using TakionInboundPadInfo = TakionInboundFeedback;

TEST_F(TakionInboundRumble, IsTyped) {
  InboundHarness rumble{InboundWire::negotiated_config()};
  rumble.deliver(1, 15, {std::byte{0xab}, std::byte{0x11}, std::byte{0x22}},
                 rumble_type);
  ASSERT_EQ(rumble.events.events.size(), 1U);
  const auto* event = rumble.events.last<takion::client_event::Rumble>();
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->player_index, 3U);
  EXPECT_EQ(event->left, 0x11U);
  EXPECT_EQ(event->right, 0x22U);
  EXPECT_EQ(event->unknown, 0xabU);
}

TEST_F(TakionInboundRumble, RejectsAShortMessage) {
  InboundHarness short_rumble{InboundWire::negotiated_config()};
  short_rumble.deliver(1, 15, {std::byte{0xab}, std::byte{0x11}}, rumble_type);
  EXPECT_EQ(short_rumble.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionInboundTriggerEffects, AreTyped) {
  InboundHarness triggers{InboundWire::negotiated_config()};
  triggers.deliver(1, 15, trigger_payload(), trigger_type);
  ASSERT_EQ(triggers.events.events.size(), 1U);
  const auto* effects =
      triggers.events.last<takion::client_event::TriggerEffects>();
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 2U);
  EXPECT_EQ(effects->type_left, 0xf1U);
  EXPECT_EQ(effects->type_right, 0xf2U);
  EXPECT_EQ(effects->left[0], 1U);
  EXPECT_EQ(effects->left[9], 10U);
  EXPECT_EQ(effects->right[0], 11U);
  EXPECT_EQ(effects->right[9], 20U);
}

TEST_F(TakionInboundTriggerEffects, SkipAnEightBytePrefix) {
  const auto message = prefixed(trigger_payload());
  ASSERT_EQ(message.size(), 33U);
  InboundHarness long_triggers{InboundWire::negotiated_config()};
  long_triggers.deliver(1, 15, message, trigger_type);
  const auto* effects =
      long_triggers.events.last<takion::client_event::TriggerEffects>();
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 2U);
  EXPECT_EQ(effects->type_left, 0xf1U);
  EXPECT_EQ(effects->left[9], 10U);
  EXPECT_EQ(effects->right[9], 20U);
}

TEST_F(TakionInboundTriggerEffects, RejectAShortMessage) {
  auto payload = trigger_payload();
  payload.resize(24);
  InboundHarness short_triggers{InboundWire::negotiated_config()};
  short_triggers.deliver(1, 15, payload, trigger_type);
  EXPECT_EQ(short_triggers.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionInboundPadInfo, ReadsTheLongRow) {
  std::vector<std::byte> long_row(0x19);
  long_row[8] = std::byte{0};
  long_row[9] = std::byte{0x11};
  long_row[10] = std::byte{0x22};
  long_row[11] = std::byte{0x33};
  long_row[12] = std::byte{1};
  long_row[13] = std::byte{1};
  long_row[14] = std::byte{0};
  long_row[15] = std::byte{1};
  long_row[20] = std::byte{3};
  long_row[21] = std::byte{2};
  InboundHarness extended{InboundWire::negotiated_config()};
  extended.deliver(1, 15, long_row, pad_info_type);
  ASSERT_EQ(extended.events.events.size(), 1U);
  const auto* info = extended.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 0U);
  EXPECT_TRUE(info->motion_reset);
  EXPECT_TRUE(info->user_valid);
  EXPECT_EQ(info->haptic_intensity, 3U);
  EXPECT_EQ(info->trigger_intensity, 2U);
  EXPECT_EQ(info->led, (std::array<std::uint8_t, 3>{0x11, 0x22, 0x33}));
  EXPECT_EQ(info->motion_settings, (std::array<std::uint8_t, 2>{1, 0}));
}

TEST_F(TakionInboundPadInfo, ReadsTheCompactRow) {
  std::vector<std::byte> short_row(0x11);
  short_row[0] = std::byte{2};
  short_row[1] = std::byte{0x44};
  short_row[4] = std::byte{1};
  short_row[12] = std::byte{0};
  short_row[13] = std::byte{1};
  InboundHarness compact{InboundWire::negotiated_config()};
  compact.deliver(1, 15, short_row, pad_info_type);
  const auto* info = compact.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 2U);
  EXPECT_FALSE(info->motion_reset);
  EXPECT_FALSE(info->user_valid);
  EXPECT_EQ(info->haptic_intensity, 0U);
  EXPECT_EQ(info->trigger_intensity, 1U);
  EXPECT_EQ(info->led[0], 0x44U);
  EXPECT_FALSE(info->motion_settings);

  short_row[0] = std::byte{0};
  short_row[5] = std::byte{0};
  short_row[6] = std::byte{1};
  InboundHarness compact_first{InboundWire::negotiated_config()};
  compact_first.deliver(1, 15, short_row, pad_info_type);
  info = compact_first.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->motion_settings, (std::array<std::uint8_t, 2>{0, 1}));
}

TEST_F(TakionInboundPadInfo, ReadsTheCouchRow) {
  InboundHarness couch{InboundWire::negotiated_config()};
  couch.deliver(1, 15,
                {std::byte{1}, std::byte{5}, std::byte{6}, std::byte{7},
                 std::byte{0}, std::byte{0}, std::byte{0}},
                pad_info_type);
  const auto* info = couch.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 1U);
  EXPECT_TRUE(info->user_valid);
  EXPECT_FALSE(info->haptic_intensity);
  EXPECT_EQ(info->led, (std::array<std::uint8_t, 3>{5, 6, 7}));
}

TEST_F(TakionInboundPadInfo, IgnoresAnAcknowledgement) {
  InboundHarness acknowledgement{InboundWire::negotiated_config()};
  acknowledgement.deliver(1, 15, std::vector<std::byte>(4), pad_info_type);
  EXPECT_TRUE(acknowledgement.events.events.empty());
}

TEST_F(TakionInboundPadInfo, ReadsTheBareRow) {
  std::vector<std::byte> bare_row(12);
  bare_row[8] = std::byte{3};
  bare_row[9] = std::byte{0x0a};
  bare_row[11] = std::byte{0x0c};
  InboundHarness bare{InboundWire::negotiated_config()};
  bare.deliver(1, 15, bare_row, pad_info_type);
  const auto* info = bare.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 3U);
  EXPECT_TRUE(info->user_valid);
  EXPECT_FALSE(info->haptic_intensity);
  EXPECT_FALSE(info->motion_reset);
  EXPECT_EQ(info->led, (std::array<std::uint8_t, 3>{0x0a, 0, 0x0c}));
  EXPECT_FALSE(info->motion_settings);
}

TEST_F(TakionInboundPadInfo, RejectsAnUnsupportedSize) {
  InboundHarness unsupported{InboundWire::negotiated_config()};
  unsupported.deliver(1, 15, std::vector<std::byte>(5), pad_info_type);
  EXPECT_EQ(unsupported.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionInboundFeedback, Version20FixedLayoutMessagesDropTheirPrefix) {
  const auto config = InboundWire::fixed_config(20);

  InboundHarness rumble{config};
  rumble.deliver(1, 20,
                 prefixed({std::byte{0x06}, std::byte{0x40}, std::byte{0x80}}),
                 rumble_type);
  ASSERT_EQ(rumble.events.events.size(), 1U);
  const auto* shake = rumble.events.last<takion::client_event::Rumble>();
  ASSERT_NE(shake, nullptr);
  EXPECT_EQ(shake->unknown, 0x06U);
  EXPECT_EQ(shake->player_index, 2U);
  EXPECT_EQ(shake->left, 0x40U);
  EXPECT_EQ(shake->right, 0x80U);

  std::vector<std::byte> trigger(25);
  trigger[0] = std::byte{1};
  trigger[1] = std::byte{0x21};
  trigger[2] = std::byte{0x26};
  for (std::size_t index = 0; index < 10; ++index) {
    trigger[5 + index] = static_cast<std::byte>(index + 1);
    trigger[15 + index] = static_cast<std::byte>(0x80 + index);
  }
  InboundHarness triggers{config};
  triggers.deliver(1, 20, prefixed(trigger), trigger_type);
  ASSERT_EQ(triggers.events.events.size(), 1U);
  const auto* effects =
      triggers.events.last<takion::client_event::TriggerEffects>();
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 1U);
  EXPECT_EQ(effects->type_left, 0x21U);
  EXPECT_EQ(effects->type_right, 0x26U);
  EXPECT_EQ(effects->left[0], 1U);
  EXPECT_EQ(effects->left[9], 10U);
  EXPECT_EQ(effects->right[0], 0x80U);
  EXPECT_EQ(effects->right[9], 0x89U);

  std::vector<std::byte> row(0x11);
  row[0] = std::byte{3};
  row[1] = std::byte{0x10};
  row[2] = std::byte{0x20};
  row[3] = std::byte{0x30};
  row[7] = std::byte{1};
  row[12] = std::byte{2};
  row[13] = std::byte{1};
  InboundHarness pad{config};
  pad.deliver(1, 20, prefixed(row), pad_info_type);
  ASSERT_EQ(pad.events.events.size(), 1U);
  const auto* info = pad.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 3U);
  EXPECT_TRUE(info->user_valid);
  EXPECT_EQ(info->haptic_intensity, 2U);
  EXPECT_EQ(info->trigger_intensity, 1U);
  EXPECT_EQ(info->led, (std::array<std::uint8_t, 3>{0x10, 0x20, 0x30}));

  InboundHarness bare{config};
  bare.deliver(1, 20, std::vector<std::byte>(8, std::byte{0x01}), rumble_type);
  EXPECT_TRUE(bare.events.events.empty());
}

TEST_F(TakionInboundFeedback, Version18MessagesHaveNoPrefix) {
  InboundHarness unprefixed{InboundWire::fixed_config(18)};
  unprefixed.deliver(1, 18, {std::byte{0x06}, std::byte{0x40}, std::byte{0x80}},
                     rumble_type);
  const auto* shake = unprefixed.events.last<takion::client_event::Rumble>();
  ASSERT_NE(shake, nullptr);
  EXPECT_EQ(shake->left, 0x40U);
  EXPECT_EQ(shake->right, 0x80U);
}

TEST_F(TakionInboundFeedback, ControllerSelectorsSplitFromVersion16) {
  std::vector<std::byte> row(0x11);
  row[0] = std::byte{0x20};
  row[1] = std::byte{0x44};
  row[4] = std::byte{1};
  row[7] = std::byte{1};
  InboundHarness whole_row{InboundWire::fixed_config(15)};
  whole_row.deliver(1, 15, row, pad_info_type);
  const auto* info = whole_row.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 0x20U);
  EXPECT_FALSE(info->motion_reset);
  EXPECT_EQ(info->led[0], 0x44U);
  InboundHarness split_row{InboundWire::fixed_config(16)};
  split_row.deliver(1, 16, row, pad_info_type);
  info = split_row.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 0U);
  EXPECT_TRUE(info->motion_reset);
  EXPECT_EQ(info->led[0], 0x44U);

  std::vector<std::byte> prefixed_row(0x19);
  prefixed_row[8] = std::byte{0x31};
  prefixed_row[12] = std::byte{1};
  InboundHarness split_prefixed{InboundWire::fixed_config(16)};
  split_prefixed.deliver(1, 16, prefixed_row, pad_info_type);
  info = split_prefixed.events.last<takion::client_event::PadInfo>();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->index, 1U);
  EXPECT_FALSE(info->motion_reset);

  std::vector<std::byte> trigger(25);
  trigger[0] = std::byte{0x52};
  trigger[1] = std::byte{0x21};
  InboundHarness whole_trigger{InboundWire::fixed_config(15)};
  whole_trigger.deliver(1, 15, trigger, trigger_type);
  const auto* effects =
      whole_trigger.events.last<takion::client_event::TriggerEffects>();
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 0x52U);
  EXPECT_EQ(effects->type_left, 0x21U);
  InboundHarness split_trigger{InboundWire::fixed_config(16)};
  split_trigger.deliver(1, 16, trigger, trigger_type);
  effects = split_trigger.events.last<takion::client_event::TriggerEffects>();
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(effects->player_index, 2U);
  EXPECT_EQ(effects->type_left, 0x21U);

  InboundHarness split_rumble{InboundWire::fixed_config(16)};
  split_rumble.deliver(
      1, 16, {std::byte{0x75}, std::byte{0x10}, std::byte{0x20}}, rumble_type);
  const auto* shake = split_rumble.events.last<takion::client_event::Rumble>();
  ASSERT_NE(shake, nullptr);
  EXPECT_EQ(shake->player_index, 1U);
  EXPECT_EQ(shake->unknown, 0x75U);
  EXPECT_EQ(shake->left, 0x10U);
  EXPECT_EQ(shake->right, 0x20U);
}

class TakionInboundControllerMessage13 : public TakionInboundFeedback {
 protected:
  const std::vector<std::byte> message{std::byte{0x9a}, std::byte{0x01},
                                       std::byte{0x02}, std::byte{0x03},
                                       std::byte{0x04}};
};

TEST_F(TakionInboundControllerMessage13, IsAProbeBeforeVersion17) {
  InboundHarness early{InboundWire::fixed_config(16)};
  early.deliver(1, 16, message, controller_13_type);
  const auto* raw = early.events.last<takion::client_event::ProbeReceived>();
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(raw->family, takion::PacketFamily::control);
  EXPECT_EQ(raw->bytes, message);
}

TEST_F(TakionInboundControllerMessage13, IsTypedFromVersion17) {
  InboundHarness typed{InboundWire::fixed_config(17)};
  typed.deliver(1, 17, message, controller_13_type);
  ASSERT_EQ(typed.events.events.size(), 1U);
  const auto* controller =
      typed.events.last<takion::client_event::ControllerMessage13>();
  ASSERT_NE(controller, nullptr);
  EXPECT_EQ(controller->selector, 0x9aU);
  EXPECT_EQ(controller->payload, (std::array<std::uint8_t, 4>{1, 2, 3, 4}));
}

TEST_F(TakionInboundControllerMessage13, IsRejectedWhenTruncated) {
  InboundHarness truncated{InboundWire::fixed_config(17)};
  truncated.deliver(1, 17,
                    std::vector<std::byte>(message.begin(), message.end() - 1),
                    controller_13_type);
  EXPECT_EQ(truncated.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionInboundControllerMessage13, DropsItsPrefixAtVersion20) {
  std::vector<std::byte> prefixed_message(8, std::byte{0xee});
  prefixed_message.push_back(std::byte{0xf3});
  prefixed_message.insert(prefixed_message.end(), message.begin() + 1,
                          message.end());
  InboundHarness extended{InboundWire::fixed_config(20)};
  extended.deliver(1, 20, prefixed_message, controller_13_type);
  const auto* controller =
      extended.events.last<takion::client_event::ControllerMessage13>();
  ASSERT_NE(controller, nullptr);
  EXPECT_EQ(controller->selector, 0xf3U);
  EXPECT_EQ(controller->payload, (std::array<std::uint8_t, 4>{1, 2, 3, 4}));
}

}  // namespace
}  // namespace takion_test
