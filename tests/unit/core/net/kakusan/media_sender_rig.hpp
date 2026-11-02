#pragma once

#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace net_kakusan_test {

class MediaSenderTest : public WireVectorsTest {
 protected:
  using MediaSender = libchiaking::net::kakusan::MediaSender;
  using MediaKind = libchiaking::net::kakusan::MediaKind;
  using MediaHeader = libchiaking::net::kakusan::MediaHeader;
  using AckRange = libchiaking::net::kakusan::AckRange;
  using Datagrams = std::vector<std::vector<std::byte>>;

  static auto payload(std::size_t size, std::uint8_t seed = 0x5a)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
      bytes[index] = static_cast<std::byte>((seed + index * 7) & 0xffU);
    return bytes;
  }

  static auto header_of(std::span<const std::byte> datagram) -> MediaHeader {
    const auto decoded =
        libchiaking::net::kakusan::TransportFrameCodec::decode_media(datagram);
    EXPECT_TRUE(decoded);
    if (!decoded) return {};
    return decoded.value().header;
  }

  static auto poll_ok(MediaSender& sender, std::uint64_t now) -> Datagrams {
    Datagrams datagrams;
    const auto polled = sender.poll(now, datagrams);
    EXPECT_TRUE(polled);
    return datagrams;
  }

  static void submit_ok(MediaSender& sender, MediaKind kind, std::size_t size,
                        std::uint64_t now) {
    const auto bytes = payload(size);
    const auto submitted = sender.submit(kind, 0, bytes, now);
    EXPECT_TRUE(submitted);
  }

  static auto ack_record(std::uint64_t largest, std::uint32_t delay,
                         const std::vector<AckRange>& ranges,
                         std::size_t extra = 0) -> std::vector<std::byte> {
    std::vector<std::byte> record;
    libchiaking::bytes::Appender writer{record};
    writer.text(libchiaking::net::kakusan::media_ack_prefix);
    writer.u64_be(largest);
    writer.u32_be(delay);
    writer.u8(static_cast<std::uint8_t>(ranges.size()));
    writer.zeros(3);
    for (const auto& range : ranges) {
      writer.u64_be(range.low);
      writer.u64_be(range.high);
    }
    writer.zeros(extra);
    return record;
  }

  static void send_three_small(MediaSender& sender) {
    for (int index = 0; index < 3; ++index)
      submit_ok(sender, MediaKind::audio, 100, 0);
    const auto datagrams = poll_ok(sender, 0);
    ASSERT_EQ(datagrams.size(), 3U);
    for (std::size_t index = 0; index < 3; ++index)
      EXPECT_EQ(header_of(datagrams[index]).datagram_sequence, index);
  }
};

}  // namespace net_kakusan_test
