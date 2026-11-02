#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace play_test {
namespace {

using libchiaking::play::DatacenterName;

class DatacenterNames : public DatacenterRig {};

TEST_F(DatacenterNames, KeepTheirText) {
  EXPECT_EQ(name("usw2").text(), "usw2");
  EXPECT_EQ(name("Frankfurt (eu-central)"), "Frankfurt (eu-central)");
}

TEST_F(DatacenterNames, RefuseEmptyTextAndTheAutomaticChoice) {
  EXPECT_FALSE(DatacenterName::parse(""));
  EXPECT_FALSE(DatacenterName::parse("Auto"));
  EXPECT_TRUE(DatacenterName::parse("auto"));
  EXPECT_TRUE(DatacenterName::parse("Auto "));
}

TEST_F(DatacenterNames, RefuseControlCharacters) {
  EXPECT_FALSE(DatacenterName::parse("us\nwest"));
  EXPECT_FALSE(DatacenterName::parse(std::string_view{"us\0west", 7}));
  EXPECT_FALSE(DatacenterName::parse("us\x7fwest"));
  EXPECT_FALSE(DatacenterName::parse("\x1f"));
  EXPECT_TRUE(DatacenterName::parse("M\xc3\xbcnchen"));
}

TEST_F(DatacenterNames, AreLimitedInLength) {
  EXPECT_TRUE(DatacenterName::parse(std::string(DatacenterName::limit, 'a')));
  EXPECT_FALSE(
      DatacenterName::parse(std::string(DatacenterName::limit + 1, 'a')));
}

TEST_F(DatacenterNames, CompareCaseSensitively) {
  EXPECT_LT(name("a"), name("b"));
  EXPECT_EQ(name("a"), name("a"));
  EXPECT_NE(name("a"), name("A"));
}

}  // namespace
}  // namespace play_test
