#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::ConceptId;
using libchiaking::catalog::Library;
using libchiaking::catalog::OwnershipId;

class LibraryLearnedConcepts : public ::testing::Test, protected RuleCases {
 protected:
  static constexpr std::string_view ghost_ship =
      "EP0109-CUSA37002_00-FAKE0050TITLE000";
  static constexpr std::string_view ghost_reserved =
      "EP0109-CUSA37002_00-FAKE0107TITLE000";
  static constexpr std::string_view ghost_ps5 =
      "EP0109-PPSA07036_00-FAKE0049TITLE000";
  static constexpr std::string_view street_chase =
      "EP0103-CUSA07008_00-FAKE0054TITLE000";

  const libchiaking::catalog::StreamableList list = list_of({
      {.product = ghost_ps5, .name = "Ghost of Isles", .concept_id = "900001"},
      on_old_service(street_chase, "Street Chase V"),
  });
  const Library plain = library_of({
      {.id = ghost_reserved,
       .product = ghost_ship,
       .name = "Ghost of Isles",
       .platform = "ps4",
       .package = "PS4GS"},
      {.id = "EP0103-CUSA07008_00-FAKE0107TITLE000",
       .product = street_chase,
       .name = "Street Chase V",
       .platform = "ps4",
       .package = "PS4GS"},
  });
  const std::vector<Library::LearnedConcept> learned{
      {OwnershipId::parse(ghost_reserved), {}, *ConceptId::parse("900001")},
      {{}, product(street_chase), *ConceptId::parse("900003")},
      {OwnershipId::parse("EP0100-CUSA99999_00-FAKE0081TITLE000"),
       {},
       *ConceptId::parse("5")},
  };
  const Library taught = plain.with_learned_concepts(learned);
};

TEST_F(LibraryLearnedConcepts, AttachToTheCopiesTheyName) {
  EXPECT_EQ(taught.copy_concepts().size(), 2U);
  EXPECT_EQ(taught.concept_of_copy(product(ghost_ship)),
            ConceptId::parse("900001"));
  EXPECT_EQ(taught.concept_of_copy(product(street_chase)),
            ConceptId::parse("900003"));
  EXPECT_TRUE(std::ranges::equal(taught.titles(), plain.titles()));
  EXPECT_TRUE(std::ranges::equal(taught.bundles(), plain.bundles()));
}

TEST_F(LibraryLearnedConcepts, NeverReachTheMatcher) {
  const auto games_before = games_of(plain, list);
  const auto games_after = games_of(taught, list);
  EXPECT_EQ(games_before, games_after);
  EXPECT_EQ(games_after.find(product(ghost_ship)), nullptr);
}

TEST_F(LibraryLearnedConcepts, AreIgnoredForACopyThatCarriesItsOwnConcept) {
  const auto attached = library_of({{.id = ghost_reserved,
                                     .product = ghost_ship,
                                     .name = "Ghost of Isles",
                                     .platform = "ps4",
                                     .package = "PS4GS",
                                     .concept_id = "900001"}});
  EXPECT_FALSE(games_of(attached, list) == games_of(plain, list));
  EXPECT_TRUE(attached.with_learned_concepts(learned).copy_concepts().empty());
}

TEST_F(LibraryLearnedConcepts, SurviveAnUpdatedTitle) {
  EXPECT_EQ(taught.with(plain.titles().front(), {}).copy_concepts().size(), 2U);
}

}  // namespace
}  // namespace catalog_test
