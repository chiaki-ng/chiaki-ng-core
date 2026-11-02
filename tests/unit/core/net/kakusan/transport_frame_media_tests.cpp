#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class TransportFrameMedia : public WireVectorsTest {
 protected:
  auto sealed(MediaHeader header, std::span<const std::byte> fragment)
      -> Result<std::vector<std::byte>> {
    return TransportFrameCodec::encode_media(
        crypto, vectors.key(), vectors.connection(), header, fragment);
  }
};

class TransportFrameConnectionId : public TransportFrameMedia {};
class TransportFrameProbe : public TransportFrameMedia {};
class TransportFrameMediaBounds : public TransportFrameMedia {};
class TransportFrameMediaAuthentication : public TransportFrameMedia {};

TEST_F(TransportFrameConnectionId, IsDerivedFromTheVectorTranscript) {
  const auto transcript = test_support::Bytes::unhex_array<32>(
      test_support::kakusan::Fixtures::string(
          test_support::kakusan::Fixtures::text("vectors.json"), "transcript"));
  const auto id = TransportFrameCodec::connection_id(crypto, transcript);
  ASSERT_TRUE(id);
  EXPECT_EQ(id.value(), vectors.connection());
}

TEST_F(TransportFrameMedia, FragmentsMatchTheVectorsAndDecodeBack) {
  const auto& expected = vectors.fragments();
  const auto& original = vectors.original();
  ASSERT_EQ(expected.size(), 2U);
  EXPECT_EQ(TransportFrameCodec::media_fragment_count(original.size()), 2U);
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    const auto offset = index * media_fragment_size;
    const auto part = std::span<const std::byte>{original}.subspan(
        offset, std::min(media_fragment_size, original.size() - offset));
    MediaHeader header;
    header.kind = MediaKind::video;
    header.datagram_sequence = index;
    header.packet_sequence = 7;
    header.key_position = (std::uint64_t{1} << 32U) + 32;
    header.original_size = static_cast<std::uint16_t>(original.size());
    header.fragment_offset = static_cast<std::uint16_t>(offset);
    header.fragment_size = static_cast<std::uint16_t>(part.size());
    const auto encoded = sealed(header, part);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(test_support::Bytes::hex(encoded.value()),
              test_support::kakusan::Fixtures::string_of(expected[index]));

    EXPECT_TRUE(TransportFrameCodec::media_matches(encoded.value(),
                                                   vectors.connection()));
    EXPECT_TRUE(TransportFrameCodec::authenticate_media(crypto, vectors.key(),
                                                        encoded.value())
                    .value());
    const auto decoded = TransportFrameCodec::decode_media(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().header.datagram_sequence, index);
    EXPECT_EQ(decoded.value().header.packet_sequence, 7U);
    EXPECT_EQ(decoded.value().header.key_position, header.key_position);
    EXPECT_EQ(decoded.value().header.original_size, original.size());
    EXPECT_EQ(decoded.value().header.fragment_offset, offset);
    EXPECT_TRUE(std::ranges::equal(decoded.value().fragment, part));
  }
}

TEST_F(TransportFrameProbe, MatchesTheVector) {
  MediaHeader header;
  header.kind = MediaKind::probe;
  header.datagram_sequence = 2;
  const auto encoded = sealed(header, {});
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value().size(), 76U);
  EXPECT_EQ(test_support::Bytes::hex(encoded.value()),
            vectors.text("probe_hex"));
  const auto decoded = TransportFrameCodec::decode_media(encoded.value());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().header.kind, MediaKind::probe);
}

TEST_F(TransportFrameProbe, RefusesAKeyPositionOrPacketSequence) {
  MediaHeader header;
  header.kind = MediaKind::probe;
  header.datagram_sequence = 2;
  header.key_position = 1;
  EXPECT_FALSE(sealed(header, {}));
  header.key_position = 0;
  header.packet_sequence = 1;
  EXPECT_FALSE(sealed(header, {}));
}

TEST_F(TransportFrameMediaBounds, FragmentsEverySizeWithinTheDatagramLimit) {
  std::vector<std::byte> source(max_source_datagram_size + 1, std::byte{0x5a});
  for (const std::size_t size : {std::size_t{1}, std::size_t{1124},
                                 std::size_t{1125}, max_source_datagram_size}) {
    SCOPED_TRACE(size);
    const auto count = TransportFrameCodec::media_fragment_count(size);
    std::vector<std::byte> rebuilt;
    for (std::size_t index = 0; index < count; ++index) {
      const auto offset = index * media_fragment_size;
      const auto part = std::span<const std::byte>{source}.subspan(
          offset, std::min(media_fragment_size, size - offset));
      MediaHeader header;
      header.datagram_sequence = index;
      header.original_size = static_cast<std::uint16_t>(size);
      header.fragment_offset = static_cast<std::uint16_t>(offset);
      header.fragment_size = static_cast<std::uint16_t>(part.size());
      const auto encoded = sealed(header, part);
      ASSERT_TRUE(encoded);
      EXPECT_LE(encoded.value().size(), max_datagram_size);
      const auto decoded = TransportFrameCodec::decode_media(encoded.value());
      ASSERT_TRUE(decoded);
      rebuilt.insert(rebuilt.end(), decoded.value().fragment.begin(),
                     decoded.value().fragment.end());
    }
    EXPECT_EQ(rebuilt.size(), size);
  }
}

