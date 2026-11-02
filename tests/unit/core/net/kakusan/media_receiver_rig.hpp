#pragma once

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace net_kakusan_test {

class MediaReceiverTest : public ::testing::Test {
 protected:
  using Bytes = std::vector<std::byte>;
  using MediaKey = libchiaking::net::kakusan::MediaKey;
  using ConnectionId = libchiaking::net::kakusan::ConnectionId;
  using MediaKind = libchiaking::net::kakusan::MediaKind;
  using MediaReceiver = libchiaking::net::kakusan::MediaReceiver;
  using MediaAck = libchiaking::net::kakusan::MediaAck;
  using AckRange = libchiaking::net::kakusan::AckRange;
  using SourcePacket = libchiaking::net::kakusan::SourcePacket;
  using Codec = libchiaking::net::kakusan::TransportFrameCodec;

  static constexpr std::uint64_t millisecond = 1000;
  static constexpr std::uint64_t max_u64 =
      std::numeric_limits<std::uint64_t>::max();

  static auto make_key(std::uint8_t seed) -> MediaKey {
    MediaKey made{};
    for (std::size_t index = 0; index < made.size(); ++index)
      made[index] = static_cast<std::byte>(seed + index);
    return made;
  }

  static auto make_connection(std::uint8_t seed) -> ConnectionId {
    ConnectionId id{};
    for (std::size_t index = 0; index < id.size(); ++index)
      id[index] = static_cast<std::byte>(seed ^ (index * 7));
    return id;
  }

  static auto pattern(std::size_t size, std::uint8_t seed) -> Bytes {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index)
      bytes[index] = static_cast<std::byte>((index * 31 + seed) & 0xff);
    return bytes;
  }

  auto sealed_fragment(const MediaKey& sealing_key,
                       const ConnectionId& sealing_connection, MediaKind kind,
                       std::uint64_t packet_sequence,
                       std::uint64_t key_position,
                       std::span<const std::byte> original, std::size_t index,
                       std::uint64_t datagram_sequence) -> Bytes {
    const auto offset = index * libchiaking::net::kakusan::media_fragment_size;
    const auto part = original.subspan(
        offset, std::min(libchiaking::net::kakusan::media_fragment_size,
                         original.size() - offset));
    libchiaking::net::kakusan::MediaHeader header;
    header.kind = kind;
    header.datagram_sequence = datagram_sequence;
    header.packet_sequence = packet_sequence;
    header.key_position = key_position;
    header.original_size = static_cast<std::uint16_t>(original.size());
    header.fragment_offset = static_cast<std::uint16_t>(offset);
    header.fragment_size = static_cast<std::uint16_t>(part.size());
    const auto encoded = Codec::encode_media(crypto, sealing_key,
                                             sealing_connection, header, part);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return encoded.value();
  }

  auto fragment_datagram(MediaKind kind, std::uint64_t packet_sequence,
                         std::uint64_t key_position,
                         std::span<const std::byte> original, std::size_t index,
                         std::uint64_t datagram_sequence) -> Bytes {
    return sealed_fragment(key, connection, kind, packet_sequence, key_position,
                           original, index, datagram_sequence);
  }

  auto fragments_of(MediaKind kind, std::uint64_t packet_sequence,
                    std::uint64_t key_position,
                    std::span<const std::byte> original,
                    std::uint64_t first_datagram) -> std::vector<Bytes> {
    std::vector<Bytes> datagrams;
    const auto count = Codec::media_fragment_count(original.size());
    for (std::size_t index = 0; index < count; ++index)
      datagrams.push_back(fragment_datagram(kind, packet_sequence, key_position,
                                            original, index,
                                            first_datagram + index));
    return datagrams;
  }

  auto single(std::uint64_t packet_sequence, std::uint64_t datagram_sequence,
              std::size_t size = 100) -> Bytes {
    const auto original =
        pattern(size, static_cast<std::uint8_t>(packet_sequence));
    return fragment_datagram(MediaKind::video, packet_sequence, 1000, original,
                             0, datagram_sequence);
  }

  auto probe(std::uint64_t datagram_sequence) -> Bytes {
    libchiaking::net::kakusan::MediaHeader header;
    header.kind = MediaKind::probe;
    header.datagram_sequence = datagram_sequence;
    const auto encoded =
        Codec::encode_media(crypto, key, connection, header, {});
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return encoded.value();
  }

  auto reseal(Bytes datagram, std::size_t offset, std::uint8_t value) -> Bytes {
    datagram.resize(datagram.size() -
                    libchiaking::net::kakusan::media_tag_size);
    datagram[offset] = static_cast<std::byte>(value);
    const auto mac =
        libchiaking::net::kakusan::Hash::hmac_sha256(crypto, key, datagram);
    EXPECT_TRUE(mac);
    if (!mac) return datagram;
    datagram.insert(
        datagram.end(), mac.value().begin(),
        mac.value().begin() + libchiaking::net::kakusan::media_tag_size);
    return datagram;
  }

  static auto accept(MediaReceiver& target, const Bytes& datagram,
                     std::uint64_t now_us) -> std::optional<SourcePacket> {
    auto result = target.receive(datagram, now_us);
    EXPECT_TRUE(result);
    if (!result) return std::nullopt;
    return std::move(result).value();
  }

  static auto fatal(MediaReceiver& target, const Bytes& datagram,
                    std::uint64_t now_us) -> bool {
    const auto result = target.receive(datagram, now_us);
    return !result &&
           result.error() == libchiaking::net::kakusan::Error::protocol;
  }

  static auto decoded_ack(const std::optional<Bytes>& record) -> MediaAck {
    EXPECT_TRUE(record);
    if (!record) return {};
    const auto ack = Codec::decode_media_ack(*record);
    EXPECT_TRUE(ack);
    if (!ack) return {};
    return ack.value();
  }

  static auto ranges(std::initializer_list<AckRange> list)
      -> std::vector<AckRange> {
    return std::vector<AckRange>{list};
  }

  static void expect_quiet(MediaReceiver& target, std::uint64_t now_us) {
    EXPECT_FALSE(target.poll(now_us));
    EXPECT_FALSE(target.next_deadline_us());
  }

  test_support::SoftwareCrypto crypto;
  const MediaKey key = make_key(1);
  const ConnectionId connection = make_connection(0xa0);
};

}  // namespace net_kakusan_test
