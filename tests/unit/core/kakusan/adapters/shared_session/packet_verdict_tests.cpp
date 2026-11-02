#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/shared_session/packet_verdict.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::Admission;
using libchiaking::kakusan::PacketVerdict;
using libchiaking::net::kakusan::MediaKind;

class PacketVerdictTest : public ::testing::Test {
 protected:
  static constexpr std::uint64_t base_position = 44992;

  static void put_u32(std::vector<std::byte>& bytes, std::size_t offset,
                      std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      bytes[offset + index] =
          static_cast<std::byte>((value >> (24U - 8U * index)) & 0xffU);
  }

  static constexpr auto shift(std::uint32_t version) noexcept -> std::size_t {
    return version == 20 ? 8U : 0U;
  }

  struct VideoSpec {
    std::uint32_t version{12};
    std::size_t size{40};
    std::uint32_t index{0};
    std::uint32_t total{1};
    std::uint32_t fec{0};
    std::uint8_t profile{0};
    bool nalu{false};
    std::uint64_t position{base_position};
  };

  static auto video_packet(const VideoSpec& spec) -> std::vector<std::byte> {
    const auto extra = shift(spec.version);
    std::vector<std::byte> bytes(spec.size + extra, std::byte{0x5a});
    bytes[0] = static_cast<std::byte>(spec.nalu ? 0x12U : 0x02U);
    if (bytes.size() >= extra + 18) {
      put_u32(bytes, extra + 5,
              (spec.index << 21U) | ((spec.total - 1U) << 10U) | spec.fec);
      put_u32(bytes, extra + 14,
              static_cast<std::uint32_t>(spec.position & 0xffffffffU));
    }
    if (bytes.size() > extra + 20)
      bytes[extra + 20] = static_cast<std::byte>(spec.profile << 5U);
    return bytes;
  }

  struct AudioSpec {
    std::uint32_t version{12};
    bool nalu{false};
    std::uint8_t tag{0};
    std::uint32_t total{1};
    std::uint32_t redundant{0};
    std::uint32_t source{1};
    std::uint32_t unit_size{4};
    std::uint8_t codec{5};
    std::ptrdiff_t payload_adjust{0};
    std::uint64_t position{base_position};
  };

  static auto audio_header_size(const AudioSpec& spec) -> std::size_t {
    return shift(spec.version) + 19U +
           (spec.nalu ? (spec.version >= 15 ? 1U : 3U) : 0U) +
           (spec.version >= 12 ? 1U : 0U);
  }

  static auto audio_packet(const AudioSpec& spec) -> std::vector<std::byte> {
    const auto payload = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(spec.unit_size * spec.total) +
        spec.payload_adjust);
    std::vector<std::byte> bytes(audio_header_size(spec) + payload,
                                 std::byte{0x77});
    bytes[0] = static_cast<std::byte>(spec.nalu ? 0x13U : 0x03U);
    const auto extra = shift(spec.version);
    put_u32(bytes, extra + 5,
            spec.version >= 15
                ? ((spec.total - 1U) << 16U) | spec.source
                : ((spec.total - 1U) << 16U) | (spec.unit_size << 8U) |
                      (spec.redundant << 4U) | spec.source);
    bytes[extra + 9] = static_cast<std::byte>(spec.codec);
    put_u32(bytes, extra + 14,
            static_cast<std::uint32_t>(spec.position & 0xffffffffU));
    bytes[extra + 18] = std::byte{};
    if (spec.version >= 12)
      bytes[audio_header_size(spec) - 1] = static_cast<std::byte>(spec.tag);
    return bytes;
  }

  static auto classify(std::uint32_t version,
                       libchiaking::play::SharedDatagram::Media media,
                       std::uint64_t position,
                       const std::vector<std::byte>& bytes,
                       std::size_t profiles = 1) -> PacketVerdict {
    return PacketVerdict::classify(
        profiles,
        SourceDescriptions::describe(version, media, position, bytes));
  }

  static auto video(std::uint32_t version, VideoSpec spec,
                    std::size_t profiles = 1) -> PacketVerdict {
    spec.version = version;
    return classify(version, libchiaking::play::SharedDatagram::Media::video,
                    spec.position, video_packet(spec), profiles);
  }

  static auto audio(const AudioSpec& spec) -> PacketVerdict {
    return classify(spec.version,
                    libchiaking::play::SharedDatagram::Media::audio,
                    spec.position, audio_packet(spec));
  }

  static constexpr PacketVerdict forward_video{Admission::forward,
                                               MediaKind::video};
  static constexpr PacketVerdict reject_video{Admission::reject,
                                              MediaKind::video};
  static constexpr PacketVerdict forward_audio{Admission::forward,
                                               MediaKind::audio};
  static constexpr PacketVerdict reject_audio{Admission::reject,
                                              MediaKind::audio};
  static constexpr PacketVerdict exclude_audio{Admission::exclude,
                                               MediaKind::audio};
};

