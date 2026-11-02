#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string_view>

namespace users_test {
namespace {

using libchiaking::NpTitleId;

TEST(NpTitleId, IsReadAsPsnSpellsIt) {
  for (const std::string_view text :
       {"PPSA07028_00", "CUSA01234_00", "NPUB30024_01"}) {
    const auto id = NpTitleId::parse(text);
    ASSERT_TRUE(id) << text;
    EXPECT_EQ(id->text(), text);
    EXPECT_EQ(*id, text);
  }
}

TEST(NpTitleId, RefusesAnythingElse) {
  for (const std::string_view text :
       {"", "PPSA07028", "ppsa07028_00", "PPSA0702_800", "PPSA07028-00",
        "PPSA07028_0", "PPSA07028_000", "PPS107028_00", "UP0114-PPSA07028_00",
        " PPSA07028_00"})
    EXPECT_FALSE(NpTitleId::parse(text)) << text;
}

TEST(NpTitleId, ComparesAndOrdersByText) {
  const auto first = NpTitleId::parse("CUSA01234_00");
  const auto second = NpTitleId::parse("PPSA07028_00");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_LT(*first, *second);
  EXPECT_NE(*first, *second);
  const std::set ids{*second, *first, *NpTitleId::parse("CUSA01234_00")};
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(*ids.begin(), *first);
}

}  // namespace
}  // namespace users_test
