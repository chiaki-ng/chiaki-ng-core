#include "src/core/consoles/model/console/wake_credential.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::WakeCredential;

static_assert(WakeCredential::maximum_digits == 8);
static_assert(!std::is_copy_constructible_v<WakeCredential>);

class WakeCredentialFromKey : public ::testing::Test {
 protected:
  static auto key(std::string_view text) -> std::array<std::byte, 16> {
    std::array<std::byte, 16> bytes{};
    for (std::size_t index = 0; index < text.size(); ++index)
      bytes[index] = static_cast<std::byte>(text[index]);
    return bytes;
  }
};

TEST_F(WakeCredentialFromKey, ReadsTheRegistrationKeyAsHex) {
  const auto parsed = WakeCredential::from_registration_key(key("3a5f9c1e"));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->value(), 0x3a5f9c1eU);
}

TEST_F(WakeCredentialFromKey, ReadsAShortUppercaseKey) {
  const auto parsed = WakeCredential::from_registration_key(key("1F"));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->value(), 0x1fU);
}

TEST_F(WakeCredentialFromKey, RefusesMoreThanEightDigits) {
  EXPECT_FALSE(WakeCredential::from_registration_key(key("3a5f9c1e0")));
}

TEST_F(WakeCredentialFromKey, RefusesANonHexDigit) {
  EXPECT_FALSE(WakeCredential::from_registration_key(key("z")));
}

TEST_F(WakeCredentialFromKey, RefusesAnEmptyKey) {
  EXPECT_FALSE(WakeCredential::from_registration_key(key("")));
  EXPECT_FALSE(WakeCredential::from_registration_key({}));
}

TEST(WakeCredential, LeavesZeroBehindWhenMoved) {
  WakeCredential held{42};
  const WakeCredential moved{std::move(held)};
  EXPECT_EQ(moved.value(), 42U);
  EXPECT_EQ(held.value(), 0U);
}

}  // namespace
}  // namespace consoles_test