TEST_F(PacketVerdictTest, ForwardsEveryLayout) {
  for (const std::uint32_t version : {9U, 10U, 12U, 14U, 15U, 19U, 20U}) {
    EXPECT_EQ(video(version, VideoSpec{}), forward_video);
    AudioSpec spec;
    spec.version = version;
    EXPECT_EQ(audio(spec), forward_audio);
    spec.total = 3;
    spec.source = 2;
    spec.redundant = 1;
    EXPECT_EQ(audio(spec), forward_audio);
  }
  for (const std::uint32_t version : {0U, 8U, 21U, 4096U}) {
    EXPECT_EQ(video(version, VideoSpec{}), reject_video);
    EXPECT_EQ(classify(version, libchiaking::play::SharedDatagram::Media::audio,
                       base_position, audio_packet(AudioSpec{})),
              reject_audio);
  }
}

TEST_F(PacketVerdictTest, RejectsUndescribedPackets) {
  const auto bytes = video_packet(VideoSpec{});
  libchiaking::play::SharedDatagram bare{
      libchiaking::play::SharedDatagram::Media::video, base_position, bytes};
  EXPECT_EQ(PacketVerdict::classify(1, bare), reject_video);
  bare.sealed_size = bytes.size();
  bare.unit_total = 1;
  EXPECT_EQ(PacketVerdict::classify(1, bare), reject_video);
  bare.sealed_size = bytes.size() - 1;
  EXPECT_EQ(PacketVerdict::classify(1, bare), forward_video);
  bare.unit_total = 0;
  EXPECT_EQ(PacketVerdict::classify(1, bare), reject_video);
}

TEST_F(PacketVerdictTest, ChecksCountOnlyAudio) {
  for (const std::uint32_t version : {15U, 20U}) {
    AudioSpec spec;
    spec.version = version;
    spec.total = 4;
    spec.source = 2;
    EXPECT_EQ(audio(spec), forward_audio);
    spec.source = 0;
    EXPECT_EQ(audio(spec), reject_audio);
    spec.source = 5;
    EXPECT_EQ(audio(spec), reject_audio);
    spec.source = 4;
    EXPECT_EQ(audio(spec), forward_audio);
    spec.payload_adjust = 1;
    EXPECT_EQ(audio(spec), reject_audio);
    spec.payload_adjust = 0;
    spec.codec = 4;
    EXPECT_EQ(audio(spec), reject_audio);
    spec.codec = 5;
    for (unsigned tag = 0; tag < 256; ++tag) {
      spec.tag = static_cast<std::uint8_t>(tag);
      const auto kind = tag & 0x0fU;
      EXPECT_EQ(audio(spec),
                (kind >= 2 && kind <= 5 ? exclude_audio : forward_audio));
    }
  }
}

TEST_F(PacketVerdictTest, BoundsThePacketLength) {
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position, {}),
            reject_video);
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, {}),
            reject_audio);
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position, std::vector<std::byte>(1, std::byte{0x02})),
            reject_video);
  EXPECT_EQ(
      classify(12, libchiaking::play::SharedDatagram::Media::audio,
               base_position, std::vector<std::byte>(17, std::byte{0x03})),
      reject_audio);

  VideoSpec spec;
  spec.size = 8168;
  EXPECT_EQ(video(12, spec), forward_video);
  EXPECT_EQ(video(9, spec), forward_video);
  spec.size = 8169;
  EXPECT_EQ(video(12, spec), reject_video);
  EXPECT_EQ(video(9, spec), reject_video);
  spec.size = 8167;
  EXPECT_EQ(video(12, spec), forward_video);

  AudioSpec audio_spec;
  audio_spec.total = 30;
  audio_spec.source = 15;
  audio_spec.redundant = 15;
  audio_spec.unit_size = 255;
  EXPECT_EQ(audio(audio_spec), forward_audio);
  EXPECT_EQ(audio_packet(audio_spec).size(), 20U + 7650U);
  audio_spec.payload_adjust = 1;
  EXPECT_EQ(audio(audio_spec), reject_audio);

  auto oversized = audio_packet(AudioSpec{});
  oversized.resize(8169, std::byte{0x77});
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, oversized),
            reject_audio);
}