TEST_F(TransportFrameMediaBounds, CountsFragmentsAtTheEdges) {
  EXPECT_EQ(TransportFrameCodec::media_fragment_count(1), 1U);
  EXPECT_EQ(TransportFrameCodec::media_fragment_count(1124), 1U);
  EXPECT_EQ(TransportFrameCodec::media_fragment_count(1125), 2U);
  EXPECT_EQ(TransportFrameCodec::media_fragment_count(max_source_datagram_size),
            8U);
}

TEST_F(TransportFrameMediaBounds, RefuseInconsistentHeaders) {
  std::vector<std::byte> source(max_source_datagram_size + 1, std::byte{0x5a});
  MediaHeader oversized;
  oversized.original_size =
      static_cast<std::uint16_t>(max_source_datagram_size + 1);
  oversized.fragment_size = static_cast<std::uint16_t>(media_fragment_size);
  EXPECT_FALSE(sealed(oversized, std::span<const std::byte>{source}.first(
                                     media_fragment_size)));

  MediaHeader misaligned;
  misaligned.original_size = 2000;
  misaligned.fragment_offset = 1000;
  misaligned.fragment_size = 1000;
  EXPECT_FALSE(
      sealed(misaligned, std::span<const std::byte>{source}.first(1000)));

  MediaHeader short_fragment;
  short_fragment.original_size = 2000;
  short_fragment.fragment_size = 1000;
  EXPECT_FALSE(
      sealed(short_fragment, std::span<const std::byte>{source}.first(1000)));

  MediaHeader empty;
  EXPECT_FALSE(sealed(empty, {}));
}

TEST_F(TransportFrameMediaAuthentication, AcceptsTheVectorAndNoTampering) {
  const auto valid = vectors.fragment(1);
  EXPECT_TRUE(
      TransportFrameCodec::authenticate_media(crypto, vectors.key(), valid)
          .value());

  for (const std::size_t offset :
       {std::size_t{5}, std::size_t{24}, std::size_t{44}, std::size_t{60},
        valid.size() - 1}) {
    SCOPED_TRACE(offset);
    auto tampered = valid;
    tampered[offset] ^= std::byte{1};
    EXPECT_FALSE(
        TransportFrameCodec::authenticate_media(crypto, vectors.key(), tampered)
            .value());
  }

  auto other_key = vectors.key().clone();
  other_key[0] ^= std::byte{1};
  EXPECT_FALSE(TransportFrameCodec::authenticate_media(crypto, other_key, valid)
                   .value());
}

TEST_F(TransportFrameMediaAuthentication, MatchesOnlyItsConnection) {
  const auto valid = vectors.fragment(1);
  auto other_connection = vectors.connection();
  other_connection[15] ^= std::byte{1};
  EXPECT_FALSE(TransportFrameCodec::media_matches(valid, other_connection));
  EXPECT_TRUE(TransportFrameCodec::media_matches(valid, vectors.connection()));
}

TEST_F(TransportFrameMediaAuthentication, RefusesTruncatedAndJumboDatagrams) {
  const auto valid = vectors.fragment(1);
  EXPECT_FALSE(TransportFrameCodec::media_matches(
      std::span<const std::byte>{valid}.first(75), vectors.connection()));
  EXPECT_FALSE(
      TransportFrameCodec::authenticate_media(
          crypto, vectors.key(), std::span<const std::byte>{valid}.first(75))
          .value());
  std::vector<std::byte> jumbo(max_datagram_size + 1);
  EXPECT_FALSE(TransportFrameCodec::decode_media(jumbo));
}

TEST_F(TransportFrameMediaAuthentication, DecodesOnlyValidResealedHeaders) {
  const auto valid = vectors.fragment(1);
  const auto reseal = [&](std::size_t offset, std::uint8_t value) {
    auto datagram = valid;
    datagram.resize(datagram.size() - media_tag_size);
    datagram[offset] = static_cast<std::byte>(value);
    const auto mac = Hash::hmac_sha256(crypto, vectors.key(), datagram);
    EXPECT_TRUE(mac);
    if (mac)
      datagram.insert(datagram.end(), mac.value().begin(),
                      mac.value().begin() + media_tag_size);
    EXPECT_TRUE(
        TransportFrameCodec::authenticate_media(crypto, vectors.key(), datagram)
            .value());
    return TransportFrameCodec::decode_media(datagram);
  };
  EXPECT_TRUE(reseal(5, 1));
  EXPECT_FALSE(reseal(4, 2));
  EXPECT_FALSE(reseal(5, 3));
  EXPECT_FALSE(reseal(6, 1));
  EXPECT_FALSE(reseal(43, 2));
  EXPECT_FALSE(reseal(57, 0x9d));
  EXPECT_FALSE(reseal(59, 1));
}

}  // namespace
}  // namespace net_kakusan_test
