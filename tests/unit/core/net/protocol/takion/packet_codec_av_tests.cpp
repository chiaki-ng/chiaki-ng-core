#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionAvPackets : public ::testing::Test {
 protected:
  static auto packet(std::uint8_t type, const std::vector<std::byte>& payload)
      -> takion::PlainPacket {
    return takion::PlainPacket{takion::PacketType{type}, std::nullopt, payload};
  }

  static auto decode_v9(std::uint8_t type,
                        const std::vector<std::byte>& payload) {
    return takion::PacketCodec::decode_av_packet(packet(type, payload),
                                                 TakionCodecRig::profile(9));
  }

  static auto av_payload(bool is_video, bool nalu, bool audio_tag,
                         std::size_t data_size, std::uint32_t packed = 0)
      -> std::vector<std::byte> {
    std::vector<std::byte> payload((is_video ? 20U : 18U) +
                                       (nalu ? (is_video ? 3U : 1U) : 0U) +
                                       (audio_tag ? 1U : 0U) + data_size,
                                   std::byte{0x5a});
    payload[4] = static_cast<std::byte>(packed >> 24);
    payload[5] = static_cast<std::byte>(packed >> 16);
    payload[6] = static_cast<std::byte>(packed >> 8);
    payload[7] = static_cast<std::byte>(packed);
    return payload;
  }

  static auto decode(std::uint32_t version, bool is_video, bool nalu,
                     const std::vector<std::byte>& payload) {
    const auto type =
        static_cast<std::uint8_t>((is_video ? 2U : 3U) | (nalu ? 0x10U : 0U));
    return takion::PacketCodec::decode_av_packet(
        takion::PlainPacket{takion::PacketType{type}, std::nullopt, payload},
        TakionCodecRig::profile(version));
  }

  static auto with_nalu(std::vector<std::byte> payload)
      -> std::vector<std::byte> {
    payload.insert(payload.begin() + 20,
                   {std::byte{1}, std::byte{2}, std::byte{3}});
    return payload;
  }

  const std::vector<std::byte> video = TakionCodecRig::bytes({
      0x00, 0x11, 0x22, 0x33, 0x00, 0x20, 0x08, 0x01, 0x3c, 0,    0,    0,
      0,    0,    0,    0,    0,    0xab, 0xcd, 0xc0, 0xde, 0xad, 0xbe, 0xef,
  });
  const std::vector<std::byte> audio = TakionCodecRig::bytes({
      0x00, 0x11, 0x22, 0x33, 0x01, 0x02, 0x00, 0x01, 0x05, 0,
      0,    0,    0,    0,    0,    0,    0,    0,    0xbe, 0xef,
  });
};

TEST_F(TakionAvPackets, DecodeAVersion9VideoUnit) {
  const auto parsed = decode_v9(0x02, video);
  ASSERT_TRUE(parsed.has_value());
  const auto video_packet = packet(0x02, video);
  const auto header = takion::PacketCodec::decode_av_header(
      video_packet, TakionCodecRig::profile(9));
  ASSERT_TRUE(header.has_value());
  EXPECT_TRUE(header.value().values.data.empty());
  EXPECT_TRUE(std::ranges::equal(header.value().data, parsed.value().data));
  EXPECT_EQ(header.value().data.data() + header.value().data.size(),
            video.data() + video.size());
  EXPECT_EQ(header.value().values.frame_index, parsed.value().frame_index);
  EXPECT_EQ(parsed.value().packet_index, 0x11);
  EXPECT_EQ(parsed.value().frame_index, 0x2233);
  EXPECT_EQ(parsed.value().unit_index, 1);
  EXPECT_EQ(parsed.value().units_in_frame_total, 3);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 1);
  EXPECT_EQ(parsed.value().codec, 0x3c);
  EXPECT_EQ(parsed.value().video_word, 0xabcd);
  EXPECT_EQ(parsed.value().adaptive_stream_index, 6);
  EXPECT_EQ(parsed.value().data,
            TakionCodecRig::bytes({0xde, 0xad, 0xbe, 0xef}));
}

TEST_F(TakionAvPackets, DecodeAVersion9AudioUnit) {
  const auto parsed = decode_v9(0x03, audio);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().unit_index, 1);
  EXPECT_EQ(parsed.value().units_in_frame_total, 3);
  EXPECT_EQ(parsed.value().units_in_frame_fec, 1);
  EXPECT_EQ(parsed.value().codec, 5);
  EXPECT_EQ(parsed.value().data, TakionCodecRig::bytes({0xbe, 0xef}));
}

TEST_F(TakionAvPackets, ReadTheWholePackedAudioFecCount) {
  auto packed_audio = audio;
  packed_audio[6] = std::byte{0x12};
  const auto parsed = decode_v9(0x03, packed_audio);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().units_in_frame_fec, 0x1201);
}

