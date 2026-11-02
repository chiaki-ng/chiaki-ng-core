#include "src/core/utils/bytes/endian.hpp"
#include "tests/unit/core/utils/bytes/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bytes_test {
namespace {

using libchiaking::bytes::append_be;
using libchiaking::bytes::load_be;
using libchiaking::bytes::load_le;
using libchiaking::bytes::store_be;
using libchiaking::bytes::store_le;

TEST(Endian, LoadsAtOffsets) {
  const std::span<const std::byte> view{Rig::sample};
  EXPECT_EQ(load_be<std::uint8_t>(view.subspan(2)), 0x7fU);
  EXPECT_EQ(load_be<std::uint16_t>(view.subspan(3)), 0x1234U);
  EXPECT_EQ(load_be<std::uint32_t>(view.subspan(5)), 0x89abcdefU);
  EXPECT_EQ(load_be<std::uint64_t>(view.subspan(9)), 0x0123456789abcdefULL);
  EXPECT_EQ(load_le<std::uint16_t>(view.subspan(3)), 0x3412U);
  EXPECT_EQ(load_le<std::uint32_t>(view.subspan(5)), 0xefcdab89U);
  EXPECT_EQ(load_le<std::uint64_t>(view.subspan(9)), 0xefcdab8967452301ULL);
  EXPECT_EQ(load_be<std::uint16_t>(view.subspan(19)), 0xfffeU);
  static_assert(load_be<std::uint16_t>(
                    std::span<const std::byte>{Rig::sample}.subspan(3)) ==
                0x1234U);
}

TEST(Endian, StoresAtOffsets) {
  std::array<std::byte, 8> buffer{};
  store_be<std::uint32_t>(std::span{buffer}.subspan(2), 0x01020304U);
  EXPECT_EQ(buffer,
            (std::array<std::byte, 8>{std::byte{0}, std::byte{0}, std::byte{1},
                                      std::byte{2}, std::byte{3}, std::byte{4},
                                      std::byte{0}, std::byte{0}}));
  store_le<std::uint16_t>(std::span{buffer}.subspan(6), 0xbeefU);
  EXPECT_EQ(buffer[6], std::byte{0xef});
  EXPECT_EQ(buffer[7], std::byte{0xbe});
  EXPECT_EQ(load_le<std::uint16_t>(std::span{buffer}.subspan(6)), 0xbeefU);
  store_le<std::uint64_t>(buffer, 0x0807060504030201ULL);
  EXPECT_EQ(load_be<std::uint64_t>(buffer), 0x0102030405060708ULL);
}

TEST(Endian, AppendsBigEndian) {
  std::vector<std::byte> appended{std::byte{0xaa}};
  append_be<std::uint16_t>(appended, 0x0102);
  append_be<std::uint32_t>(appended, 0x03040506U);
  EXPECT_EQ(appended,
            (std::vector<std::byte>{std::byte{0xaa}, std::byte{1}, std::byte{2},
                                    std::byte{3}, std::byte{4}, std::byte{5},
                                    std::byte{6}}));
}

}  // namespace
}  // namespace bytes_test
