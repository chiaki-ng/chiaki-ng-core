#include "tests/unit/core/net/protocol/takion/client_inbound_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

class InboundRecordingTap final : public takion::SourceTap {
 public:
  struct Seen final {
    takion::SourceMedia media{};
    std::uint64_t key_position{};
    std::vector<std::byte> bytes;
    takion::SourceDatagram facts{};
  };

  std::vector<Seen> datagrams;
  std::size_t key_callbacks{};

  void on_source_keys(std::uint32_t,
                      const takion::SourceReceiveKeys&) noexcept override {
    ++key_callbacks;
  }

  void on_source_datagram(
      const takion::SourceDatagram& datagram) noexcept override {
    datagrams.push_back(Seen{
        datagram.media, datagram.key_position,
        std::vector<std::byte>(datagram.bytes.begin(), datagram.bytes.end()),
        datagram});
    datagrams.back().facts.bytes = {};
  }
};

class TakionInboundSourceTap : public ::testing::Test {
 protected:
  static auto video() -> std::vector<std::byte> {
    std::vector<std::byte> bytes(20 + 4, std::byte{});
    bytes[8] = std::byte{0x3c};
    std::ranges::copy(plain, bytes.begin() + 20);
    return bytes;
  }

  static auto short_video() -> std::vector<std::byte> {
    std::vector<std::byte> bytes(20 + 3, std::byte{0x33});
    bytes[4] = std::byte{0};
    bytes[5] = std::byte{0};
    bytes[6] = std::byte{0};
    bytes[7] = std::byte{0};
    return bytes;
  }

  static auto layout_audio(bool tagged, bool count_only, std::uint8_t tag,
                           std::uint8_t source) -> std::vector<std::byte> {
    const std::size_t header = tagged ? 19 : 18;
    std::vector<std::byte> bytes(header + 8, std::byte{0x22});
    bytes[4] = std::byte{0x00};
    bytes[5] = std::byte{0x01};
    bytes[6] = count_only ? std::byte{0x00} : std::byte{0x04};
    bytes[7] = count_only ? static_cast<std::byte>(source)
                          : static_cast<std::byte>(0x10U | source);
    bytes[8] = std::byte{0x05};
    if (tagged) bytes[18] = static_cast<std::byte>(tag);
    return bytes;
  }

  static inline const std::vector<std::byte> plain{std::byte{1}, std::byte{2},
                                                   std::byte{3}, std::byte{4}};

  InboundRecordingTap tap;
};

TEST_F(TakionInboundSourceTap, SeesAuthenticatedOriginalDatagrams) {
  InboundKeyedReceiver keyed{9};
  keyed.harness.config.source_tap = &tap;
  const auto wire =
      keyed.protect(takion::PacketFamily::video, video(), 0x1234, plain.size());
  keyed.receive(wire);
  ASSERT_EQ(tap.datagrams.size(), 1U);
  EXPECT_EQ(tap.datagrams[0].media, takion::SourceMedia::video);
  EXPECT_EQ(tap.datagrams[0].key_position, 0x1234U);
  EXPECT_EQ(tap.datagrams[0].bytes, wire);
  const auto* unit =
      keyed.harness.events.first<takion::client_event::AvUnitReceived>();
  ASSERT_NE(unit, nullptr);
  EXPECT_EQ(unit->values.data, plain);
  EXPECT_FALSE(std::equal(plain.begin(), plain.end(), wire.end() - 4));
}

TEST_F(TakionInboundSourceTap, SkipsForgedAndUnparsedDatagrams) {
  InboundKeyedReceiver keyed{9};
  keyed.harness.config.source_tap = &tap;
  keyed.receive(keyed.protect(takion::PacketFamily::video, video(), 0x1234,
                              plain.size()));
  ASSERT_EQ(tap.datagrams.size(), 1U);

  auto forged =
      keyed.protect(takion::PacketFamily::video, video(), 0x2000, plain.size());
  forged[forged.size() - 1] ^= std::byte{1};
  keyed.receive(std::move(forged));
  EXPECT_EQ(tap.datagrams.size(), 1U);

  keyed.receive(
      keyed.protect(takion::PacketFamily::video, short_video(), 0x3000));
  EXPECT_EQ(tap.datagrams.size(), 1U);
  EXPECT_EQ(tap.key_callbacks, 0U);
}

TEST_F(TakionInboundSourceTap, IsOptional) {
  InboundKeyedReceiver untapped{9};
  untapped.receive(untapped.protect(takion::PacketFamily::video, video(),
                                    0x1234, plain.size()));
  EXPECT_EQ(untapped.harness.events.events.size(), 1U);
}

