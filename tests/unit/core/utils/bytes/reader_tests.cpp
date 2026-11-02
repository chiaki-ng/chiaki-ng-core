#include "src/core/utils/bytes/byte_io.hpp"
#include "tests/unit/core/utils/bytes/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>

namespace bytes_test {
namespace {

using libchiaking::bytes::Reader;

TEST(ByteReader, ReadsEveryWidthInOrder) {
  Reader reader{Rig::sample};
  EXPECT_EQ(reader.offset(), 0U);
  EXPECT_EQ(reader.remaining(), Rig::sample.size());
  EXPECT_FALSE(reader.consume("RX"));
  EXPECT_EQ(reader.offset(), 0U);
  EXPECT_TRUE(reader.consume("RC"));
  EXPECT_EQ(reader.u8().value(), 0x7f);
  EXPECT_EQ(reader.u16_be().value(), 0x1234);
  EXPECT_EQ(reader.u32_be().value(), 0x89abcdefU);
  EXPECT_EQ(reader.u64_be().value(), 0x0123456789abcdefULL);
  EXPECT_EQ(reader.i16_be().value(), -32768);
  EXPECT_EQ(reader.offset(), 19U);
  EXPECT_EQ(reader.remaining(), 2U);
  const auto tail = reader.take(2);
  ASSERT_TRUE(tail);
  EXPECT_EQ(tail.value().size(), 2U);
  EXPECT_EQ(tail.value().data(), Rig::sample.data() + 19);
  EXPECT_EQ(reader.remaining(), 0U);
  EXPECT_TRUE(reader.take(0));
  EXPECT_TRUE(reader.skip(0));
  EXPECT_TRUE(reader.consume(""));
}

TEST(ByteReader, ReadsSignedSixteenBitValues) {
  Reader negative{std::span{Rig::sample}.last(2)};
  EXPECT_EQ(negative.i16_be().value(), -2);
  Reader positive{std::span{Rig::sample}.subspan(2, 2)};
  EXPECT_EQ(positive.i16_be().value(), 0x7f12);
}

TEST(ByteReader, RefusesEveryReadFromNothing) {
  Reader empty{std::span<const std::byte>{}};
  EXPECT_TRUE(Rig::refused(empty.u8(), 0, 1, 0));
  EXPECT_TRUE(Rig::refused(empty.u16_be(), 0, 2, 0));
  EXPECT_TRUE(Rig::refused(empty.u32_be(), 0, 4, 0));
  EXPECT_TRUE(Rig::refused(empty.u64_be(), 0, 8, 0));
  EXPECT_TRUE(Rig::refused(empty.i16_be(), 0, 2, 0));
  EXPECT_TRUE(Rig::refused(empty.take(1), 0, 1, 0));
  EXPECT_TRUE(Rig::refused(empty.skip(1), 0, 1, 0));
  EXPECT_FALSE(empty.consume("R"));
}

TEST(ByteReader, RefusesAWideReadFromEveryShorterInput) {
  for (std::size_t size = 0; size < 8; ++size) {
    Reader reader{std::span{Rig::sample}.first(size)};
    EXPECT_TRUE(Rig::refused(reader.u64_be(), 0, 8, size)) << "size " << size;
    EXPECT_EQ(reader.offset(), 0U) << "size " << size;
    EXPECT_EQ(reader.remaining(), size);
  }
}

TEST(ByteReader, KeepsItsPlaceAfterARefusedRead) {
  Reader reader{std::span{Rig::sample}.first(5)};
  EXPECT_TRUE(reader.skip(2));
  EXPECT_TRUE(Rig::refused(reader.u32_be(), 2, 4, 3));
  EXPECT_EQ(reader.offset(), 2U);
  EXPECT_TRUE(Rig::refused(reader.take(4), 2, 4, 3));
  EXPECT_TRUE(Rig::refused(reader.take(static_cast<std::size_t>(-1)), 2,
                           static_cast<std::size_t>(-1), 3));
  EXPECT_TRUE(Rig::refused(reader.skip(static_cast<std::size_t>(-1)), 2,
                           static_cast<std::size_t>(-1), 3));
  EXPECT_FALSE(reader.consume("\x7f\x12\x34\x89"));
  EXPECT_EQ(reader.offset(), 2U);
  EXPECT_EQ(reader.u16_be().value(), 0x7f12);
  EXPECT_EQ(reader.u8().value(), 0x34);
  EXPECT_TRUE(Rig::refused(reader.u8(), 5, 1, 0));
}

}  // namespace
}  // namespace bytes_test
