#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionNativeAvVectors : public ::testing::Test {
 protected:
  struct NativeAvVector final {
    std::uint32_t version;
    bool video;
    bool slice_extension;
    const char* hex;
    std::uint16_t packet_index;
    std::uint16_t frame_index;
    std::uint16_t unit_index;
    std::uint16_t total;
    std::uint16_t fec;
    std::uint8_t codec;
    std::uint16_t video_word;
    std::uint8_t adaptive_stream_index;
    bool dynamic_range_flag;
    bool suppress_presentation;
    std::uint8_t audio_profile;
    std::uint8_t audio_mode;
    std::optional<takion::VideoSliceExtension> slice;
  };

  using VideoSliceExtension = takion::VideoSliceExtension;

  static constexpr NativeAvVector native_av_vectors_21_to_25[]{
      {21, false, false,
       "030000000000000000521055960205e0021b05f2097b6f0892508560", 21008, 21910,
       2, 6, 2, 27, 0, 0, false, false, 6, 14, std::nullopt},
      {21, false, true,
       "1300000000000000006e636e3f0205500208ed52516d1f52b87b1176a6", 28259,
       28223, 2, 6, 2, 8, 0, 0, false, false, 10, 5,
       VideoSliceExtension{0, 4, false, true}},
      {21, true, false,
       "0200000000000000004a569238004014020af5a4160d4f9ad850939d20", 19030,
       37432, 2, 6, 2, 10, 37789, 1, false, false, 0, 0, std::nullopt},
      {21, true, true,
       "120000000000000000914b5289002014021c066ac509e95e83390f8c61000011",
       37195, 21129, 1, 6, 2, 28, 3980, 3, true, false, 0, 0,
       VideoSliceExtension{0, 4, false, true}},
      {23, false, false,
       "03000000000000000097cef1fe0105a0023a7f1389c0b159abed2b2e", 38862, 61950,
       1, 6, 2, 58, 0, 0, false, false, 2, 10, std::nullopt},
      {23, false, true,
       "1300000000000000009ad2a5a80005900250c59faa8e18114c80305d22", 39634,
       42408, 0, 6, 2, 80, 0, 0, false, false, 2, 9,
       VideoSliceExtension{1, 4, false, false}},
      {23, true, false,
       "0200000000000000001b3e7a810040140271c1a160ac24ec5e4c191441", 6974,
       31361, 2, 6, 2, 113, 6420, 2, true, false, 0, 0, std::nullopt},
      {23, true, true,
       "120000000000000000dfd7285d0060140277f86aef1d1d36bee9b467c0004013",
       57303, 10333, 3, 6, 2, 119, 46183, 6, false, false, 0, 0,
       VideoSliceExtension{2, 4, true, true}},
      {25, false, false,
       "0300000000000000001fd2c5790305a0022273046a250af45e0b714d", 8146, 50553,
       3, 6, 2, 34, 0, 0, false, false, 4, 10, std::nullopt},
      {25, false, true,
       "130000000000000000a2bbea7a01058002096497f64da012a5b050be40", 41659,
       60026, 1, 6, 2, 9, 0, 0, false, false, 4, 8,
       VideoSliceExtension{2, 4, false, false}},
      {25, true, false,
       "02000000000000000067a9bb0a004014027e09d1c001f5637685ffa763", 26537,
       47882, 2, 6, 2, 126, 65447, 3, true, true, 0, 0, std::nullopt},
      {25, true, true,
       "12000000000000000003bff4110060140238e88c07824af3b67c5441c1000011", 959,
       62481, 3, 6, 2, 56, 21569, 6, true, false, 0, 0,
       VideoSliceExtension{0, 4, false, true}},
  };

  static auto packet(std::uint8_t type, const std::vector<std::byte>& payload)
      -> takion::PlainPacket {
    return takion::PlainPacket{takion::PacketType{type}, std::nullopt, payload};
  }

  auto packet_with(std::uint8_t type, std::vector<std::byte> payload,
                   std::initializer_list<std::uint8_t> data)
      -> takion::PlainPacket {
    for (const auto value : data) payload.push_back(std::byte{value});
    payloads.push_back(std::move(payload));
    return takion::PlainPacket{takion::PacketType{type}, std::nullopt,
                               payloads.back()};
  }

  std::deque<std::vector<std::byte>> payloads;
};

