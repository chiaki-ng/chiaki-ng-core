#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HeldAs;
using catalog::ReleaseDate;
using catalog::ServiceGeneration;

class OwnedLibrary : public ::testing::Test, public RuleCases {
 protected:
  static constexpr std::string_view classic =
      "UP0114-CUSA07031_00-FAKE0106TITLE000";

  static auto ownership(std::string_view id) -> catalog::OwnershipId {
    return *catalog::OwnershipId::parse(id);
  }

  static auto held_as(const catalog::Library& library, std::string_view id)
      -> std::optional<HeldAs> {
    const auto* title = library.find(ownership(id));
    if (title == nullptr) return std::nullopt;
    return title->held_as();
  }
};

TEST_F(OwnedLibrary, LeavesOutRecordsThatAreNotGames) {
  const auto library = library_of({
      {.id = "EP-PPSA00001_00-NETFLIX",
       .product = "EP-PPSA00001_00-NETFLIX",
       .name = "Netflix",
       .package = "PSMEDIA"},
      {.id = "EP-PPSA00002_00-ARTBOOK",
       .product = "EP-PPSA00002_00-GAME",
       .name = "Some Game Artbook",
       .package = "PSGD"},
      {.id = "EP-PPSA00003_00-TRACK",
       .product = "EP-PPSA00003_00-GAME",
       .name = "Real Game",
       .package = "PSTRACK"},
  });
  EXPECT_EQ(library.size(), 0U);
  EXPECT_EQ(library.account(), account);
  EXPECT_EQ(library.fetched_at(), at_ms(library_fetched_ms));
  EXPECT_EQ(library.siblings_of(product("EP-PPSA00002_00-GAME")).size(), 1U);
}

