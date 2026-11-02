#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/control_chunk_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionEnvelope : public ::testing::Test {
 protected:
  const std::vector<std::byte> plain = TakionControlChunkVectors::init();
  const takion::PacketEnvelope psn_strategy{takion::PsnEnvelope{0x42}};
};

TEST_F(TakionEnvelope, LeavesAPlainPacketUnchanged) {
  const takion::PacketEnvelope plain_strategy{takion::PlainEnvelope{}};
  const auto unchanged = takion::EnvelopeCodec::encode(plain, plain_strategy);
  ASSERT_TRUE(unchanged);
  EXPECT_TRUE(std::ranges::equal(unchanged.value().bytes(), plain));
  const auto plain_decoded =
      takion::EnvelopeCodec::decode(plain, plain_strategy);
  ASSERT_TRUE(plain_decoded);
  EXPECT_TRUE(std::ranges::equal(plain_decoded.value(), plain));
}

TEST_F(TakionEnvelope, PrefixesAPsnPacketWithItsType) {
  const auto wrapped = takion::EnvelopeCodec::encode(plain, psn_strategy);
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped.value().size(), plain.size() + 4);
  EXPECT_EQ(wrapped.value()[0], std::byte{0});
  EXPECT_EQ(wrapped.value()[1], std::byte{0});
  EXPECT_EQ(wrapped.value()[2], std::byte{0});
  EXPECT_EQ(wrapped.value()[3], std::byte{0x42});
  const auto unwrapped =
      takion::EnvelopeCodec::decode(wrapped.value(), psn_strategy);
  ASSERT_TRUE(unwrapped);
  EXPECT_TRUE(std::ranges::equal(unwrapped.value(), plain));
}

TEST_F(TakionEnvelope, RefusesATruncatedPsnPrefix) {
  const auto wrapped = takion::EnvelopeCodec::encode(plain, psn_strategy);
  ASSERT_TRUE(wrapped.has_value());
  for (std::size_t size = 0; size < 4; ++size) {
    const auto malformed = takion::EnvelopeCodec::decode(
        std::span{wrapped.value()}.first(size), psn_strategy);
    ASSERT_FALSE(malformed) << "size " << size;
    EXPECT_EQ(malformed.error().reason, takion::ParseFailureReason::truncated);
  }
}

TEST_F(TakionEnvelope, RefusesANonZeroPsnPadding) {
  const auto wrapped = takion::EnvelopeCodec::encode(plain, psn_strategy);
  ASSERT_TRUE(wrapped.has_value());
  for (std::size_t index = 0; index < 3; ++index) {
    auto malformed = wrapped.value().clone();
    malformed[index] = std::byte{1};
    const auto result =
        takion::EnvelopeCodec::decode(malformed.bytes(), psn_strategy);
    ASSERT_FALSE(result) << "index " << index;
    EXPECT_EQ(result.error().reason, takion::ParseFailureReason::invalid_field);
    EXPECT_EQ(result.error().offset.value, index);
  }
}

TEST_F(TakionEnvelope, RefusesAnotherPsnType) {
  const auto wrapped = takion::EnvelopeCodec::encode(plain, psn_strategy);
  ASSERT_TRUE(wrapped.has_value());
  auto wrong_type = wrapped.value().clone();
  wrong_type[3] = std::byte{0x41};
  const auto mismatch =
      takion::EnvelopeCodec::decode(wrong_type.bytes(), psn_strategy);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().offset.value, 3U);
}

}  // namespace
}  // namespace takion_test