TEST_F(TakionNativeAvVectors, DecodeVersion20AudioWithNaluInfo) {
  const auto audio20 = TakionCodecRig::bytes({
      0x9d, 0x07, 0x98, 0xea, 0x00, 0x05, 0x90, 0x02, 0x4d, 0x08,
      0xd6, 0xf5, 0xfd, 0xb7, 0x99, 0x20, 0xaa, 0x51, 0xe2, 0x2a,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet(0x13, audio20), TakionCodecRig::profile(20));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().uses_nalu_info_structs);
  EXPECT_EQ(parsed.value().packet_index, 0x9d07);
  EXPECT_EQ(parsed.value().frame_index, 0x98ea);
  EXPECT_EQ(parsed.value().unit_index, 0);
  EXPECT_EQ(parsed.value().units_in_frame_total, 6);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
  EXPECT_EQ(parsed.value().audio_mode, 9);
  EXPECT_EQ(parsed.value().codec, 0x4d);
  EXPECT_EQ(parsed.value().av_tag, std::optional<std::uint8_t>{0x0a});
  EXPECT_EQ(parsed.value().audio_profile, 2);
  EXPECT_FALSE(parsed.value().is_haptics);
  EXPECT_TRUE(parsed.value().data.empty());
}

TEST_F(TakionNativeAvVectors, DecodeVersion19AudioAndHaptics) {
  const auto audio19 = TakionCodecRig::bytes({
      0xe6, 0xfa, 0x44, 0x57, 0x01, 0x05, 0x90, 0x02, 0x45, 0xc4,
      0x2c, 0xfa, 0xf1, 0x58, 0xa0, 0x6d, 0xd1, 0x32, 0xf8, 0x23,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet(0x13, audio19), TakionCodecRig::profile(19));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().unit_index, 1);
  EXPECT_EQ(parsed.value().units_in_frame_total, 6);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
  EXPECT_EQ(parsed.value().audio_mode, 9);
  EXPECT_EQ(parsed.value().av_tag, std::optional<std::uint8_t>{0x03});
  EXPECT_EQ(parsed.value().audio_profile, 2);
  EXPECT_TRUE(parsed.value().is_haptics);
  EXPECT_TRUE(parsed.value().data.empty());
  parsed = takion::PacketCodec::decode_av_packet(packet(0x03, audio19),
                                                 TakionCodecRig::profile(19));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed.value().uses_nalu_info_structs);
  EXPECT_EQ(parsed.value().av_tag, std::optional<std::uint8_t>{0x08});
  EXPECT_EQ(parsed.value().audio_profile, 0xf);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({0x23}));
  const auto rejected = takion::PacketCodec::decode_av_packet(
      packet(0x13, audio19), TakionCodecRig::profile(13));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ParseFailureReason::unsupported_layout);
  EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
      packet(0x13, audio19), TakionCodecRig::profile(9)));
}

TEST_F(TakionNativeAvVectors, DecodeAVersion20VideoSlice) {
  auto video20 = TakionCodecRig::bytes({
      0x5a, 0xb1, 0xb5, 0x39, 0x00, 0x60, 0x14, 0x02, 0x23,
      0x19, 0x72, 0x0e, 0x60, 0xd3, 0x4e, 0x1f, 0xa9, 0xa6,
      0x4d, 0x80, 0x00, 0x60, 0x12, 1,    2,    3,    4,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet(0x12, video20), TakionCodecRig::profile(20));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().packet_index, 0x5ab1);
  EXPECT_EQ(parsed.value().frame_index, 0xb539);
  EXPECT_EQ(parsed.value().unit_index, 3);
  EXPECT_EQ(parsed.value().units_in_frame_total, 6);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
  EXPECT_EQ(parsed.value().codec, 0x23);
  EXPECT_EQ(parsed.value().video_word, 0xa64d);
  EXPECT_EQ(parsed.value().adaptive_stream_index, 4);
  ASSERT_TRUE(parsed.value().slice.has_value());
  EXPECT_EQ(parsed.value().slice->index, 3);
  EXPECT_EQ(parsed.value().slice->count, 4);
  EXPECT_TRUE(parsed.value().slice->start);
  EXPECT_FALSE(parsed.value().slice->end);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({1, 2, 3, 4}));
  parsed = takion::PacketCodec::decode_av_packet(packet(0x02, video20),
                                                 TakionCodecRig::profile(20));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed.value().slice.has_value());
  EXPECT_EQ(parsed.value().data,
            TakionCodecRig::bytes({0x00, 0x60, 0x12, 1, 2, 3, 4}));
}

