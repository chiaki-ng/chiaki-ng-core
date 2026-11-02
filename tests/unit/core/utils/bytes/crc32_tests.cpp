#include "src/core/utils/bytes/crc32.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace bytes_test {
namespace {

using libchiaking::bytes::crc32;

class Crc32 : public ::testing::Test {
 protected:
  static auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    for (const char value : text)
      bytes.push_back(static_cast<std::byte>(value));
    return bytes;
  }
};

TEST_F(Crc32, MatchesTheStandardCheckValue) {
  EXPECT_EQ(crc32(bytes_of("123456789")), 0xcbf43926U);
}

TEST_F(Crc32, IsZeroForEmptyInput) {
  static_assert(crc32(std::span<const std::byte>{}) == 0U);
  EXPECT_EQ(crc32(std::span<const std::byte>{}), 0U);
}

TEST_F(Crc32, MatchesKnownVectors) {
  EXPECT_EQ(crc32(bytes_of("a")), 0xe8b7be43U);
  EXPECT_EQ(crc32(bytes_of("The quick brown fox jumps over the lazy dog")),
            0x414fa339U);
  constexpr std::array<std::byte, 4> zeros{};
  static_assert(crc32(zeros) == 0x2144df1cU);
  EXPECT_EQ(crc32(zeros), 0x2144df1cU);
}

}  // namespace
}  // namespace bytes_test
