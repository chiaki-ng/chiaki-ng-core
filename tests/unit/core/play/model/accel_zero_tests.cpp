#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using libchiaking::controllers::AccelZero;

TEST(AccelZero, OffsetsGravityOnlyWhenInactiveAndAsked) {
  auto zero = AccelZero::inactive(true);
  EXPECT_EQ(zero.offset().x, 0.0F);
  EXPECT_EQ(zero.offset().y, 1.0F);
  EXPECT_EQ(zero.offset().z, 0.0F);
  zero = AccelZero::inactive(false);
  EXPECT_EQ(zero.offset().y, 0.0F);
  EXPECT_EQ(AccelZero{}.offset().y, 0.0F);
}

TEST(AccelZero, OffsetsTheRestingReadingLeavingGravityUnlessAsked) {
  auto zero = AccelZero::active({0.25F, 3.0F, 0.5F}, false);
  EXPECT_EQ(zero.offset().x, 0.25F);
  EXPECT_EQ(zero.offset().y, 2.0F);
  EXPECT_EQ(zero.offset().z, 0.5F);
  zero = AccelZero::active({0.25F, 3.0F, 0.5F}, true);
  EXPECT_EQ(zero.offset().y, 3.0F);
}

TEST(AccelZero, RemovesItsOffsetFromAReading) {
  const auto zero = AccelZero::active({0.25F, 3.0F, 0.5F}, true);
  const auto removed = zero.removed_from({1.25F, 4.0F, 0.5F});
  EXPECT_EQ(removed.x, 1.0F);
  EXPECT_EQ(removed.y, 1.0F);
  EXPECT_EQ(removed.z, 0.0F);
}

}  // namespace
}  // namespace play_test
