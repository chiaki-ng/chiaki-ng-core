#include "tests/unit/core/consoles/model/search/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::SearchRanges;

TEST(SearchRanges, AreEmptyByDefault) {
  const SearchRanges none;
  EXPECT_TRUE(none.subnets().empty());
  EXPECT_FALSE(none.unicast());
  EXPECT_EQ(none.target_count(), 0U);
}

TEST(SearchRanges, MergeTheSameSubnetAndCountTheirTargets) {
  const std::vector<std::string> two{"192.168.1.0/24", "192.168.1.9/24",
                                     "10.0.0.8/30"};
  const auto ranges = SearchRanges::parse(two, true);
  ASSERT_TRUE(ranges);
  EXPECT_TRUE(ranges.value().unicast());
  EXPECT_EQ(ranges.value().subnets().size(), 2U);
  EXPECT_EQ(ranges.value().target_count(), 256U);
  EXPECT_TRUE(ranges.value() == SearchRanges::parse(two, true).value());
  EXPECT_FALSE(ranges.value() == SearchRanges::parse(two, false).value());
}

TEST(SearchRanges, RefuseAnUnreadableSubnet) {
  const std::vector<std::string> bad{"192.168.1.0/24", "nonsense"};
  EXPECT_EQ(SearchRanges::parse(bad, false).error(),
            SearchRanges::Invalid::subnet);
}

TEST(SearchRanges, HoldUpToTheSubnetLimit) {
  std::vector<std::string> many;
  for (std::size_t index = 0; index <= SearchRanges::subnet_limit; ++index)
    many.push_back("10.0." + std::to_string(index) + ".1/32");
  EXPECT_EQ(SearchRanges::parse(many, false).error(),
            SearchRanges::Invalid::too_many_subnets);
  many.pop_back();
  const auto full = SearchRanges::parse(many, false);
  ASSERT_TRUE(full);
  EXPECT_EQ(full.value().subnets().size(), SearchRanges::subnet_limit);
  EXPECT_EQ(full.value().target_count(), SearchRanges::subnet_limit);
}

TEST(SearchRanges, HoldUpToTheTargetLimit) {
  std::vector<std::string> wide;
  for (std::size_t index = 0; index < 4; ++index)
    wide.push_back("10.1." + std::to_string(index) + ".0/24");
  const auto fits = SearchRanges::parse(wide, true);
  ASSERT_TRUE(fits);
  EXPECT_EQ(fits.value().target_count(), 1016U);
  wide.push_back("10.1.4.0/28");
  EXPECT_EQ(SearchRanges::parse(wide, true).error(),
            SearchRanges::Invalid::too_many_targets);
  wide.back() = "10.1.4.0/29";
  EXPECT_EQ(SearchRanges::parse(wide, true).value().target_count(), 1022U);
}

}  // namespace
}  // namespace consoles_test