TEST_F(OwnedLibrary, HoldsOneTitlePerOwnership) {
  const auto owned = library_of({
      {.id = "UP0114-PPSA07043_00-FAKE0043TITLE000",
       .product = "UP0114-PPSA07043_00-FAKE0010TITLE000",
       .name = "Synthetic Odyssey"},
      {.id = classic,
       .product = classic,
       .name = "Classic Quest",
       .platform = "ps4",
       .package = "PS4GD"},
      {.id = "UP0114-PPSA07043_00-FAKE0043TITLE000",
       .product = "UP0114-PPSA07043_00-FAKE0043TITLE000",
       .name = "Synthetic Odyssey",
       .active_date = "2025-03-14T00:00:00.000Z"},
      {.id = "UP0114-PPSA07042_00-FAKE0065TITLE000",
       .product = "UP0114-PPSA07042_00-FAKE0065TITLE000",
       .name = "Inactive Game",
       .active = false},
  });
  ASSERT_EQ(owned.size(), 2U);
  const auto& odyssey = owned.titles()[0];
  EXPECT_EQ(odyssey.id(),
            std::string_view{"UP0114-PPSA07043_00-FAKE0043TITLE000"});
  EXPECT_EQ(odyssey.product_id(),
            product("UP0114-PPSA07043_00-FAKE0043TITLE000"));
  EXPECT_TRUE(odyssey.release().is_unrestricted());
  EXPECT_EQ(odyssey.held_as(), HeldAs::full_game);
  EXPECT_EQ(odyssey.service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(owned.titles()[1].id(), classic);
  EXPECT_EQ(owned.titles()[1].service(), ServiceGeneration::old_service);
  EXPECT_EQ(owned.titles()[1].platform(), catalog::Platform::ps4);
  EXPECT_EQ(
      owned.siblings_of(product("UP0114-PPSA07043_00-FAKE0010TITLE000")).size(),
      1U);
  const auto siblings = owned.siblings_of(product(classic));
  ASSERT_FALSE(siblings.empty());
  EXPECT_EQ(siblings.front(), product(classic));
}

TEST_F(OwnedLibrary, KeepsTheEarliestReleaseOfAnOwnershipsRecords) {
  const auto library = library_of({
      {.id = "UP0114-PPSA07048_00-FAKE0068TITLE000",
       .product = "UP0114-PPSA07048_00-FAKE0068TITLE000",
       .name = "Launch Day",
       .preorder = true},
      {.id = "UP0114-PPSA07048_00-FAKE0068TITLE000",
       .product = "UP0114-PPSA07048_00-FAKE0069TITLE000",
       .name = "Launch Day",
       .active_date = "2026-11-20T00:00:00.000Z",
       .preorder = true},
      {.id = "UP0114-PPSA07049_00-FAKE0048TITLE000",
       .product = "UP0114-PPSA07049_00-FAKE0048TITLE000",
       .name = "Garbled",
       .active_date = "next tuesday"},
      {.id = "UP0114-PPSA07045_00-FAKE0097TITLE000",
       .product = "UP0114-PPSA07045_00-FAKE0097TITLE000",
       .name = "Next Ember",
       .preorder = true},
  });
  EXPECT_EQ(library.size(), 3U);
  const auto launch = ReleaseDate::parse_time("2026-11-20T00:00:00Z");
  ASSERT_TRUE(launch);
  const auto* day =
      library.find(ownership("UP0114-PPSA07048_00-FAKE0068TITLE000"));
  ASSERT_NE(day, nullptr);
  EXPECT_EQ(day->release(), ReleaseDate::at(*launch));
  EXPECT_EQ(day->product_id(), product("UP0114-PPSA07048_00-FAKE0068TITLE000"));
  const auto* garbled =
      library.find(ownership("UP0114-PPSA07049_00-FAKE0048TITLE000"));
  ASSERT_NE(garbled, nullptr);
  EXPECT_TRUE(garbled->release().is_unannounced());
  const auto* preorder =
      library.find(ownership("UP0114-PPSA07045_00-FAKE0097TITLE000"));
  ASSERT_NE(preorder, nullptr);
  EXPECT_TRUE(preorder->release().is_unannounced());
}

TEST_F(OwnedLibrary, OwnsAnyActiveGameRecordHoweverItWasObtained) {
  const auto library = library_of({
      {.id = "UP0114-PPSA07050_00-FAKE0020TITLE000",
       .product = "UP0114-PPSA07050_00-FAKE0020TITLE000",
       .name = "Claimed With Plus",
       .feature = OwnedRecord::trial_access_feature},
      {.id = "UP0114-PPSA07051_00-FAKE0009TITLE000",
       .product = "UP0114-PPSA07051_00-FAKE0009TITLE000",
       .name = "Bought"},
      {.id = "UP0114-PPSA07052_00-FAKE0114TITLE000",
       .product = "UP0114-PPSA07052_00-FAKE0114TITLE000",
       .name = "Redeemed",
       .feature = 7},
      {.id = "UP0114-CUSA07038_00-FAKE0136TITLE000",
       .product = "UP0114-CUSA07038_00-FAKE0136TITLE000",
       .name = "Upgraded",
       .feature = OwnedRecord::disc_upgrade_feature},
      {.id = "UP0114-PPSA07053_00-FAKE0067TITLE000",
       .product = "UP0114-PPSA07053_00-FAKE0067TITLE000",
       .name = "Lapsed",
       .active = false},
  });
  EXPECT_EQ(library.size(), 4U);
  EXPECT_EQ(held_as(library, "UP0114-PPSA07050_00-FAKE0020TITLE000"),
            HeldAs::trial_access);
  EXPECT_EQ(held_as(library, "UP0114-PPSA07051_00-FAKE0009TITLE000"),
            HeldAs::full_game);
  EXPECT_EQ(held_as(library, "UP0114-PPSA07052_00-FAKE0114TITLE000"),
            HeldAs::other_game);
  EXPECT_EQ(held_as(library, "UP0114-CUSA07038_00-FAKE0136TITLE000"),
            HeldAs::disc_upgrade);
  EXPECT_EQ(library.find(ownership("UP0114-PPSA07053_00-FAKE0067TITLE000")),
            nullptr);
}

}  // namespace
}  // namespace catalog_test