TEST_F(TakionAvPackets, SkipVideoNaluInfoStructs) {
  const auto parsed = decode_v9(0x12, with_nalu(video));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().uses_nalu_info_structs);
  EXPECT_EQ(parsed.value().data,
            TakionCodecRig::bytes({0xde, 0xad, 0xbe, 0xef}));
}

TEST_F(TakionAvPackets, RefuseTruncatedVersion9Headers) {
  for (std::size_t length = 0; length < 24; ++length) {
    const auto prefix = std::vector<std::byte>(
        video.begin(), video.begin() + static_cast<std::ptrdiff_t>(length));
    const auto result = decode_v9(0x02, prefix);
    ASSERT_FALSE(result) << "video length " << length;
    EXPECT_EQ(result.error().reason, takion::ParseFailureReason::truncated);
  }
  for (std::size_t length = 0; length < 19; ++length) {
    const auto prefix = std::vector<std::byte>(
        audio.begin(), audio.begin() + static_cast<std::ptrdiff_t>(length));
    const auto result = decode_v9(0x03, prefix);
    ASSERT_FALSE(result) << "audio length " << length;
    EXPECT_EQ(result.error().reason, takion::ParseFailureReason::truncated);
  }
  const auto nalu = with_nalu(video);
  for (std::size_t length = 20; length < 24; ++length) {
    const auto prefix = std::vector<std::byte>(
        nalu.begin(), nalu.begin() + static_cast<std::ptrdiff_t>(length));
    EXPECT_FALSE(decode_v9(0x12, prefix)) << "nalu length " << length;
  }
}

TEST_F(TakionAvPackets, PassOutOfRangeUnitCountsThrough) {
  auto invalid = video;
  invalid[4] = std::byte{0x00};
  invalid[5] = std::byte{0x60};
  auto parsed = decode_v9(0x02, invalid);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().unit_index, 3);
  EXPECT_EQ(parsed.value().units_in_frame_total, 3);
  invalid = video;
  invalid[7] = std::byte{0x03};
  parsed = decode_v9(0x02, invalid);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().units_in_frame_fec, 3);
  EXPECT_EQ(parsed.value().units_in_frame_total, 3);
}

TEST_F(TakionAvPackets, DecodeOnlyAvFamiliesButAtAnyVersion) {
  EXPECT_FALSE(decode_v9(0x05, video));
  EXPECT_TRUE(takion::PacketCodec::decode_av_packet(packet(0x02, video),
                                                    TakionCodecRig::profile(7))
                  .has_value());
  EXPECT_TRUE(takion::PacketCodec::decode_av_packet(packet(0x02, video),
                                                    TakionCodecRig::profile(12))
                  .has_value());
}

TEST_F(TakionAvPackets, FollowTheReferenceLayoutForEveryVersionAndKind) {
  for (const auto version : {9U, 10U, 11U, 12U, 15U, 20U}) {
    for (const bool is_video : {false, true}) {
      for (const bool nalu : {false, true}) {
        for (const bool high_tag : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << "version " << version << " video " << is_video
                       << " nalu " << nalu << " high tag " << high_tag);
          if (!is_video && nalu && version < 15) {
            std::vector<std::byte> rejected(40, std::byte{});
            takion::PlainPacket refused{takion::PacketType{0x13}, {}, rejected};
            const auto decoded = takion::PacketCodec::decode_av_packet(
                refused, TakionCodecRig::profile(version));
            ASSERT_FALSE(decoded);
            EXPECT_EQ(decoded.error().reason,
                      takion::ParseFailureReason::unsupported_layout);
            continue;
          }
          const std::size_t header = (is_video ? 20U : 18U) +
                                     (nalu ? (is_video ? 3U : 1U) : 0U) +
                                     (!is_video && version >= 11 ? 1U : 0U);
          std::vector<std::byte> payload(header, std::byte{});
          payload[1] = std::byte{0x11};
          payload[3] = std::byte{0x22};
          payload[8] = is_video ? std::byte{0x3c} : std::byte{5};
          payload[16] = std::byte{0x40};
          if (!is_video && version >= 11)
            payload[header - 1] = high_tag ? std::byte{0xe2} : std::byte{2};
          const auto data = TakionCodecRig::bytes({0, 1, 0xab, 0xcd});
          payload.insert(payload.end(), data.begin(), data.end());
          const auto type = static_cast<std::uint8_t>((is_video ? 2U : 3U) |
                                                      (nalu ? 0x10U : 0U));
          takion::PlainPacket unit{takion::PacketType{type}, {}, payload};
          const auto decoded = takion::PacketCodec::decode_av_packet(
              unit, TakionCodecRig::profile(version));
          ASSERT_TRUE(decoded.has_value());
          EXPECT_EQ(decoded.value().data, data);
          EXPECT_EQ(decoded.value().packet_index, 0x11);
          EXPECT_EQ(decoded.value().frame_index, 0x22);
          EXPECT_EQ(decoded.value().unit_index, 0);
          EXPECT_EQ(decoded.value().units_in_frame_total, 1);
          EXPECT_EQ(decoded.value().is_haptics,
                    !is_video && version >= 11 && (!high_tag || version >= 15));
          for (std::size_t length = 0; length <= header; ++length) {
            if (!is_video && nalu && length == header) continue;
            unit.payload = std::span{payload}.first(length);
            EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
                unit, TakionCodecRig::profile(version)))
                << "length " << length;
          }
        }
      }
    }
  }
}

