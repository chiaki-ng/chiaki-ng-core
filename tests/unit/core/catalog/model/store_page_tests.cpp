#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::StoreLocale;
using libchiaking::catalog::StorePage;

class StorePageLinks : public ValueCases {
 protected:
  StoreLocale british = *StoreLocale::parse("en-GB");
};

TEST_F(StorePageLinks, PointAtAProductPage) {
  const auto page = StorePage::of_product(british, product(rivet));
  ASSERT_TRUE(page);
  EXPECT_EQ(page->source(), StorePage::Source::product);
  EXPECT_EQ(page->url(),
            "https://store.playstation.com/en-gb/product/"
            "EP0109-PPSA07028_00-FAKE0110TITLE000");
}

TEST_F(StorePageLinks, AreNotMadeForASkuOrALowercaseProduct) {
  EXPECT_FALSE(StorePage::of_product(
      british, product("UP0112-PPSA01234_00-GAME000000000000-E001")));
  EXPECT_FALSE(StorePage::of_product(
      british, product("ep9000-PPSA07028_00-FAKE0110TITLE000")));
}

TEST_F(StorePageLinks, AcceptAConceptPageOnTheStore) {
  const auto concept_page =
      StorePage::at("https://store.playstation.com/en-gb/concept/90000016",
                    StorePage::Source::concept_page);
  ASSERT_TRUE(concept_page);
  EXPECT_EQ(concept_page->source(), StorePage::Source::concept_page);
}

TEST_F(StorePageLinks, RefuseAnythingElse) {
  for (const auto* url :
       {"", "https://store.playstation.com/",
        "http://store.playstation.com/en-gb/concept/1",
        "https://store.playstation.com.evil.example/en-gb/concept/1",
        "https://user:secret@store.playstation.com/en-gb/concept/1",
        "https://store.playstation.com/en-gb/concept/1 2",
        "https://store.playstation.com/<script>"})
    EXPECT_FALSE(StorePage::at(url, StorePage::Source::concept_page)) << url;
  EXPECT_FALSE(StorePage::at(
      std::string{StorePage::origin} + std::string(StorePage::url_limit, 'a'),
      StorePage::Source::concept_page));
}

}  // namespace
}  // namespace catalog_test
