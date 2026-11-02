#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionFeedbackHistoryPackets : public ::testing::Test {
 protected:
  TakionFeedbackHistoryPackets() { values.sequence = 0x1234; }

  const takion::PacketEnvelope plain_envelope{takion::PlainEnvelope{}};
  takion::FeedbackHistoryValues values;
};

TEST_F(TakionFeedbackHistoryPackets, EncodeAPressedButtonAfterTheHeader) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::cross, true}};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_packet,
            TakionCodecRig::bytes({0x01, 0x12, 0x34, 0x00, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0x80, 0x88, 0xff}));
  EXPECT_EQ(encoded.value().datagram_size, 15);
  EXPECT_EQ(encoded.value().event_payload_offset, 12);
  EXPECT_EQ(encoded.value().event_payload_size, 3);
  EXPECT_EQ(encoded.value().event_count, 1);
}

TEST_F(TakionFeedbackHistoryPackets, EncodeAReleasedPsButton) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::ps, false}};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 2,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0x80, 0x8e}));
}

TEST_F(TakionFeedbackHistoryPackets, EncodeAPressedPsButton) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::ps, true}};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 2,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0x80, 0xae}));
}

TEST_F(TakionFeedbackHistoryPackets, EncodeAnAnalogButtonWithItsValue) {
  values.events = {
      takion::FeedbackAnalogButtonEvent{takion::FeedbackAnalogButton::l2, 42}};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 3,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0x80, 0x86, 42}));
}

TEST_F(TakionFeedbackHistoryPackets, EncodeATouch) {
  values.events = {takion::FeedbackTouchEvent{true, 3, 1920, 942}};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 5,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0xd0, 0x03, 0x78, 0x03, 0xae}));
}

TEST_F(TakionFeedbackHistoryPackets, CountEveryEventInThePayload) {
  values.events = {
      takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::cross,
                                         true},
      takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::l1,
                                         false},
      takion::FeedbackTouchEvent{true, 3, 1920, 942},
  };
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().event_count, 3);
  EXPECT_EQ(encoded.value().event_payload_size, 11);
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 11,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0x80, 0x88, 0xff, 0x80, 0x84, 0x00, 0xd0,
                                   0x03, 0x78, 0x03, 0xae}));
}

TEST_F(TakionFeedbackHistoryPackets, SitBehindAnExtendedHeaderAtVersion20) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::ps, false}};
  values.extended_header = takion::ExtendedPacketHeader{0x11223344, 0x55667788};
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(20),
      takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(std::equal(
      encoded.value().encoded_packet.begin(),
      encoded.value().encoded_packet.begin() + 20,
      TakionCodecRig::bytes({0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                             0x77, 0x88, 0x12, 0x34, 0x00, 0,    0,
                             0,    0,    0,    0,    0,    0})
          .begin()));
  EXPECT_EQ(encoded.value().event_payload_offset, 20);
  EXPECT_EQ(encoded.value().event_payload_size, 3);
  EXPECT_EQ(encoded.value().encoded_packet.size(), 23);
  EXPECT_EQ(encoded.value().datagram_size, 27);
  EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.begin() + 20,
                                   encoded.value().encoded_packet.end()),
            TakionCodecRig::bytes({0x80, 0x8e, 0x00}));
  values.extended_header.reset();
}

TEST_F(TakionFeedbackHistoryPackets, EncodeAnEmptyHistory) {
  values.events.clear();
  auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(
      encoded.value().encoded_packet,
      TakionCodecRig::bytes({0x01, 0x12, 0x34, 0x00, 0, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(encoded.value().event_count, 0);
  EXPECT_EQ(encoded.value().event_payload_size, 0);
}

TEST_F(TakionFeedbackHistoryPackets, StampTheControllerOnEveryEvent) {
  for (std::uint8_t controller_id = 1; controller_id <= 3; ++controller_id) {
    values.controller_id = controller_id;
    values.events = {takion::FeedbackDigitalButtonEvent{
        takion::FeedbackDigitalButton::cross, true}};
    auto encoded = takion::PacketCodec::encode_feedback_history_packet(
        values, TakionCodecRig::profile(7), plain_envelope);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded.value().encoded_packet[12],
              static_cast<std::byte>(0x80U | controller_id));
    EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 2,
                                     encoded.value().encoded_packet.end()),
              TakionCodecRig::bytes({0x88, 0xff}));

    values.events = {takion::FeedbackTouchEvent{true, 3, 1920, 942}};
    encoded = takion::PacketCodec::encode_feedback_history_packet(
        values, TakionCodecRig::profile(7), plain_envelope);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded.value().encoded_packet[12],
              static_cast<std::byte>(0xd0U | controller_id));
    EXPECT_EQ(std::vector<std::byte>(encoded.value().encoded_packet.end() - 4,
                                     encoded.value().encoded_packet.end()),
              TakionCodecRig::bytes({0x03, 0x78, 0x03, 0xae}));

    values.events = {
        takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::cross,
                                           true},
        takion::FeedbackAnalogButtonEvent{takion::FeedbackAnalogButton::l2,
                                          0x40},
        takion::FeedbackTouchEvent{true, 3, 1920, 942},
        takion::FeedbackTouchEvent{false, 3, 1920, 942},
    };
    encoded = takion::PacketCodec::encode_feedback_history_packet(
        values, TakionCodecRig::profile(7), plain_envelope);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded.value().event_count, 4);
    EXPECT_EQ(encoded.value().encoded_packet[12],
              static_cast<std::byte>(0x80U | controller_id));
    EXPECT_EQ(encoded.value().encoded_packet[15],
              static_cast<std::byte>(0x80U | controller_id));
    EXPECT_EQ(encoded.value().encoded_packet[18],
              static_cast<std::byte>(0xd0U | controller_id));
    EXPECT_EQ(encoded.value().encoded_packet[23],
              static_cast<std::byte>(0xc0U | controller_id));
  }
}

