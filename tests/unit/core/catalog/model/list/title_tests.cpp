#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::ListStreamability;
using libchiaking::catalog::TitleNumber;

class ListedTitles : public ValueCases {
 protected:
  static auto rift() -> Title {
    Title::Facts facts{.product = product(rivet)};
    facts.name = "Rivet & Bolt: Split Worlds";
    facts.platform = Platform::ps5;
    facts.service = ServiceGeneration::ps5_cloud;
    facts.concept_id = libchiaking::catalog::ConceptId::parse("90000016");
    facts.aliases = {product(rivet_deluxe), product(rivet),
                     product(rivet_deluxe)};
    facts.concept_page = libchiaking::catalog::StorePage::at(
        "https://store.playstation.com/en-gb/concept/90000016",
        libchiaking::catalog::StorePage::Source::concept_page);
    return Title::from(std::move(facts)).value();
  }
};

TEST_F(ListedTitles, NeedAName) {
  Title::Facts nameless{.product = product(rivet)};
  nameless.name = "   ";
  EXPECT_EQ(Title::from(std::move(nameless)).error(), Title::Invalid::name);
}

TEST_F(ListedTitles, KeepTheirFacts) {
  Title::Facts facts{.product = product(rivet)};
  facts.name = "Rivet & Bolt: Split Worlds";
  facts.platform = Platform::ps5;
  facts.service = ServiceGeneration::ps5_cloud;
  facts.concept_id = libchiaking::catalog::ConceptId::parse("90000016");
  facts.concept_page = libchiaking::catalog::StorePage::at(
      "https://store.playstation.com/en-gb/concept/90000016",
      libchiaking::catalog::StorePage::Source::concept_page);
  const auto listed = Title::from(std::move(facts));
  ASSERT_TRUE(listed);
  EXPECT_EQ(listed->title_number(), *TitleNumber::from("PPSA07028"));
  EXPECT_EQ(listed->streamability(), ListStreamability::unknown);
  EXPECT_TRUE(listed->concept_page());
  ASSERT_TRUE(listed->concept_id());
  EXPECT_EQ(listed->concept_id()->text(), "90000016");
  EXPECT_EQ(listed->service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(listed->platform(), Platform::ps5);
}

TEST_F(ListedTitles, KeepEachAliasOnceAndNotTheirOwnProduct) {
  const auto listed = rift();
  ASSERT_EQ(listed.aliases().size(), 1U);
  EXPECT_EQ(listed.aliases()[0], product(rivet_deluxe));
  EXPECT_EQ(listed.with_alias(product(show)).aliases().size(), 2U);
  EXPECT_EQ(listed.with_alias(product(rivet)).aliases().size(), 1U);
}

TEST_F(ListedTitles, AreKnownByTheirProductAndAliases) {
  const auto listed = rift();
  EXPECT_TRUE(listed.is_known_as(product(rivet)));
  EXPECT_TRUE(listed.is_known_as(product(rivet_deluxe)));
  EXPECT_FALSE(listed.is_known_as(product(show)));
}

TEST_F(ListedTitles, TakeANewStreamability) {
  EXPECT_EQ(rift()
                .with_streamability(ListStreamability::marked_unstreamable)
                .streamability(),
            ListStreamability::marked_unstreamable);
}

}  // namespace
}  // namespace catalog_test
