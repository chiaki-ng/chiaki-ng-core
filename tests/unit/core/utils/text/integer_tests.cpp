#include "src/core/utils/text/integer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace text_test {
namespace {

using libchiaking::text::parse_integer;

TEST(ParseInteger, AcceptsWholeNumbers) {
  EXPECT_EQ(parse_integer<std::uint16_t>("0"), 0U);
  EXPECT_EQ(parse_integer<std::uint16_t>("65535"), 65535U);
  EXPECT_EQ(parse_integer<std::uint16_t>("0080"), 80U);
  EXPECT_EQ(parse_integer<std::int64_t>("-200"), -200);
  EXPECT_EQ(parse_integer<std::uint32_t>("80108b10", 16), 0x80108b10U);
  EXPECT_EQ(parse_integer<std::uint32_t>("80108B10", 16), 0x80108b10U);
}

TEST(ParseInteger, RejectsSignsSpacesAndDecorations) {
  for (const std::string_view text :
       {"", " 1", "1 ", "+1", "1x", "0x10", "1.0", "\t1"})
    EXPECT_FALSE(parse_integer<std::uint32_t>(text)) << '"' << text << '"';
}

TEST(ParseInteger, RejectsValuesOutOfRange) {
  EXPECT_FALSE(parse_integer<std::uint16_t>("65536"));
  EXPECT_FALSE(parse_integer<std::uint16_t>("-1"));
  EXPECT_FALSE(parse_integer<std::uint32_t>("100000000", 16));
  EXPECT_FALSE(parse_integer<std::uint32_t>("0x1f", 16));
  EXPECT_FALSE(parse_integer<std::int64_t>("99999999999999999999"));
}

}  // namespace
}  // namespace text_test
