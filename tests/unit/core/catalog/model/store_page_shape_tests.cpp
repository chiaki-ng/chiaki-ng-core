#include "tests/support/enum_printing.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::ProductId;
using libchiaking::catalog::StoreLocale;
using libchiaking::catalog::StorePage;

class StorePageShape : public ::testing::Test {
 protected:
  static constexpr std::string_view death_haulage =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";
  static constexpr std::string_view ghost_grant =
      "EP0109-CUSA37002_00-FAKE0050TITLE000";

  static auto locale(std::string_view text) -> StoreLocale {
    auto parsed = StoreLocale::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto product(std::string_view id) -> ProductId {
    auto parsed = ProductId::parse(id);
    EXPECT_TRUE(parsed) << id;
    return std::move(parsed).value();
  }

  static auto shaped(std::string_view id) -> bool {
    const auto parsed = ProductId::parse(id);
    return parsed &&
           StorePage::of_product(locale("en-GB"), *parsed).has_value();
  }

  static auto concept_at(std::string_view url) {
    return StorePage::at(url, StorePage::Source::concept_page);
  }
};

TEST_F(StorePageShape, LowercasesTheStoreLocaleForItsPath) {
  EXPECT_EQ(locale("en-GB").lowercase(), "en-gb");
  EXPECT_EQ(locale("de_DE").lowercase(), "de-de");
  EXPECT_EQ(locale("ja-jp").lowercase(), "ja-jp");
}

TEST_F(StorePageShape, RefusesALocaleThatIsNotLanguageAndCountry) {
  EXPECT_FALSE(StoreLocale::parse("en"));
  EXPECT_FALSE(StoreLocale::parse("en-GBR"));
  EXPECT_FALSE(StoreLocale::parse("e1-GB"));
  EXPECT_FALSE(StoreLocale::parse("en/GB"));
  EXPECT_FALSE(StoreLocale::parse("english"));
}

TEST_F(StorePageShape, PutsTheProductUnderTheStoreLocale) {
  const auto page =
      StorePage::of_product(locale("fr-FR"), product(death_haulage));
  ASSERT_TRUE(page);
  EXPECT_EQ(page->url(),
            "https://store.playstation.com/fr-fr/product/"
            "EP0109-PPSA07033_00-FAKE0030TITLE000");
}

TEST_F(StorePageShape, AcceptsProductIdsWithTheStoreShape) {
  EXPECT_TRUE(shaped(death_haulage));
  EXPECT_TRUE(shaped(ghost_grant));
}

TEST_F(StorePageShape, RefusesProductIdsWithoutTheStoreShape) {
  EXPECT_FALSE(shaped("EP0109-PPSA07033_00-DEATHSTRANDINGE"));
  EXPECT_FALSE(shaped("EP0109-PPSA07033_00-deathstrandingeu"));
  EXPECT_FALSE(shaped("EP0109-PPSA07033_00-DEATHSTRANDING/U"));
  EXPECT_FALSE(shaped("EP0109-PPSA07033-00-FAKE0030TITLE000"));
  EXPECT_FALSE(shaped("EP-CUSA7_00-UPGRADE"));
}

TEST_F(StorePageShape, RefusesAConceptPageOutsideTheStore) {
  EXPECT_FALSE(concept_at("https://evil.example/concept/1"));
  EXPECT_FALSE(concept_at("http://store.playstation.com/en-gb/concept/1"));
  EXPECT_FALSE(concept_at("https://store.playstation.com/"));
  EXPECT_FALSE(concept_at("https://store.playstation.com/en-gb/concept/1 x"));
  EXPECT_FALSE(concept_at("https://store.playstation.com/en-gb/concept/1\""));
}

}  // namespace
}  // namespace catalog_test
