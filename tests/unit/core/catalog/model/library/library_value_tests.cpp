#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::Library;
using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::TitleNumber;

class LibraryOfOwnedTitles : public ValueCases {
 protected:
  std::vector<OwnedTitle> titles{
      owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud),
      owned(god_of_war, Platform::ps4, ServiceGeneration::old_service)};
  std::vector<Library::Bundle> bundles{
      {product(rivet), {product(rivet_deluxe)}}};
};

TEST_F(LibraryOfOwnedTitles, BelongsToOneAccount) {
  const auto built = Library::of(account, titles, bundles, at_ms(1000));
  ASSERT_TRUE(built);
  EXPECT_EQ(built->account(), account);
  EXPECT_EQ(built->size(), 2U);
  EXPECT_EQ(built->fetched_at(), at_ms(1000));
}

TEST_F(LibraryOfOwnedTitles, FindsWhatItHolds) {
  const auto built = Library::of(account, titles, bundles, at_ms(1000));
  ASSERT_TRUE(built);
  const auto& library = built.value();
  EXPECT_NE(library.find(*OwnershipId::parse(rivet)), nullptr);
  EXPECT_EQ(library.find(*OwnershipId::parse(show)), nullptr);
  EXPECT_NE(library.held_under(product(god_of_war)), nullptr);
  EXPECT_EQ(library.held_under(product(show)), nullptr);
  EXPECT_TRUE(library.holds_title(*TitleNumber::from("PPSA07028")));
  EXPECT_FALSE(library.holds_title(*TitleNumber::from("PPSA37004")));
  EXPECT_EQ(library.siblings_of(product(rivet)).size(), 1U);
  EXPECT_TRUE(library.siblings_of(product(show)).empty());
}

TEST_F(LibraryOfOwnedTitles, GrowsIntoACopyOnceForEachTitle) {
  const auto built = Library::of(account, titles, bundles, at_ms(1000));
  ASSERT_TRUE(built);
  const auto grown = built->with(
      owned(show, Platform::ps5, ServiceGeneration::ps5_cloud), at_ms(2000));
  EXPECT_EQ(grown.size(), 3U);
  EXPECT_EQ(grown.fetched_at(), at_ms(2000));
  EXPECT_EQ(built->size(), 2U);
  EXPECT_EQ(grown
                .with(owned(show, Platform::ps5, ServiceGeneration::ps5_cloud),
                      at_ms(3000))
                .size(),
            3U);
}

TEST_F(LibraryOfOwnedTitles, RefusesTheSameOwnershipTwice) {
  titles.push_back(owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud));
  EXPECT_EQ(Library::of(account, titles, {}, at_ms(1000)).error(),
            Library::Invalid::duplicate_ownership);
}

}  // namespace
}  // namespace catalog_test
