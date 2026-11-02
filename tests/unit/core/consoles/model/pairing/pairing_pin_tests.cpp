#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <string_view>
#include <type_traits>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::PairingPin;

static_assert(PairingPin::maximum == 99'999'999);
static_assert(!std::is_default_constructible_v<PairingPin>);
static_assert(!std::is_copy_constructible_v<PairingPin> &&
              std::is_move_constructible_v<PairingPin>);

TEST(PairingPin, ReadsEightDigits) {
  const auto pin = PairingPin::parse("01234567");
  ASSERT_TRUE(pin);
  EXPECT_EQ(pin->value(), 1'234'567U);
  const auto highest = PairingPin::parse("99999999");
  ASSERT_TRUE(highest);
  EXPECT_EQ(highest->value(), PairingPin::maximum);
}

TEST(PairingPin, RefusesAnythingButEightDigits) {
  for (const std::string_view refused :
       {"1234567", "123456789", "1234 567", "1234567a", ""})
    EXPECT_FALSE(PairingPin::parse(refused)) << refused;
}

TEST(PairingPin, IsCreatedUpToTheMaximum) {
  EXPECT_TRUE(PairingPin::create(0));
  EXPECT_TRUE(PairingPin::create(PairingPin::maximum));
  EXPECT_FALSE(PairingPin::create(PairingPin::maximum + 1));
}

TEST(PairingPin, HoldsEveryEightDigitValue) {
  EXPECT_EQ(PairingPin::create(0).value().value(), 0U);
  EXPECT_EQ(PairingPin::create(99'999'999).value().value(), 99'999'999U);
  EXPECT_FALSE(PairingPin::create(100'000'000));
}

TEST(PairingPin, LeavesZeroBehindWhenMovedByValue) {
  auto pin = PairingPin::create(12345678).value();
  const PairingPin moved{std::move(pin)};
  EXPECT_EQ(moved.value(), 12345678U);
  EXPECT_EQ(pin.value(), 0U);
}

TEST(PairingPin, LeavesZeroBehindWhenMoved) {
  auto held = PairingPin::create(12'345'678);
  ASSERT_TRUE(held);
  PairingPin moved{std::move(*held)};
  EXPECT_EQ(moved.value(), 12'345'678U);
  EXPECT_EQ(held->value(), 0U);
}

}  // namespace
}  // namespace consoles_test
