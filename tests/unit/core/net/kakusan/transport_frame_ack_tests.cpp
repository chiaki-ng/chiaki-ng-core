#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class TransportFrameMediaAck : public WireVectorsTest {
 protected:
  static auto raw(std::string_view text) -> Result<MediaAck> {
    return TransportFrameCodec::decode_media_ack(
        test_support::Bytes::unhex(text));
  }
};

TEST_F(TransportFrameMediaAck, MatchesTheVectorAndDecodesBack) {
  const auto encoded = TransportFrameCodec::encode_media_ack(
      MediaAck{2, 1000, {AckRange{0, 2}}});
  ASSERT_TRUE(encoded);
  EXPECT_EQ(test_support::Bytes::hex(encoded.value()), vectors.text("ack_hex"));
  const auto decoded = TransportFrameCodec::decode_media_ack(encoded.value());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().largest, 2U);
  EXPECT_EQ(decoded.value().delay_us, 1000U);
  EXPECT_EQ(decoded.value().ranges, (std::vector<AckRange>{AckRange{0, 2}}));
}

TEST_F(TransportFrameMediaAck, CarriesGappedRanges) {
  const auto gapped = TransportFrameCodec::encode_media_ack(
      MediaAck{20, 0, {AckRange{18, 20}, AckRange{10, 16}, AckRange{0, 0}}});
  ASSERT_TRUE(gapped);
  const auto decoded = TransportFrameCodec::decode_media_ack(gapped.value());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().ranges.size(), 3U);
}

TEST_F(TransportFrameMediaAck, RefusesToEncodeInvalidRanges) {
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(MediaAck{2, 0, {}}));
  EXPECT_FALSE(
      TransportFrameCodec::encode_media_ack(MediaAck{3, 0, {AckRange{0, 2}}}));
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(
      MediaAck{20, 0, {AckRange{18, 20}, AckRange{10, 17}}}));
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(
      MediaAck{20, 0, {AckRange{18, 20}, AckRange{10, 18}}}));
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(
      MediaAck{20, 0, {AckRange{20, 18}}}));
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(
      MediaAck{20, 0, {AckRange{10, 20}, AckRange{30, 40}}}));
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(
      MediaAck{5, 0, {AckRange{0, 5}, AckRange{0, 0}}}));
}

TEST_F(TransportFrameMediaAck, HoldsAtMostTheRangeLimit) {
  std::vector<AckRange> many;
  for (std::uint64_t index = 0; index < max_ack_ranges + 1; ++index)
    many.push_back(AckRange{1000 - index * 10, 1000 - index * 10});
  EXPECT_FALSE(TransportFrameCodec::encode_media_ack(MediaAck{1000, 0, many}));
  many.pop_back();
  EXPECT_TRUE(TransportFrameCodec::encode_media_ack(MediaAck{1000, 0, many}));
}

TEST_F(TransportFrameMediaAck, RefusesToDecodeMalformedRecords) {
  const std::string valid = vectors.text("ack_hex");
  EXPECT_TRUE(raw(valid));
  EXPECT_FALSE(raw(valid + "00"));
  EXPECT_FALSE(raw(valid.substr(0, valid.size() - 2)));
  EXPECT_FALSE(raw("52434132" + valid.substr(8)));
  EXPECT_FALSE(raw(valid.substr(0, 32) + "00" + valid.substr(34)));
  EXPECT_FALSE(raw(valid.substr(0, 32) + "02" + valid.substr(34)));
  EXPECT_FALSE(raw(valid.substr(0, 34) + "01" + valid.substr(36)));
  EXPECT_FALSE(raw(valid.substr(0, 8) + "0000000000000003" + valid.substr(24)));
}

}  // namespace
}  // namespace net_kakusan_test
