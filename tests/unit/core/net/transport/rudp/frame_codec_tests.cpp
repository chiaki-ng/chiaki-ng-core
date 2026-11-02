#include "tests/unit/core/net/transport/rudp/frame_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace rudp_test {
namespace {

class RudpFrameDecode : public FrameCodecRig {};

TEST_F(RudpFrameDecode, ReadsTheGoldenSingleFrame) {
  const auto result = FrameCodec::decode(golden);
  ASSERT_TRUE(result.has_value());
  const auto& frames = result.value();
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].type, 0x8030);
  ASSERT_EQ(frames[0].payload.size(), 2U);
  EXPECT_EQ(frames[0].payload[0], std::byte{0x12});
  EXPECT_EQ(frames[0].payload[1], std::byte{0x34});
}

TEST_F(RudpFrameDecode, SplitsConcatenatedFrames) {
  Bytes packet;
  append_frame(packet, 0x0001, {std::byte{0xaa}});
  append_frame(packet, 0x0002,
               {std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}});
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result.has_value());
  const auto& frames = result.value();
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[0].type, 1);
  ASSERT_EQ(frames[0].payload.size(), 1U);
  EXPECT_EQ(frames[0].payload[0], std::byte{0xaa});
  EXPECT_EQ(frames[1].type, 2);
  ASSERT_EQ(frames[1].payload.size(), 3U);
  EXPECT_EQ(frames[1].payload[2], std::byte{0xdd});
}

TEST_F(RudpFrameDecode, KeepsAnUnknownType) {
  const auto result = FrameCodec::decode(packet_of(0xBEEF, {std::byte{0x00}}));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value()[0].type, 0xBEEF);
}

TEST_F(RudpFrameDecode, RefusesADatagramUnderTheReferenceFloor) {
  for (std::size_t length = 0; length <= 8; ++length) {
    SCOPED_TRACE(length);
    const Bytes prefix(golden.begin(),
                       golden.begin() + static_cast<std::ptrdiff_t>(length));
    const auto result = FrameCodec::decode(prefix);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::malformed);
  }
}

TEST_F(RudpFrameDecode, ClampsAFrameToANineByteDatagram) {
  const Bytes nine(golden.begin(), golden.begin() + 9);
  const auto clamped = FrameCodec::decode(nine);
  ASSERT_TRUE(clamped);
  ASSERT_EQ(clamped.value().size(), 1U);
  EXPECT_EQ(clamped.value()[0].type, 0x8030);
  EXPECT_EQ(clamped.value()[0].payload, Bytes{std::byte{0x12}});
}

TEST_F(RudpFrameDecode, IgnoresTheMagicAndTheTopNibble) {
  Bytes packet = golden;
  packet[3] = std::byte{0x4e};
  packet[0] = std::byte{0xb0};
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value()[0].type, 0x8030);
  EXPECT_EQ(result.value()[0].payload,
            (Bytes{std::byte{0x12}, std::byte{0x34}}));
}

TEST_F(RudpFrameDecode, ClampsTheLengthToTheDatagram) {
  Bytes packet = golden;
  packet[0] = std::byte{0xcf};
  packet[1] = std::byte{0xff};
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value()[0].payload,
            (Bytes{std::byte{0x12}, std::byte{0x34}}));
}

TEST_F(RudpFrameDecode, LeavesBytesPastAShortLengthToTheNextFrame) {
  Bytes packet = golden;
  packet[1] = std::byte{0x08};
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_TRUE(result.value()[0].payload.empty());
}

TEST_F(RudpFrameDecode, ReadsTheFrameAfterAnUndersizedLength) {
  Bytes nested;
  append_frame(nested, 0x0001, {});
  nested[1] = std::byte{0x04};
  append_frame(nested, 0x0002, {std::byte{0xaa}});
  const auto two = FrameCodec::decode(nested);
  ASSERT_TRUE(two);
  ASSERT_EQ(two.value().size(), 2U);
  EXPECT_TRUE(two.value()[0].payload.empty());
  EXPECT_EQ(two.value()[1].type, 0x0002);
  EXPECT_EQ(two.value()[1].payload, Bytes{std::byte{0xaa}});
}

TEST_F(RudpFrameDecode, DropsATrailingFragmentShorterThanAHeader) {
  for (std::size_t extra = 1; extra < 8; ++extra) {
    SCOPED_TRACE(extra);
    Bytes packet = golden;
    packet.insert(packet.end(), extra, std::byte{0xff});
    const auto result = FrameCodec::decode(packet);
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].payload.size(), 2U);
  }
}

TEST_F(RudpFrameDecode, ReadsATrailingHeaderAsAnEmptyFrame) {
  Bytes packet = golden;
  packet.insert(packet.end(), 8, std::byte{0x00});
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 2U);
  EXPECT_TRUE(result.value()[1].payload.empty());
}

