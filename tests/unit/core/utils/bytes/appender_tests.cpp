#include "src/core/utils/bytes/byte_io.hpp"
#include "tests/unit/core/utils/bytes/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

namespace bytes_test {
namespace {

using libchiaking::bytes::Appender;
using libchiaking::bytes::Reader;

TEST(ByteAppender, AppendsToWhatIsThere) {
  std::vector<std::byte> output{std::byte{0xaa}};
  Appender appender{output};
  appender.text("RC");
  appender.u8(0x7f);
  appender.u16_be(0x1234);
  appender.u32_be(0x89abcdefU);
  appender.u64_be(0x0123456789abcdefULL);
  appender.i16_be(-32768);
  appender.bytes(std::span{Rig::sample}.last(2));
  appender.zeros(2);
  appender.zeros(0);
  appender.text("");
  ASSERT_EQ(output.size(), 1 + Rig::sample.size() + 2);
  EXPECT_EQ(output.front(), std::byte{0xaa});
  for (std::size_t index = 0; index < Rig::sample.size(); ++index)
    EXPECT_EQ(output[index + 1], Rig::sample[index]) << "index " << index;
  EXPECT_EQ(output[22], std::byte{});
  EXPECT_EQ(output[23], std::byte{});

  Reader reader{std::span{output}.subspan(1)};
  ASSERT_TRUE(reader.consume("RC"));
  EXPECT_EQ(reader.u8().value(), 0x7f);
  EXPECT_EQ(reader.u16_be().value(), 0x1234);
}

}  // namespace
}  // namespace bytes_test
