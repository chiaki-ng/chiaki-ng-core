#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace play_test {
namespace {

using libchiaking::play::AccessCode;

TEST(AccessCode, HoldsFourDigits) {
  EXPECT_EQ(AccessCode::digits, 4u);
  const auto code = AccessCode::parse("0420");
  ASSERT_TRUE(code);
  EXPECT_EQ(code->text(), "0420");
  ASSERT_EQ(code->bytes().size(), 4u);
  EXPECT_EQ(code->bytes()[0], std::byte{'0'});
  EXPECT_EQ(code->bytes()[1], std::byte{'4'});
  EXPECT_EQ(code->clone().text(), "0420");
}

TEST(AccessCode, RefusesAnythingButFourDigits) {
  EXPECT_FALSE(AccessCode::parse(""));
  EXPECT_FALSE(AccessCode::parse("042"));
  EXPECT_FALSE(AccessCode::parse("04200"));
  EXPECT_FALSE(AccessCode::parse("04a0"));
  EXPECT_FALSE(AccessCode::parse("04 0"));
  EXPECT_FALSE(AccessCode::parse("-420"));
}

}  // namespace
}  // namespace play_test
