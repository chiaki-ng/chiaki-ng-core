#include "src/core/net/protocol/takion/detail/feedback_record_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionFeedbackRecords : public ::testing::Test {
 protected:
  static auto encode(const takion::FeedbackHistoryEvent& event,
                     std::uint8_t controller_id, std::uint32_t version) {
    return takion::FeedbackRecordCodec::encode(
        event, controller_id, TakionCodecRig::profile(version));
  }

  static void expect_rejected(const takion::FeedbackHistoryEvent& event,
                              std::uint32_t version,
                              takion::ParseFailureReason reason) {
    const auto encoded = encode(event, 0, version);
    ASSERT_FALSE(encoded.has_value()) << "version " << version;
    EXPECT_EQ(encoded.error().reason, reason) << "version " << version;
  }

  static void expect_encoded(const takion::FeedbackHistoryEvent& event,
                             std::uint8_t controller_id, std::uint32_t version,
                             const std::vector<std::byte>& wire) {
    const auto encoded = encode(event, controller_id, version);
    ASSERT_TRUE(encoded.has_value()) << "version " << version;
    EXPECT_EQ(encoded.value(), wire) << "version " << version;
  }
};

TEST_F(TakionFeedbackRecords, EncodesButton20FromVersion12) {
  const takion::FeedbackHistoryEvent event{takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::digital_20, true}};
  expect_rejected(event, 11, takion::ParseFailureReason::unsupported_layout);
  const auto encoded = encode(event, 1, 12);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), TakionCodecRig::bytes({0x81, 0xb4}));

  const takion::FeedbackButtonRecord record{.entries = {{20, 1}}};
  expect_rejected(record, 11, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(record, 1, 12, TakionCodecRig::bytes({0x81, 0xb4}));

  for (const auto button : {takion::FeedbackDigitalButton::digital_18,
                            takion::FeedbackDigitalButton::digital_19}) {
    const auto early =
        encode(takion::FeedbackDigitalButtonEvent{button, false}, 0, 9);
    ASSERT_TRUE(early);
    EXPECT_EQ(early.value().size(), 2U);
  }
}

TEST_F(TakionFeedbackRecords, EncodesButton21AndTheFlagFromVersion16) {
  const takion::FeedbackHistoryEvent event{takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::digital_21, true}};
  expect_rejected(event, 15, takion::ParseFailureReason::unsupported_layout);
  const auto encoded = encode(event, 1, 16);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), TakionCodecRig::bytes({0x81, 0xb5}));

  const takion::FeedbackButtonRecord flagged{.entries = {{12, 1}},
                                             .flag = true};
  expect_rejected(flagged, 15, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(flagged, 0, 16, TakionCodecRig::bytes({0x90, 0xac}));
}

TEST_F(TakionFeedbackRecords, AppendsTheButtonTrailingByteFromVersion19) {
  const takion::FeedbackHistoryEvent cross{takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::cross, true}};
  auto encoded = encode(cross, 0, 18);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), TakionCodecRig::bytes({0x80, 0x88, 0xff}));
  encoded = encode(cross, 0, 19);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), TakionCodecRig::bytes({0x80, 0x88, 0xff, 0x00}));

  const takion::FeedbackButtonRecord tailed{.entries = {{8, 0xff}},
                                            .trailing = 0x5a};
  expect_rejected(tailed, 18, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(tailed, 0, 19,
                 TakionCodecRig::bytes({0x80, 0x88, 0xff, 0x5a}));
  expect_encoded(takion::FeedbackButtonRecord{.entries = {{8, 0xff}}}, 0, 18,
                 TakionCodecRig::bytes({0x80, 0x88, 0xff}));

  encoded = encode(takion::FeedbackPresenceEvent{false}, 3, 20);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), TakionCodecRig::bytes({0x83, 0x9f, 0x00}));
}

TEST_F(TakionFeedbackRecords, OrdersButtonEntriesByTheirEncoding) {
  const takion::FeedbackButtonRecord record{
      .presence = true,
      .entries = {{6, 0x40}, {12, 1}, {20, 0}, {8, 0xff}, {17, 1}},
      .flag = true,
      .trailing = 0x33,
  };
  const auto wire = TakionCodecRig::bytes(
      {0x92, 0x3f, 0x2c, 0x31, 0x14, 0x06, 0x40, 0x88, 0xff, 0x33});
  const auto encoded = encode(record, 2, 19);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), wire);
  const takion::FeedbackButtonRecord sorted{
      .presence = true,
      .entries = {{12, 1}, {17, 1}, {20, 0}, {6, 0x40}, {8, 0xff}},
      .flag = true,
      .trailing = 0x33,
  };
  expect_encoded(sorted, 2, 19, wire);

  expect_rejected(takion::FeedbackButtonRecord{}, 19,
                  takion::ParseFailureReason::invalid_field);
  expect_rejected(takion::FeedbackButtonRecord{.entries = {{12, 2}}}, 19,
                  takion::ParseFailureReason::invalid_field);
  expect_rejected(takion::FeedbackButtonRecord{.entries = {{8, 1}, {8, 2}}}, 19,
                  takion::ParseFailureReason::invalid_field);
  expect_rejected(takion::FeedbackButtonRecord{.entries = {{22, 1}}}, 20,
                  takion::ParseFailureReason::unsupported_layout);
  EXPECT_FALSE(encode(
                   takion::FeedbackDigitalButtonEvent{
                       takion::FeedbackDigitalButton::cross, true},
                   4, 19)
                   .has_value());
}

