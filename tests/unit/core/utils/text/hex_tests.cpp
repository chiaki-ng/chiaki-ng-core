#include "src/core/utils/text/hex.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace text_test {
namespace {

using libchiaking::text::append_hex;
using libchiaking::text::decode_hex;
using libchiaking::text::decode_hex_into;
using libchiaking::text::hex_digit;
using libchiaking::text::HexCase;
using libchiaking::text::lower_hex_digit;

class Hex : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 4> sample{
      std::byte{0x00}, std::byte{0x9f}, std::byte{0xa0}, std::byte{0xff}};
};

TEST_F(Hex, EncodesBothCases) {
  std::string lower;
  append_hex(lower, sample);
  EXPECT_EQ(lower, "009fa0ff");
  std::string upper = "x";
  append_hex(upper, sample, HexCase::upper);
  EXPECT_EQ(upper, "x009FA0FF");
}

TEST_F(Hex, ClassifiesDigits) {
  EXPECT_EQ(hex_digit('0'), 0U);
  EXPECT_EQ(hex_digit('9'), 9U);
  EXPECT_EQ(hex_digit('a'), 10U);
  EXPECT_EQ(hex_digit('F'), 15U);
  EXPECT_FALSE(hex_digit('g'));
  EXPECT_FALSE(hex_digit('/'));
  EXPECT_FALSE(hex_digit(':'));
}

TEST_F(Hex, ClassifiesOnlyLowercaseDigitsAsLower) {
  for (const char value : std::string{"0123456789abcdef"})
    EXPECT_TRUE(lower_hex_digit(value)) << value;
  for (const char value : std::string{"ABCDEFg/:` "})
    EXPECT_FALSE(lower_hex_digit(value)) << value;
}

TEST_F(Hex, DecodesExactlyTheBufferLength) {
  std::array<std::byte, 4> exact{};
  ASSERT_TRUE(decode_hex("009FA0ff", exact));
  EXPECT_EQ(exact, sample);
  EXPECT_FALSE(decode_hex("009fa0f", exact));
  EXPECT_FALSE(decode_hex("009fa0ffff", exact));
  EXPECT_FALSE(decode_hex("009fa0fg", exact));
}

TEST_F(Hex, DecodesUpToTheBufferLength) {
  std::array<std::byte, 4> bounded{};
  EXPECT_EQ(decode_hex_into("009f", bounded), 2U);
  EXPECT_EQ(bounded[0], std::byte{0x00});
  EXPECT_EQ(bounded[1], std::byte{0x9f});
  EXPECT_EQ(decode_hex_into("", bounded), 0U);
  EXPECT_EQ(decode_hex_into("009fa0ff", bounded), 4U);
  EXPECT_EQ(bounded, sample);
  EXPECT_FALSE(decode_hex_into("009fa0ff00", bounded));
  EXPECT_FALSE(decode_hex_into("009", bounded));
  EXPECT_FALSE(decode_hex_into("0x9f", bounded));
}

TEST_F(Hex, LeavesNothingDecodedAfterAFailure) {
  std::array<std::byte, 4> exact{};
  EXPECT_FALSE(decode_hex("ffffffzz", exact));
  EXPECT_EQ(exact, (std::array<std::byte, 4>{}));
  std::array<std::byte, 4> bounded{};
  bounded.fill(std::byte{0x5a});
  EXPECT_FALSE(decode_hex_into("ffzz", bounded));
  EXPECT_EQ(bounded, (std::array<std::byte, 4>{}));
  bounded.fill(std::byte{0x5a});
  EXPECT_FALSE(decode_hex_into("009", bounded));
  EXPECT_EQ(bounded, (std::array<std::byte, 4>{}));
}

}  // namespace
}  // namespace text_test
