#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::HardwareId;

static_assert(!std::is_default_constructible_v<HardwareId>);

TEST(HardwareId, ReadsTwelveHexDigits) {
  const auto plain = HardwareId::parse("a1b2c3d4e5f6");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->text(), "a1b2c3d4e5f6");
  EXPECT_EQ(plain->bytes()[0], std::byte{0xa1});
  EXPECT_EQ(plain->bytes()[5], std::byte{0xf6});
}

TEST(HardwareId, ReadsTheSameDigitsInAnyDress) {
  const auto plain = HardwareId::parse("a1b2c3d4e5f6");
  ASSERT_TRUE(plain);
  EXPECT_EQ(HardwareId::parse("A1B2C3D4E5F6"), plain);
  EXPECT_EQ(HardwareId::parse("A1:b2:C3:d4:E5:f6"), plain);
  EXPECT_EQ(HardwareId::parse("a1-b2-c3-d4-e5-f6"), plain);
  EXPECT_EQ(HardwareId::parse("a1b2.c3d4.e5f6"), plain);
  EXPECT_EQ(HardwareId::from_bytes(plain->bytes()), plain);
}

TEST(HardwareId, OrdersByValue) {
  const auto plain = HardwareId::parse("a1b2c3d4e5f6");
  const auto low = HardwareId::parse("000000000001");
  ASSERT_TRUE(plain);
  ASSERT_TRUE(low);
  EXPECT_LT(*low, *plain);
}

TEST(HardwareId, RefusesEverythingElse) {
  for (const std::string_view refused :
       {"", "a1b2c3d4e5f", "a1b2c3d4e5f6a", "a1b2c3d4e5g6", "a1 b2 c3 d4 e5 f6",
        "a1:b2:c3:d4:e5:f6:", "::::::::::::", "000000000000"})
    EXPECT_FALSE(HardwareId::parse(refused)) << refused;
}

TEST(HardwareId, RefusesZeroBytes) {
  EXPECT_FALSE(
      HardwareId::from_bytes(std::array<std::byte, HardwareId::size>{}));
}

}  // namespace
}  // namespace consoles_test