TEST_F(TakionFeedbackRecords, PinsTheBytesOfATouchRecord) {
  expect_encoded(takion::FeedbackTouchEvent{true, 3, 1920, 942}, 2, 9,
                 TakionCodecRig::bytes({0xd2, 0x03, 0x78, 0x03, 0xae}));
}

TEST_F(TakionFeedbackRecords, EncodesKind0RecordsFromVersion14) {
  const takion::FeedbackKind0Record record{
      .raw_1 = 0x11,
      .raw_2 = 0x22,
      .upper_3 = 5,
      .entries = {{0x01, 0x02}, {0x03, 0x04}}};
  expect_rejected(record, 13, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(
      record, 2, 14,
      TakionCodecRig::bytes({0x02, 0x11, 0x22, 0xa2, 0x01, 0x02, 0x03, 0x04}));
  expect_encoded(takion::FeedbackKind0Record{}, 0, 20,
                 TakionCodecRig::bytes({0x00, 0x00, 0x00, 0x00}));

  auto full = takion::FeedbackKind0Record{};
  full.entries.resize(takion::feedback_kind0_max_entries);
  const auto encoded = encode(full, 0, 14);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value().size(), 36U);
  EXPECT_EQ(encoded.value()[3], std::byte{0x10});
  full.entries.push_back({});
  expect_rejected(full, 14, takion::ParseFailureReason::invalid_field);
  expect_rejected(takion::FeedbackKind0Record{.upper_3 = 8}, 14,
                  takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackRecords, EncodesLegacyKind1RecordsFromVersion14To16) {
  const takion::FeedbackKind1LegacyRecord record{
      .flag = true, .payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}};
  const auto wire =
      TakionCodecRig::bytes({0x31, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
  expect_rejected(record, 13, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(record, 1, 14, wire);
  expect_encoded(record, 1, 16, wire);
  expect_rejected(record, 17, takion::ParseFailureReason::unsupported_layout);
}

TEST_F(TakionFeedbackRecords, EncodesKind1ModeRecordsFromVersion17) {
  const takion::FeedbackKind1Record mode_1{
      .raw_1 = 0xaa,
      .raw_2 = 0xbb,
      .mode = 1,
      .common = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
      .mode_1_tail = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},
  };
  expect_rejected(mode_1, 16, takion::ParseFailureReason::unsupported_layout);
  expect_encoded(
      mode_1, 3, 17,
      TakionCodecRig::bytes({0x23, 0xaa, 0xbb, 0x01, 0x01, 0x02, 0x03,
                             0x04, 0x05, 0x06, 0x07, 0x08, 0x10, 0x11,
                             0x12, 0x13, 0x14, 0x15, 0x16, 0x17}));

  takion::FeedbackKind1Record mode_2{
      .flag = true,
      .raw_1 = 0x0c,
      .raw_2 = 0x0d,
      .mode = 2,
      .common = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
  };
  mode_2.mode_2_fields[0] = {0x01, 0x02};
  mode_2.mode_2_fields[12] = {0xa1, 0xa2, 0xa3, 0xa4};
  mode_2.mode_2_fields[13] = {0xb1, 0xb2, 0xb3, 0xb4};
  mode_2.mode_2_fields[15] = {0xf1, 0xf2};
  expect_encoded(mode_2, 1, 17,
                 TakionCodecRig::bytes(
                     {0x31, 0x0c, 0x0d, 0x02, 0x21, 0x22, 0x23, 0x24, 0x25,
                      0x26, 0x27, 0x28, 0x01, 0xb0, 0x01, 0x02, 0xa1, 0xa2,
                      0xa3, 0xa4, 0xb1, 0xb2, 0xb3, 0xb4, 0xf1, 0xf2}));
  expect_encoded(
      takion::FeedbackKind1Record{.mode = 2}, 0, 20,
      TakionCodecRig::bytes({0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

  takion::FeedbackKind1Record full{.mode = 2};
  for (std::size_t bit = 0; bit < full.mode_2_fields.size(); ++bit)
    full.mode_2_fields[bit].resize(
        takion::PacketCodec::feedback_kind1_field_size(bit));
  const auto encoded = encode(full, 0, 17);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value().size(), takion::feedback_record_max_size);

  auto narrow = mode_2;
  narrow.mode_2_fields[12] = {0xa1, 0xa2};
  expect_rejected(narrow, 17, takion::ParseFailureReason::invalid_field);
  auto mixed = mode_2;
  mixed.mode_1_tail[0] = 1;
  expect_rejected(mixed, 17, takion::ParseFailureReason::invalid_field);
  auto mode_1_with_fields = mode_1;
  mode_1_with_fields.mode_2_fields[0] = {0x01, 0x02};
  expect_rejected(mode_1_with_fields, 17,
                  takion::ParseFailureReason::invalid_field);
  expect_rejected(takion::FeedbackKind1Record{.mode = 3}, 17,
                  takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackRecords, CarriesEveryRecordKindInAHistoryPacket) {
  takion::FeedbackKind1Record mode_1{.mode = 1};
  takion::FeedbackHistoryValues values{
      .sequence = 0x0102,
      .events =
          {
              takion::FeedbackKind0Record{.raw_1 = 0x11,
                                          .entries = {{0x01, 0x02}}},
              mode_1,
              takion::FeedbackButtonRecord{
                  .entries = {{21, 1}}, .flag = true, .trailing = 0x44},
          },
      .controller_id = 1,
  };
  const auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(19),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().event_count, 3U);
  auto expected = TakionCodecRig::bytes(
      {0x01, 0x01, 0x02, 0x02, 0,    0,    0,    0,    0,    0,    0,
       0,    0x01, 0x11, 0x00, 0x01, 0x01, 0x02, 0x21, 0x00, 0x00, 0x01});
  expected.resize(expected.size() + 16);
  for (const auto value : {0x91, 0xb5, 0x44})
    expected.push_back(static_cast<std::byte>(value));
  EXPECT_EQ(encoded.value().encoded_packet, expected);
  EXPECT_EQ(encoded.value().event_payload_size, expected.size() - 12);

  values.events = {takion::FeedbackKind1LegacyRecord{}};
  const auto legacy = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(16),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(legacy);
  EXPECT_EQ(legacy.value().event_payload_size, 8U);
  EXPECT_EQ(legacy.value().encoded_packet[12], std::byte{0x21});
  const auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(17),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ParseFailureReason::unsupported_layout);
}

TEST_F(TakionFeedbackRecords, LaysOutTheMotionRecordByVersion) {
  takion::FeedbackStateValues values;
  values.controller_id = 2;
  values.orient_w = 1.0F;
  values.left_x = -32768;
  values.left_y = -1;
  values.right_y = 32767;
  const auto base = TakionCodecRig::bytes({
      0xa2, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f,
      0xff, 0x7f, 0xff, 0x7f, 0xfe, 0xf7, 0xef, 0x1f, 0x80,
      0x00, 0xff, 0xff, 0x00, 0x00, 0x7f, 0xff,
  });
  const auto with_flags = [&base](std::uint8_t flags) {
    auto wire = base;
    for (const auto value : {std::uint8_t{0}, std::uint8_t{0}, flags})
      wire.push_back(static_cast<std::byte>(value));
    return wire;
  };
  const auto payload = [&values](std::uint32_t version) {
    const auto encoded = takion::PacketCodec::encode_feedback_state(
        values, TakionCodecRig::profile(version), takion::PlainEnvelope{});
    EXPECT_TRUE(encoded.has_value());
    return encoded.value().encoded_payload;
  };

  for (const auto version : {9U, 10U, 11U})
    EXPECT_EQ(payload(version), base) << "version " << version;
  for (const auto version : {12U, 15U, 16U, 20U})
    EXPECT_EQ(payload(version), with_flags(0x01)) << "version " << version;

  values.trigger_states_emulated = false;
  EXPECT_EQ(payload(15), with_flags(0x00));
  values.second_motion_flag = true;
  const auto early = takion::PacketCodec::encode_feedback_state(
      values, TakionCodecRig::profile(15), takion::PlainEnvelope{});
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error().reason,
            takion::ParseFailureReason::unsupported_layout);
  EXPECT_EQ(payload(16), with_flags(0x02));
  values.trigger_states_emulated = true;
  EXPECT_EQ(payload(16), with_flags(0x03));
  values.second_motion_flag = false;
  values.trigger_states_emulated = false;
  values.left_trigger_state = 5;
  values.right_trigger_state = 3;
  auto reported = with_flags(0x00);
  reported[25] = std::byte{5};
  reported[26] = std::byte{3};
  EXPECT_EQ(payload(12), reported);
  EXPECT_EQ(payload(9), base);
}

}  // namespace
}  // namespace takion_test