TEST_F(TakionAvPackets, AcceptExactlyTheReferenceDataBounds) {
  for (const auto version : {9U, 10U, 11U, 12U, 15U, 20U}) {
    SCOPED_TRACE(testing::Message() << "version " << version);
    for (std::size_t data = 0; data < 4; ++data)
      EXPECT_FALSE(
          decode(version, true, false, av_payload(true, false, false, data)))
          << "data " << data;
    auto accepted =
        decode(version, true, false, av_payload(true, false, false, 4));
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().data.size(), 4U);
    EXPECT_FALSE(decode(version, true, true, av_payload(true, true, false, 0)));
    accepted = decode(version, true, true, av_payload(true, true, false, 1));
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().data.size(), 1U);

    const bool tag = version >= 11;
    EXPECT_FALSE(
        decode(version, false, false, av_payload(false, false, tag, 0)));
    accepted = decode(version, false, false, av_payload(false, false, tag, 1));
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().data.size(), 1U);
    if (version >= 15) {
      accepted = decode(version, false, true, av_payload(false, true, tag, 0));
      ASSERT_TRUE(accepted);
      EXPECT_TRUE(accepted.value().data.empty());
      auto underflow = av_payload(false, true, tag, 0);
      underflow.pop_back();
      EXPECT_FALSE(decode(version, false, true, underflow));
    } else {
      EXPECT_FALSE(
          decode(version, false, true, av_payload(false, true, tag, 4)));
    }
  }
}

TEST_F(TakionAvPackets, MaskOverflowingUnitFieldsLikeTheReference) {
  for (const auto version : {9U, 10U, 11U, 12U, 15U, 20U}) {
    SCOPED_TRACE(testing::Message() << "version " << version);
    const bool tag = version >= 11;
    const std::uint32_t video_overflow = 3U | (1U << 10U) | (5U << 21U);
    auto accepted = decode(version, true, false,
                           av_payload(true, false, false, 4, video_overflow));
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted.value().unit_index, 5);
    EXPECT_EQ(accepted.value().units_in_frame_total, 2);
    EXPECT_EQ(accepted.value().units_in_frame_fec, 3);
    const std::uint32_t audio_overflow = 7U | (0U << 16U) | (9U << 24U);
    accepted = decode(version, false, false,
                      av_payload(false, false, tag, 1, audio_overflow));
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted.value().unit_index, 9);
    EXPECT_EQ(accepted.value().units_in_frame_total, 1);
  }
}

TEST_F(TakionAvPackets, AcceptEmptyDataAtVersion7) {
  for (const bool is_video : {false, true}) {
    for (const bool nalu : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "video " << is_video << " nalu " << nalu);
      auto payload = av_payload(is_video, nalu, false, 0);
      if (!is_video) payload.pop_back();
      if (!is_video && nalu) payload.insert(payload.end(), 2, std::byte{0x5a});
      const auto accepted = decode(7, is_video, nalu, payload);
      ASSERT_TRUE(accepted);
      EXPECT_TRUE(accepted.value().data.empty());
      payload.pop_back();
      EXPECT_FALSE(decode(7, is_video, nalu, payload));
    }
  }
}

TEST_F(TakionAvPackets, RefuseTheFuzzedAudioReproduction) {
  const auto repro = TakionCodecRig::bytes(
      {0x33, 0xea, 0x28, 0x58, 0x73, 0x03, 0x06, 0x00, 0x03, 0xbe,
       0xa5, 0xea, 0xcf, 0xf4, 0x16, 0x10, 0x3e, 0xd5, 0xf0, 0xf9});
  const auto plain = takion::PacketCodec::decode_plain_packet(
      repro, TakionCodecRig::profile(9));
  ASSERT_TRUE(plain.has_value());
  EXPECT_FALSE(takion::PacketCodec::decode_av_packet(
      plain.value(), TakionCodecRig::profile(9)));
}

}  // namespace
}  // namespace takion_test
