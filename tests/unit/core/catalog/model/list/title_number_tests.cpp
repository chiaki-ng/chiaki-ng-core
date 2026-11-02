#include "tests/support/enum_printing.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::Platform;
using libchiaking::catalog::TitleKey;
using libchiaking::catalog::TitleNumber;

TEST(TitleNumberFrom, ReadsTheNumberInsideAnId) {
  EXPECT_EQ(TitleNumber::from("EP0109-PPSA07028_00-FAKE0110TITLE000"),
            "PPSA07028");
  EXPECT_EQ(TitleNumber::from("UP0114-CUSA07009_00-FAKE0051TITLE000"),
            "CUSA07009");
  EXPECT_EQ(TitleNumber::from("PPSAX-CUSA7"), "CUSA7");
}

TEST(TitleNumberFrom, RefusesIdsWithoutAPs4OrPs5Number) {
  EXPECT_FALSE(TitleNumber::from("EP0109-NPEA00001_00-OLD"));
  EXPECT_FALSE(TitleNumber::from("PPSA"));
  EXPECT_FALSE(TitleNumber::from(""));
}

TEST(TitleNumberFrom, KnowsThePlatformOfTheNumber) {
  EXPECT_EQ(
      TitleNumber::from("EP0109-PPSA07028_00-FAKE0110TITLE000")->platform(),
      Platform::ps5);
  EXPECT_EQ(
      TitleNumber::from("UP0114-CUSA07009_00-FAKE0051TITLE000")->platform(),
      Platform::ps4);
}

TEST(TitleNumberFrom, ComparesAndOrdersByNumber) {
  EXPECT_EQ(*TitleNumber::from("PPSA07028"),
            *TitleNumber::from("EP0109-PPSA07028_00-FAKE0087TITLE000"));
  EXPECT_LT(*TitleNumber::from("CUSA07009"), *TitleNumber::from("PPSA07028"));
}

TEST(TitleNumberPlatformMarkedIn, ReadsThePlatformAnIdNames) {
  EXPECT_EQ(TitleNumber::platform_marked_in("UP0114-CUSA07009_00-X"),
            Platform::ps4);
  EXPECT_EQ(TitleNumber::platform_marked_in("EP0109-PPSA07028_00-CUSA"),
            Platform::ps5);
  EXPECT_FALSE(TitleNumber::platform_marked_in("NPUB37001"));
}

TEST(TitleKeyOf, IsTheTitleNumberForAPs4OrPs5Id) {
  EXPECT_EQ(TitleKey::of("EP0109-PPSA07028_00-FAKE0110TITLE000"), "PPSA07028");
  EXPECT_EQ(TitleKey::of(std::string_view{"UP0114-CUSA07009_00\0junk", 24}),
            "CUSA07009");
}

TEST(TitleKeyOf, GroupsTheEditionsOfAnOlderTitle) {
  EXPECT_EQ(TitleKey::of("UP0114-NPUA87001_00-FAKE0084TITLE000"),
            "UP0114|NPUA87001|00");
  EXPECT_EQ(TitleKey::of("UP0114-NPUA87001_00-FAKE0084TITLE000"),
            TitleKey::of("UP0114-NPUA87001_00-FAKE0088TITLE000"));
}

TEST(TitleKeyOf, RefusesTextWithoutParts) {
  EXPECT_FALSE(TitleKey::of(""));
  EXPECT_FALSE(TitleKey::of("SINGLE"));
  EXPECT_FALSE(TitleKey::of("--__"));
}

}  // namespace
}  // namespace catalog_test