TEST_F(TakionInboundSourceTap, DescribesUnitsAtEveryLayout) {
  for (const std::uint32_t version : {9U, 12U, 15U, 20U}) {
    InboundKeyedReceiver keyed{version};
    InboundRecordingTap layout_tap;
    keyed.harness.config.source_tap = &layout_tap;
    const bool tagged = version >= 12;
    const bool count_only = version >= 15;

    std::vector<std::byte> unit(20 + 6, std::byte{0x11});
    unit[4] = std::byte{0x00};
    unit[5] = std::byte{0x20};
    unit[6] = std::byte{0x08};
    unit[7] = std::byte{0x01};
    unit[8] = std::byte{0x3c};
    unit[19] = std::byte{0x40};
    keyed.receive(keyed.protect(takion::PacketFamily::video, unit, 0x1000, 6));
    ASSERT_EQ(layout_tap.datagrams.size(), 1U) << "version " << version;
    const auto seen_video = layout_tap.datagrams[0].facts;
    EXPECT_EQ(seen_video.media, takion::SourceMedia::video)
        << "version " << version;
    EXPECT_EQ(seen_video.key_position, 0x1000U) << "version " << version;
    EXPECT_EQ(seen_video.sealed_size, 6U) << "version " << version;
    EXPECT_EQ(seen_video.unit_index, 1U) << "version " << version;
    EXPECT_EQ(seen_video.unit_total, 3U) << "version " << version;
    EXPECT_EQ(seen_video.unit_redundant, 1U) << "version " << version;
    EXPECT_EQ(seen_video.stream_index, 2U) << "version " << version;
    EXPECT_EQ(seen_video.codec, 0x3cU) << "version " << version;
    EXPECT_FALSE(seen_video.auxiliary) << "version " << version;

    keyed.receive(keyed.protect(takion::PacketFamily::audio_and_haptics,
                                layout_audio(tagged, count_only, 0, 1), 0x2000,
                                8));
    ASSERT_EQ(layout_tap.datagrams.size(), 2U) << "version " << version;
    const auto seen_audio = layout_tap.datagrams[1].facts;
    EXPECT_EQ(seen_audio.media, takion::SourceMedia::audio)
        << "version " << version;
    EXPECT_EQ(seen_audio.key_position, 0x2000U) << "version " << version;
    EXPECT_EQ(seen_audio.sealed_size, 8U) << "version " << version;
    EXPECT_EQ(seen_audio.unit_total, 2U) << "version " << version;
    EXPECT_EQ(seen_audio.unit_redundant, 1U) << "version " << version;
    EXPECT_EQ(seen_audio.unit_size, 4U) << "version " << version;
    EXPECT_EQ(seen_audio.codec, 5U) << "version " << version;
    EXPECT_FALSE(seen_audio.auxiliary) << "version " << version;

    keyed.receive(keyed.protect(takion::PacketFamily::audio_and_haptics,
                                layout_audio(tagged, count_only, 0, 0), 0x3000,
                                8));
    ASSERT_EQ(layout_tap.datagrams.size(), 3U) << "version " << version;
    EXPECT_EQ(layout_tap.datagrams[2].facts.unit_total, 0U)
        << "version " << version;
    EXPECT_EQ(layout_tap.datagrams[2].facts.unit_size, 0U)
        << "version " << version;

    if (!tagged) continue;
    keyed.receive(keyed.protect(
        takion::PacketFamily::audio_and_haptics,
        layout_audio(tagged, count_only, count_only ? 0xe3 : 0x03, 1), 0x4000,
        8));
    ASSERT_EQ(layout_tap.datagrams.size(), 4U) << "version " << version;
    EXPECT_TRUE(layout_tap.datagrams[3].facts.auxiliary)
        << "version " << version;
  }
}

TEST_F(TakionInboundSourceTap, DescribesAuthenticatedTemporalAudio) {
  for (const std::uint32_t version : {15U, 20U, 23U, 25U}) {
    InboundKeyedReceiver keyed{version};
    InboundRecordingTap audio_tap;
    keyed.harness.config.source_tap = &audio_tap;
    std::uint64_t position = 0x1000;
    for (const std::uint8_t mode :
         {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}}) {
      for (const std::uint8_t tag : {std::uint8_t{0}, std::uint8_t{0x22}}) {
        std::vector<std::byte> bytes(19 + 240, std::byte{});
        bytes[3] = std::byte{100};
        bytes[5] = std::byte{2};
        bytes[6] = static_cast<std::byte>(mode << 4U);
        bytes[7] = std::byte{2};
        bytes[8] = std::byte{5};
        bytes[18] = static_cast<std::byte>(tag);
        keyed.receive(keyed.protect(takion::PacketFamily::audio_and_haptics,
                                    bytes, position, 240));
        ASSERT_FALSE(audio_tap.datagrams.empty())
            << "version " << version << " mode " << +mode << " tag " << +tag;
        const auto seen = audio_tap.datagrams.back().facts;
        EXPECT_EQ(seen.key_position, position)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        EXPECT_EQ(seen.sealed_size, 240U)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        EXPECT_EQ(seen.unit_total, 3U)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        EXPECT_EQ(seen.unit_size, 80U)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        EXPECT_EQ(seen.unit_redundant, mode == 2 && tag == 0 ? 2U : 1U)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        EXPECT_EQ(seen.auxiliary, tag != 0)
            << "version " << version << " mode " << +mode << " tag " << +tag;
        position += 0x1000;
      }
    }
    EXPECT_EQ(audio_tap.datagrams.size(), 8U) << "version " << version;
  }
}

}  // namespace
}  // namespace takion_test
