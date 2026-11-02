#include "src/core/net/kakusan/pake.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::Code;

static_assert(!std::is_default_constructible_v<Code>);
static_assert(!std::is_copy_constructible_v<Code>);
static_assert(std::is_nothrow_move_constructible_v<Code>);

TEST(PairingCode, ParsesExactlyFourDigits) {
  const auto parsed = Code::parse("0042");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().view(), "0042");
  EXPECT_FALSE(Code::parse("042"));
  EXPECT_FALSE(Code::parse("00420"));
  EXPECT_FALSE(Code::parse("00a2"));
  EXPECT_FALSE(Code::parse(" 042"));
}

TEST(PairingCode, WritesAValueWithLeadingZeros) {
  const auto small = Code::from_value(42);
  ASSERT_TRUE(small);
  EXPECT_EQ(small.value().view(), "0042");
  const auto largest = Code::from_value(9999);
  ASSERT_TRUE(largest);
  EXPECT_EQ(largest.value().view(), "9999");
  const auto zero = Code::from_value(0);
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero.value().view(), "0000");
  EXPECT_FALSE(Code::from_value(10000));
}

TEST(PairingCode, OwnsItsDigitsAndWipesThemWhenMoved) {
  std::string input{"0042"};
  auto parsed = Code::parse(input);
  ASSERT_TRUE(parsed);
  input.assign("9999");
  auto independent = parsed.value().clone();
  auto moved = std::move(parsed).value();
  EXPECT_EQ(moved.view(), "0042");
  EXPECT_EQ(independent.view(), "0042");
  EXPECT_TRUE(std::ranges::all_of(parsed.value().view(),
                                  [](char value) { return value == '\0'; }));
  auto replacement = Code::from_value(9999);
  ASSERT_TRUE(replacement);
  independent = std::move(replacement).value();
  EXPECT_EQ(moved.view(), "0042");
}

}  // namespace
}  // namespace net_kakusan_test