TEST_F(PacketVerdictTest, MatchesThePacketFamilyToTheMedia) {
  auto bytes = video_packet(VideoSpec{});
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, bytes),
            reject_audio);
  bytes = audio_packet(AudioSpec{});
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position, bytes),
            reject_video);
  for (const unsigned type :
       {0x00U, 0x01U, 0x04U, 0x05U, 0x0fU, 0x10U, 0x11U, 0x14U}) {
    auto altered = video_packet(VideoSpec{});
    altered[0] = static_cast<std::byte>(type);
    EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                       base_position, altered),
              reject_video);
  }
  for (const unsigned type : {0x20U, 0x40U, 0x80U, 0xe0U}) {
    auto flagged = video_packet(VideoSpec{});
    flagged[0] = static_cast<std::byte>(0x02U | type);
    EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                       base_position, flagged),
              forward_video);
  }
}

TEST_F(PacketVerdictTest, BindsTheKeyPosition) {
  const auto bytes = video_packet(VideoSpec{});
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position + (1ULL << 32U), bytes),
            forward_video);
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position + (5ULL << 32U), bytes),
            forward_video);

  VideoSpec high;
  high.position = (1ULL << 32U) + 32;
  EXPECT_EQ(video(12, high), forward_video);
  high.position = std::numeric_limits<std::uint64_t>::max() - 15 - 40;
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     high.position, video_packet(high)),
            reject_video);
  high.position = std::numeric_limits<std::uint64_t>::max() - 16 - 40;
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     high.position, video_packet(high)),
            forward_video);
  high.position = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     high.position, video_packet(high)),
            reject_video);
}

TEST_F(PacketVerdictTest, BoundsTheVideoUnitIndices) {
  for (const std::uint32_t version : {9U, 12U}) {
    VideoSpec spec;
    spec.total = 4;
    spec.index = 3;
    spec.fec = 3;
    EXPECT_EQ(video(version, spec), forward_video);
    spec.index = 4;
    EXPECT_EQ(video(version, spec), reject_video);
    spec.index = 3;
    spec.fec = 4;
    EXPECT_EQ(video(version, spec), reject_video);
    spec.fec = 0;
    EXPECT_EQ(video(version, spec), forward_video);

    VideoSpec widest;
    widest.total = 2048;
    widest.index = 2047;
    widest.fec = 1023;
    EXPECT_EQ(video(version, widest), forward_video);
    widest.total = 1;
    widest.index = 0;
    widest.fec = 1;
    EXPECT_EQ(video(version, widest), reject_video);
    widest.fec = 0;
    EXPECT_EQ(video(version, widest), forward_video);
    widest.total = 1024;
    widest.fec = 1023;
    EXPECT_EQ(video(version, widest), forward_video);
    widest.total = 1023;
    EXPECT_EQ(video(version, widest), reject_video);
  }
}

TEST_F(PacketVerdictTest, NeedsAKnownVideoProfile) {
  for (std::uint8_t profile = 0; profile < 8; ++profile) {
    VideoSpec spec;
    spec.profile = profile;
    for (std::size_t count = 1; count <= 8; ++count)
      EXPECT_EQ(video(12, spec, count),
                (profile < count ? forward_video : reject_video));
  }
  VideoSpec spec;
  EXPECT_EQ(video(12, spec, 0), reject_video);
}

TEST_F(PacketVerdictTest, BoundsTheVideoPayload) {
  VideoSpec spec;
  spec.size = 24;
  EXPECT_EQ(video(12, spec), reject_video);
  spec.size = 25;
  EXPECT_EQ(video(12, spec), forward_video);
  spec.nalu = true;
  spec.size = 24;
  EXPECT_EQ(video(12, spec), reject_video);
  spec.size = 25;
  EXPECT_EQ(video(12, spec), forward_video);
  spec.nalu = false;
  spec.size = 21;
  EXPECT_EQ(video(12, spec), reject_video);
  spec.size = 20;
  EXPECT_EQ(video(12, spec), reject_video);
  spec.size = 18;
  EXPECT_EQ(video(12, spec), reject_video);
}

TEST_F(PacketVerdictTest, ReadsEachAudioLayoutVersion) {
  AudioSpec nine;
  nine.version = 9;
  nine.unit_size = 1;
  EXPECT_EQ(audio_packet(nine).size(), 20);
  EXPECT_EQ(audio(nine), forward_audio);
  auto bytes = audio_packet(nine);
  bytes.pop_back();
  EXPECT_EQ(classify(9, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, bytes),
            reject_audio);

  AudioSpec twelve;
  twelve.version = 12;
  twelve.unit_size = 1;
  EXPECT_EQ(audio_packet(twelve).size(), 21);
  EXPECT_EQ(audio(twelve), forward_audio);
  bytes = audio_packet(twelve);
  bytes.pop_back();
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, bytes),
            reject_audio);

  nine.nalu = true;
  EXPECT_EQ(audio(nine), reject_audio);
  twelve.nalu = true;
  EXPECT_EQ(audio(twelve), reject_audio);

  AudioSpec fifteen;
  fifteen.version = 15;
  fifteen.unit_size = 1;
  fifteen.nalu = true;
  EXPECT_EQ(audio_packet(fifteen).size(), 22);
  EXPECT_EQ(audio(fifteen), forward_audio);
  bytes = audio_packet(fifteen);
  bytes.pop_back();
  EXPECT_EQ(classify(15, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, bytes),
            reject_audio);

  AudioSpec reinterpreted;
  reinterpreted.version = 9;
  auto payload_first = audio_packet(reinterpreted);
  payload_first[19] = std::byte{2};
  EXPECT_EQ(classify(9, libchiaking::play::SharedDatagram::Media::audio,
                     base_position, payload_first),
            forward_audio);
}

