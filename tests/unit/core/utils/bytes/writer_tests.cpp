#include "src/core/utils/bytes/byte_io.hpp"
#include "tests/unit/core/utils/bytes/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

namespace bytes_test {
namespace {

using libchiaking::bytes::Writer;

TEST(ByteWriter, WritesEveryWidthInOrder) {
  std::array<std::byte, 26> buffer{};
  buffer.fill(std::byte{0xee});
  Writer writer{buffer};
  EXPECT_TRUE(writer.text("RC"));
  EXPECT_TRUE(writer.u8(0x7f));
  EXPECT_TRUE(writer.u16_be(0x1234));
  EXPECT_TRUE(writer.u32_be(0x89abcdefU));
  EXPECT_TRUE(writer.u64_be(0x0123456789abcdefULL));
  EXPECT_TRUE(writer.i16_be(-32768));
  EXPECT_TRUE(writer.bytes(std::span{Rig::sample}.last(2)));
  EXPECT_EQ(writer.offset(), Rig::sample.size());
  EXPECT_EQ(writer.remaining(), 5U);
  EXPECT_TRUE(writer.zeros(3));
  EXPECT_TRUE(writer.i16_be(-2));
  EXPECT_EQ(writer.remaining(), 0U);
  for (std::size_t index = 0; index < Rig::sample.size(); ++index)
    EXPECT_EQ(buffer[index], Rig::sample[index]) << "index " << index;
  EXPECT_EQ(buffer[21], std::byte{});
  EXPECT_EQ(buffer[22], std::byte{});
  EXPECT_EQ(buffer[23], std::byte{});
  EXPECT_EQ(buffer[24], std::byte{0xff});
  EXPECT_EQ(buffer[25], std::byte{0xfe});
  EXPECT_TRUE(writer.zeros(0));
  EXPECT_TRUE(writer.bytes({}));
  EXPECT_TRUE(writer.text(""));
}

TEST(ByteWriter, LeavesTheBufferUntouchedPastARefusal) {
  std::array<std::byte, 9> buffer{};
  buffer.fill(std::byte{0xee});
  Writer writer{std::span{buffer}.first(7)};
  EXPECT_TRUE(writer.u32_be(1));
  EXPECT_TRUE(Rig::refused(writer.u64_be(2), 4, 8, 3));
  EXPECT_TRUE(Rig::refused(writer.u32_be(2), 4, 4, 3));
  EXPECT_TRUE(Rig::refused(writer.text("abcd"), 4, 4, 3));
  EXPECT_TRUE(
      Rig::refused(writer.bytes(std::span{Rig::sample}.first(4)), 4, 4, 3));
  EXPECT_TRUE(Rig::refused(writer.zeros(4), 4, 4, 3));
  EXPECT_TRUE(Rig::refused(writer.zeros(static_cast<std::size_t>(-1)), 4,
                           static_cast<std::size_t>(-1), 3));
  EXPECT_EQ(writer.offset(), 4U);
  for (std::size_t index = 4; index < buffer.size(); ++index)
    EXPECT_EQ(buffer[index], std::byte{0xee}) << "index " << index;
  EXPECT_TRUE(writer.u16_be(0x0102));
  EXPECT_TRUE(writer.u8(3));
  EXPECT_TRUE(Rig::refused(writer.u8(4), 7, 1, 0));
  EXPECT_TRUE(Rig::refused(writer.u16_be(4), 7, 2, 0));
  EXPECT_TRUE(Rig::refused(writer.i16_be(4), 7, 2, 0));
  EXPECT_EQ(buffer[4], std::byte{1});
  EXPECT_EQ(buffer[5], std::byte{2});
  EXPECT_EQ(buffer[6], std::byte{3});
  EXPECT_EQ(buffer[7], std::byte{0xee});
  EXPECT_EQ(buffer[8], std::byte{0xee});
}

TEST(ByteWriter, RefusesEveryWriteIntoNothingButZeroBytes) {
  Writer none{std::span<std::byte>{}};
  EXPECT_TRUE(Rig::refused(none.u8(1), 0, 1, 0));
  EXPECT_TRUE(none.zeros(0));
}

TEST(ByteWriter, WritesLittleEndian) {
  std::array<std::byte, 6> buffer{};
  Writer writer(buffer);
  EXPECT_TRUE(writer.u16_le(0x1234));
  EXPECT_TRUE(writer.u32_le(0x89abcdefU));
  EXPECT_EQ(buffer, (std::array<std::byte, 6>{
                        std::byte{0x34}, std::byte{0x12}, std::byte{0xef},
                        std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}}));
  EXPECT_TRUE(Rig::refused(writer.u16_le(1), 6, 2, 0));
}

}  // namespace
}  // namespace bytes_test