TEST_F(TakionFeedbackHistoryPackets, StampTheControllerBehindAnExtendedHeader) {
  values.controller_id = 2;
  values.events = {takion::FeedbackTouchEvent{false, 3, 1920, 942}};
  values.extended_header = takion::ExtendedPacketHeader{1, 2};
  const auto encoded = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(20),
      takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().event_payload_offset, 20);
  EXPECT_EQ(encoded.value().encoded_packet[20], std::byte{0xc2});
}

TEST_F(TakionFeedbackHistoryPackets, RefuseAControllerBeyondTheFourth) {
  values.controller_id = 4;
  values.events = {takion::FeedbackTouchEvent{false, 3, 1920, 942}};
  EXPECT_FALSE(takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope));
}

TEST_F(TakionFeedbackHistoryPackets, RefuseAnUnknownDigitalButton) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      static_cast<takion::FeedbackDigitalButton>(0x99), true}};
  auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().reason, takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackHistoryPackets, RefuseAnUnknownAnalogButton) {
  values.events = {takion::FeedbackAnalogButtonEvent{
      static_cast<takion::FeedbackAnalogButton>(0x99), 1}};
  auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().reason, takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackHistoryPackets, EncodeATouchAtTheEdgeOfTheScreen) {
  values.events = {takion::FeedbackTouchEvent{true, 0x85, 1919, 1079}};
  auto touched = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(touched.has_value());
  EXPECT_EQ(std::vector<std::byte>(touched.value().encoded_packet.end() - 5,
                                   touched.value().encoded_packet.end()),
            TakionCodecRig::bytes({0xd0, 0x05, 0x77, 0xf4, 0x37}));
}

TEST_F(TakionFeedbackHistoryPackets, ClampATouchBeyondTheScreen) {
  values.events = {takion::FeedbackTouchEvent{false, 1, 5000, 4096}};
  auto touched = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(touched.has_value());
  EXPECT_EQ(std::vector<std::byte>(touched.value().encoded_packet.end() - 5,
                                   touched.value().encoded_packet.end()),
            TakionCodecRig::bytes({0xc0, 0x01, 0xff, 0xff, 0xff}));
}

TEST_F(TakionFeedbackHistoryPackets, RefuseMoreThan255Events) {
  values.events = {takion::FeedbackTouchEvent{false, 1, 5000, 4096}};
  values.events.resize(256, takion::FeedbackDigitalButtonEvent{
                                takion::FeedbackDigitalButton::cross, true});
  auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().reason,
            takion::ParseFailureReason::invalid_length);
}

TEST_F(TakionFeedbackHistoryPackets, RefuseAMissingExtendedHeaderAtVersion20) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::cross, true}};
  auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(20), plain_envelope);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().reason, takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackHistoryPackets, RefuseAnExtendedHeaderAtVersion7) {
  values.events = {takion::FeedbackDigitalButtonEvent{
      takion::FeedbackDigitalButton::cross, true}};
  values.extended_header = takion::ExtendedPacketHeader{1, 2};
  auto rejected = takion::PacketCodec::encode_feedback_history_packet(
      values, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().reason, takion::ParseFailureReason::invalid_field);
}

class TakionFeedbackHistoryPads : public ::testing::Test {
 protected:
  TakionFeedbackHistoryPads() {
    values.sequence = 0x0102;
    values.events = {
        takion::FeedbackPresenceEvent{true},
        takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::cross,
                                           true},
        takion::FeedbackTouchEvent{false, 1, 0, 0},
    };
  }

  auto encode() const {
    return takion::PacketCodec::encode_feedback_history_packet(
        values, TakionCodecRig::profile(7), plain_envelope);
  }

  const takion::PacketEnvelope plain_envelope{takion::PlainEnvelope{}};
  takion::FeedbackHistoryValues values;
};

TEST_F(TakionFeedbackHistoryPads, StampEachEventWithItsOwnPad) {
  values.event_controller_ids = {2, 0, 3};
  const auto encoded = encode();
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().event_count, 3U);
  EXPECT_EQ(
      encoded.value().encoded_packet,
      TakionCodecRig::bytes({0x01, 0x01, 0x02, 0x02, 0,    0,    0,    0,
                             0,    0,    0,    0,    0x82, 0xbf, 0x80, 0x88,
                             0xff, 0xc3, 0x01, 0x00, 0x00, 0x00}));
}

TEST_F(TakionFeedbackHistoryPads, PreferThePerEventPadOverThePacketPad) {
  values.controller_id = 1;
  values.event_controller_ids = {0, 0, 0};
  const auto encoded = encode();
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_packet[12], std::byte{0x80});
}

TEST_F(TakionFeedbackHistoryPads, RefuseAPadBeyondTheFourth) {
  values.controller_id = 1;
  values.event_controller_ids = {0, 4, 0};
  EXPECT_FALSE(encode());
}

TEST_F(TakionFeedbackHistoryPads, RefuseAPadListOfAnotherLength) {
  values.controller_id = 1;
  values.event_controller_ids = {0, 0};
  EXPECT_FALSE(encode());
}

}  // namespace
}  // namespace takion_test