TEST_F(TakionNativeAvVectors, DecodeVersion10AudioWithoutATag) {
  const auto audio10 = TakionCodecRig::bytes({
      0x14,
      0x23,
      0x05,
      0x58,
      0x00,
      0x05,
      0x00,
      0x02,
      0x7f,
      0x0e,
      0x18,
      0xe0,
      0x80,
      0xee,
      0x20,
      0xf2,
      0xc0,
      0xb0,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x03, audio10, {0x0d, 0xa5}), TakionCodecRig::profile(10));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().packet_index, 0x1423);
  EXPECT_EQ(parsed.value().frame_index, 0x0558);
  EXPECT_EQ(parsed.value().unit_index, 0);
  EXPECT_EQ(parsed.value().units_in_frame_total, 6);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
  EXPECT_EQ(parsed.value().codec, 0x7f);
  EXPECT_FALSE(parsed.value().av_tag.has_value());
  EXPECT_FALSE(parsed.value().is_haptics);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({0x0d, 0xa5}));
  EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
      packet_with(0x03, audio10, {}), TakionCodecRig::profile(10)));
}

TEST_F(TakionNativeAvVectors, DecodeVersion11AudioWithItsTag) {
  const auto audio11 = TakionCodecRig::bytes({
      0xee, 0x4f, 0xcb, 0x79, 0x03, 0x05, 0x00, 0x02, 0x2b, 0x07,
      0x4b, 0xf0, 0x7f, 0x39, 0xfc, 0xe5, 0x25, 0xb3, 0x0d,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x03, audio11, {0xa5}), TakionCodecRig::profile(11));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().packet_index, 0xee4f);
  EXPECT_EQ(parsed.value().frame_index, 0xcb79);
  EXPECT_EQ(parsed.value().unit_index, 3);
  EXPECT_EQ(parsed.value().units_in_frame_total, 6);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
  EXPECT_EQ(parsed.value().codec, 0x2b);
  EXPECT_EQ(parsed.value().av_tag, std::optional<std::uint8_t>{0x0d});
  EXPECT_EQ(parsed.value().audio_profile, 0);
  EXPECT_EQ(parsed.value().audio_mode, 0);
  EXPECT_FALSE(parsed.value().is_haptics);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({0xa5}));
  EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
      packet_with(0x03, audio11, {}), TakionCodecRig::profile(11)));
  parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x03, audio11, {0xa5}), TakionCodecRig::profile(10));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed.value().av_tag.has_value());
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({0x0d, 0xa5}));
  auto haptic11 = audio11;
  haptic11.back() = std::byte{0x03};
  parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x03, haptic11, {0xa5}), TakionCodecRig::profile(11));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().is_haptics);
  EXPECT_EQ(parsed.value().av_tag, std::optional<std::uint8_t>{0x03});
  EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
      packet_with(0x13, audio11, {0xa5}), TakionCodecRig::profile(11)));
}

