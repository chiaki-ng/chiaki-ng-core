#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string_view>

namespace users_test {
namespace {

using libchiaking::AccountId;

TEST(AccountIdDecimal, AcceptsLeadingZerosAndZero) {
  EXPECT_EQ(AccountId::parse("007"), AccountId{7});
  EXPECT_EQ(AccountId::parse("0"), AccountId{0});
  EXPECT_EQ(AccountId::parse("00000000000000000007"), AccountId{7});
  EXPECT_EQ(AccountId::parse("18446744073709551615"),
            AccountId{18446744073709551615U});
}

TEST(AccountIdDecimal, RefusesSignsSpacesAndOverflow) {
  for (const std::string_view refused :
       {"", "+7", "-7", " 7", "7 ", "7e0", "0x7", "18446744073709551616",
        "000000000000000000007"})
    EXPECT_FALSE(AccountId::parse(refused)) << refused;
}

TEST(AccountIdBase64, IgnoresWhiteSpaceAndNeedsNoPadding) {
  for (const std::string_view text :
       {"AQAAAAAAAAA=", "AQAAAAAAAAA", "AQAA AAAA\nAAA=", " AQAAAAAAAAA "})
    EXPECT_EQ(AccountId::from_base64(text), AccountId{1}) << text;
}

}  // namespace
}  // namespace users_test