TEST_F(RudpFrameDecode, ReadsAnyNumberOfFrames) {
  Bytes packet;
  const Bytes empty_payload;
  for (int index = 0; index < 40; ++index)
    append_frame(packet, 1, empty_payload);
  const auto result = FrameCodec::decode(packet);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().size(), 40U);
}

TEST_F(RudpFrameDecode, RefusesADatagramOverTheLimit) {
  const Bytes packet(65537, std::byte{0});
  const auto result = FrameCodec::decode(packet);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::too_large);
}

class RudpFrameEncode : public FrameCodecRig {};

TEST_F(RudpFrameEncode, WritesTheGoldenInitRequestFrame) {
  const Frame frame{0x8030, {std::byte{0x12}, std::byte{0x34}}};
  const auto result = FrameCodec::encode({&frame, 1});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), golden);
}

TEST_F(RudpFrameEncode, RoundTripsTwoFramesExactly) {
  Bytes packet;
  append_frame(packet, 0x0001, {std::byte{0xaa}});
  append_frame(packet, 0x0002,
               {std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}});
  const auto decoded = FrameCodec::decode(packet);
  ASSERT_TRUE(decoded.has_value());
  const auto encoded = FrameCodec::encode(decoded.value());
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value(), packet);
}

TEST_F(RudpFrameEncode, KeepsAnUnknownType) {
  const Frame frame{0xBEEF, {std::byte{0x00}}};
  const auto encoded = FrameCodec::encode({&frame, 1});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = FrameCodec::decode(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_FALSE(decoded.value().empty());
  EXPECT_EQ(decoded.value()[0].type, 0xBEEF);
}

TEST_F(RudpFrameEncode, RefusesNoFrames) {
  const auto result = FrameCodec::encode({});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpFrameEncode, RefusesMoreThanSixteenFrames) {
  const std::vector<Frame> seventeen(17, Frame{1, {}});
  const auto result = FrameCodec::encode(seventeen);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::too_many_frames);
}

TEST_F(RudpFrameEncode, AcceptsSixteenFrames) {
  const std::vector<Frame> sixteen(16, Frame{1, {}});
  const auto result = FrameCodec::encode(sixteen);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().size(), 16U * 8U);
}

TEST_F(RudpFrameEncode, AcceptsAFrameUpToTheLengthField) {
  const Frame fits{1, Bytes(4087, std::byte{0x00})};
  const auto accept = FrameCodec::encode({&fits, 1});
  ASSERT_TRUE(accept.has_value());
  EXPECT_EQ(accept.value().size(), 4095U);
}

TEST_F(RudpFrameEncode, RefusesAFrameOverTheLengthField) {
  const Frame too_big{1, Bytes(4088, std::byte{0x00})};
  const auto reject = FrameCodec::encode({&too_big, 1});
  ASSERT_FALSE(reject.has_value());
  EXPECT_EQ(reject.error(), FrameError::too_large);
}

class RudpFinishPacket : public FrameCodecRig {
 protected:
  const Frame finish{0xC000, {std::byte{0x00}}};
  const Frame other{0x8030, {std::byte{0x12}}};
};

TEST_F(RudpFinishPacket, RecognisesAFinishFrame) {
  const auto single = FrameCodec::encode({&finish, 1});
  ASSERT_TRUE(single);
  EXPECT_TRUE(FrameCodec::is_finish_packet(single.value()));
}

TEST_F(RudpFinishPacket, RecognisesAFinishFrameWithFlags) {
  const Frame finish_with_flags{0xC030, {std::byte{0x00}}};
  const auto flagged = FrameCodec::encode({&finish_with_flags, 1});
  ASSERT_TRUE(flagged);
  EXPECT_TRUE(FrameCodec::is_finish_packet(flagged.value()));
}

TEST_F(RudpFinishPacket, RecognisesAFinishFrameAfterAnother) {
  const std::vector<Frame> frames{other, finish};
  const auto compound = FrameCodec::encode(frames);
  ASSERT_TRUE(compound);
  EXPECT_TRUE(FrameCodec::is_finish_packet(compound.value()));
}

TEST_F(RudpFinishPacket, RefusesAPacketWithoutAFinishFrame) {
  const auto plain = FrameCodec::encode({&other, 1});
  ASSERT_TRUE(plain);
  EXPECT_FALSE(FrameCodec::is_finish_packet(plain.value()));
  EXPECT_FALSE(FrameCodec::is_finish_packet({}));
}

TEST_F(RudpFinishPacket, RefusesATruncatedFinishFrame) {
  const auto single = FrameCodec::encode({&finish, 1});
  ASSERT_TRUE(single);
  const Bytes truncated(single.value().begin(), single.value().end() - 2);
  EXPECT_FALSE(FrameCodec::is_finish_packet(truncated));
}

}  // namespace
}  // namespace rudp_test