TEST_F(TakionNativeAvVectors, DecodeVersion10And11Video) {
  const auto video10 = TakionCodecRig::bytes({
      0xe5, 0x60, 0xb1, 0xa0, 0x00, 0x40, 0x14, 0x02, 0x38, 0xfb,
      0x92, 0x4d, 0xab, 0x99, 0xf4, 0xc8, 0xbc, 0xe7, 0xa4, 0x20,
  });
  const auto video11 = TakionCodecRig::bytes({
      0x00, 0xc5, 0x06, 0x26, 0x00, 0x20, 0x14, 0x02, 0x6b, 0x7a,
      0xd6, 0x91, 0x19, 0x56, 0x03, 0x0b, 0xbd, 0x74, 0xe7, 0xa0,
  });
  for (const auto version : {10U, 11U}) {
    const auto& video = version == 10 ? video10 : video11;
    auto parsed = takion::PacketCodec::decode_av_packet(
        packet_with(0x02, video, {1, 2, 3, 4}),
        TakionCodecRig::profile(version));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().unit_index, (version == 10 ? 2 : 1));
    EXPECT_EQ(parsed.value().units_in_frame_total, 6);
    EXPECT_EQ(parsed.value().units_in_frame_fec, 2);
    EXPECT_EQ(parsed.value().codec, (version == 10 ? 0x38 : 0x6b));
    EXPECT_EQ(parsed.value().video_word, (version == 10 ? 0xe7a4 : 0x74e7));
    EXPECT_EQ(parsed.value().adaptive_stream_index, (version == 10 ? 1 : 5));
    EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({1, 2, 3, 4}));
  }
}

TEST_F(TakionNativeAvVectors, DecodeVersion10And11VideoSlices) {
  const auto video10_extension = TakionCodecRig::bytes({
      0x97, 0xaf, 0x30, 0x6f, 0x00, 0x00, 0x14, 0x02, 0x67, 0xaa, 0x95, 0x29,
      0x3a, 0x8a, 0x28, 0xf8, 0x62, 0x54, 0xa4, 0x20, 0x00, 0x60, 0x10,
  });
  auto parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x12, video10_extension, {9}), TakionCodecRig::profile(10));
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed.value().slice.has_value());
  EXPECT_EQ(parsed.value().slice->index, 3);
  EXPECT_EQ(parsed.value().slice->count, 4);
  EXPECT_FALSE(parsed.value().slice->start);
  EXPECT_FALSE(parsed.value().slice->end);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({9}));
  const auto video11_extension = TakionCodecRig::bytes({
      0xeb, 0xb3, 0xf4, 0x69, 0x00, 0x00, 0x14, 0x02, 0x2e, 0xe4, 0x45, 0x8d,
      0xc7, 0x24, 0x06, 0xaa, 0x9b, 0x49, 0xca, 0x20, 0x00, 0x00, 0x12,
  });
  parsed = takion::PacketCodec::decode_av_packet(
      packet_with(0x12, video11_extension, {9}), TakionCodecRig::profile(11));
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed.value().slice.has_value());
  EXPECT_EQ(parsed.value().slice->index, 0);
  EXPECT_EQ(parsed.value().slice->count, 4);
  EXPECT_TRUE(parsed.value().slice->start);
  EXPECT_FALSE(parsed.value().slice->end);
}

TEST_F(TakionNativeAvVectors, DecodeVersions21To25) {
  const auto trailing = TakionCodecRig::bytes({0xc0, 0xc1, 0xc2, 0xc3});
  for (const auto& vector : native_av_vectors_21_to_25) {
    auto wire = TakionCodecRig::from_hex(vector.hex);
    wire.insert(wire.end(), trailing.begin(), trailing.end());
    const auto plain = takion::PacketCodec::decode_plain_packet(
        wire, TakionCodecRig::profile(vector.version));
    ASSERT_TRUE(plain.has_value());
    EXPECT_TRUE(plain.value().extended_header.has_value());
    const auto parsed = takion::PacketCodec::decode_av_packet(
        plain.value(), TakionCodecRig::profile(vector.version));
    ASSERT_TRUE(parsed.has_value());
    const auto& value = parsed.value();
    EXPECT_EQ(value.type.family(),
              (vector.video ? takion::PacketFamily::video
                            : takion::PacketFamily::audio_and_haptics));
    EXPECT_EQ(value.packet_index, vector.packet_index);
    EXPECT_EQ(value.frame_index, vector.frame_index);
    EXPECT_EQ(value.unit_index, vector.unit_index);
    EXPECT_EQ(value.units_in_frame_total, vector.total);
    EXPECT_EQ(value.units_in_frame_fec, vector.fec);
    EXPECT_EQ(value.codec, vector.codec);
    EXPECT_EQ(value.video_word, vector.video_word);
    EXPECT_EQ(value.adaptive_stream_index, vector.adaptive_stream_index);
    EXPECT_EQ(value.dynamic_range_flag, vector.dynamic_range_flag);
    EXPECT_EQ(value.suppress_presentation, vector.suppress_presentation);
    EXPECT_EQ(value.audio_mode, vector.audio_mode);
    EXPECT_EQ(value.data, trailing);
    if (vector.video) {
      EXPECT_EQ(value.slice, vector.slice);
    } else {
      EXPECT_EQ(value.audio_profile, vector.audio_profile);
    }
  }
}