TEST_F(PacketVerdictTest, ExcludesHapticsAudio) {
  for (const bool nalu : {false, true}) {
    for (unsigned tag = 0; tag < 256; ++tag) {
      AudioSpec spec;
      spec.nalu = nalu;
      spec.tag = static_cast<std::uint8_t>(tag);
      const auto expected = nalu                   ? reject_audio
                            : tag >= 2 && tag <= 5 ? exclude_audio
                                                   : forward_audio;
      EXPECT_EQ(audio(spec), expected);
      spec.version = 15;
      const auto kind = tag & 0x0fU;
      EXPECT_EQ(audio(spec),
                (kind >= 2 && kind <= 5 ? exclude_audio : forward_audio));
    }
  }
  AudioSpec spec;
  spec.tag = 2;
  spec.payload_adjust = 3;
  EXPECT_EQ(audio(spec), exclude_audio);
  spec.codec = 4;
  EXPECT_EQ(audio(spec), exclude_audio);
  EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                     base_position, audio_packet(spec)),
            reject_video);
}

TEST_F(PacketVerdictTest, ChecksTheAudioUnits) {
  AudioSpec spec;
  EXPECT_EQ(audio(spec), forward_audio);

  spec.codec = 4;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.codec = 6;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.codec = 0;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.codec = 5;

  spec.unit_size = 0;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.unit_size = 1;
  EXPECT_EQ(audio(spec), forward_audio);
  spec.unit_size = 255;
  EXPECT_EQ(audio(spec), forward_audio);
  spec.unit_size = 4;

  spec.source = 0;
  spec.total = 1;
  spec.redundant = 1;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.total = 1;
  spec.redundant = 0;
  EXPECT_EQ(audio(spec), reject_audio);

  spec.source = 2;
  spec.redundant = 1;
  spec.total = 3;
  EXPECT_EQ(audio(spec), forward_audio);
  spec.total = 2;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.total = 4;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.total = 3;
  spec.payload_adjust = 1;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.payload_adjust = -1;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.payload_adjust =
      -static_cast<std::ptrdiff_t>(spec.unit_size * spec.total);
  EXPECT_EQ(audio(spec), reject_audio);
  spec.payload_adjust = 0;

  spec.total = 2;
  spec.source = 1;
  spec.redundant = 1;
  EXPECT_EQ(audio(spec), forward_audio);
  spec.source = 15;
  spec.redundant = 15;
  spec.total = 30;
  EXPECT_EQ(audio(spec), forward_audio);
  spec.total = 31;
  EXPECT_EQ(audio(spec), reject_audio);
  spec.total = 256;
  spec.source = 15;
  spec.redundant = 15;
  EXPECT_EQ(audio(spec), reject_audio);
}

TEST_F(PacketVerdictTest, ForwardsTheFixturePackets) {
  const auto document = libchiaking::json::strict::Value::parse(
      test_support::kakusan::Fixtures::text("wire-vectors.json"));
  ASSERT_TRUE(document);
  const auto& packets = *test_support::kakusan::Fixtures::member(
                             document.value(), "source_packets")
                             .array();
  EXPECT_EQ(packets.size(), 3U);
  for (const auto& packet : packets) {
    const auto bytes =
        test_support::Bytes::unhex(test_support::kakusan::Fixtures::string_of(
            test_support::kakusan::Fixtures::member(packet, "datagram_hex")));
    const auto position =
        *test_support::kakusan::Fixtures::member(packet, "key_position")
             .integer();
    EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::audio,
                       position, bytes),
              forward_audio);
    EXPECT_EQ(classify(12, libchiaking::play::SharedDatagram::Media::video,
                       position, bytes),
              reject_video);
    EXPECT_EQ(classify(9, libchiaking::play::SharedDatagram::Media::audio,
                       position, bytes),
              reject_audio);
  }
}

}  // namespace
}  // namespace kakusan_test