TEST_F(TakionNativeAvVectors, DecodeVideoTotalsAndFlagsAtTheirBoundaries) {
  struct Boundary final {
    std::uint32_t version;
    const char* hex;
    std::uint16_t total;
    bool dynamic_range_flag;
    bool suppress_presentation;
  };
  constexpr Boundary boundaries[]{
      {15, "021234567800bffc03060000000000000000010241", 2048, true, false},
      {15, "021234567800a00003060000000000000000010241", 1, true, false},
      {15, "021234567800bff803060000000000000000010243", 2047, true, false},
      {12, "021234567800bffc03060000000000000000010243", 2048, false, false},
      {20, "0201020304050607081234567800bffc03060000000000000000010241", 2048,
       true, false},
      {21, "0201020304050607081234567800bffc03060000000000000000010241", 2048,
       true, false},
      {21, "0201020304050607081234567800a00003060000000000000000010243", 1,
       true, false},
      {23, "0201020304050607081234567800bffc03060000000000000000010241", 0,
       true, false},
      {23, "0201020304050607081234567800bff803060000000000000000010243", 2047,
       true, false},
      {23, "0201020304050607081234567800a00003060000000000000000010242", 1,
       false, false},
      {25, "0201020304050607081234567800bffc03060000000000000000010243", 0,
       true, true},
      {25, "0201020304050607081234567800a00003060000000000000000010243", 1,
       true, true},
      {25, "0201020304050607081234567800bff803060000000000000000010242", 2047,
       false, true},
      {25, "0201020304050607081234567800bff803060000000000000000010240", 2047,
       false, false},
  };
  const auto trailing = TakionCodecRig::bytes({0, 0, 0xab, 0xcd});
  for (const auto& boundary : boundaries) {
    auto wire = TakionCodecRig::from_hex(boundary.hex);
    wire.insert(wire.end(), trailing.begin(), trailing.end());
    const auto plain = takion::PacketCodec::decode_plain_packet(
        wire, TakionCodecRig::profile(boundary.version));
    ASSERT_TRUE(plain.has_value());
    if (boundary.version >= 20) {
      EXPECT_EQ(plain.value().extended_header->timestamp, 0x01020304);
      EXPECT_EQ(plain.value().extended_header->counter, 0x05060708);
    }
    const auto parsed = takion::PacketCodec::decode_av_packet(
        plain.value(), TakionCodecRig::profile(boundary.version));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().packet_index, 0x1234);
    EXPECT_EQ(parsed.value().frame_index, 0x5678);
    EXPECT_EQ(parsed.value().unit_index, 5);
    EXPECT_EQ(parsed.value().units_in_frame_fec, 3);
    EXPECT_EQ(parsed.value().video_word, 0x0102);
    EXPECT_EQ(parsed.value().adaptive_stream_index, 2);
    EXPECT_EQ(parsed.value().units_in_frame_total, boundary.total);
    EXPECT_EQ(parsed.value().dynamic_range_flag, boundary.dynamic_range_flag);
    EXPECT_EQ(parsed.value().suppress_presentation,
              boundary.suppress_presentation);
    EXPECT_EQ(parsed.value().data, trailing);
  }
}

}  // namespace
}  // namespace takion_test
