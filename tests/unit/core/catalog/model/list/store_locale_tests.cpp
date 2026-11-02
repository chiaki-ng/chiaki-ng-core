#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::StoreLocale;
using libchiaking::users::Region;

class StoreLocaleChains : public ::testing::Test {
 protected:
  static auto chain_is(std::span<const StoreLocale> chain,
                       std::initializer_list<std::string_view> expected)
      -> testing::AssertionResult {
    if (chain.size() != expected.size())
      return testing::AssertionFailure()
             << "the chain holds " << chain.size() << " locales, expected "
             << expected.size();
    std::size_t index{};
    for (const auto item : expected) {
      if (chain[index].text() != item)
        return testing::AssertionFailure()
               << "locale " << index << " is " << chain[index].text()
               << ", expected " << item;
      ++index;
    }
    return testing::AssertionSuccess();
  }

  static auto parsed_chain_is(std::string_view input,
                              std::initializer_list<std::string_view> expected)
      -> testing::AssertionResult {
    const auto locale = StoreLocale::parse(input);
    if (!locale) return testing::AssertionFailure() << input << " was refused";
    return chain_is(locale->fallbacks(), expected) << " for " << input;
  }

  static auto account_chain_is(const Region& region,
                               std::initializer_list<std::string_view> expected)
      -> testing::AssertionResult {
    const auto locale = StoreLocale::for_account(region);
    if (!locale) return testing::AssertionFailure() << "the region was refused";
    return chain_is(locale->fallbacks(), expected);
  }
};

TEST_F(StoreLocaleChains, FallBackToEnglishThenTheUnitedStates) {
  EXPECT_TRUE(parsed_chain_is("en-US", {"en-US"}));
  EXPECT_TRUE(parsed_chain_is("en-GB", {"en-GB", "en-US"}));
  EXPECT_TRUE(parsed_chain_is("de-DE", {"de-DE", "en-DE", "en-US"}));
  EXPECT_TRUE(parsed_chain_is("zh-Hans-HK", {"zh-hans-HK", "en-HK", "en-US"}));
  EXPECT_EQ(StoreLocale::english_us().fallbacks().size(), 1U);
}

TEST_F(StoreLocaleChains, AreReadThroughSpacesCaseAndUnderscores) {
  EXPECT_TRUE(parsed_chain_is("   EN-gb", {"en-GB", "en-US"}));
  EXPECT_TRUE(parsed_chain_is("De-dE  ", {"de-DE", "en-DE", "en-US"}));
  EXPECT_TRUE(parsed_chain_is("fr_FR", {"fr-FR", "en-FR", "en-US"}));
}

TEST_F(StoreLocaleChains, AreSettledByAnAccountRegion) {
  EXPECT_TRUE(account_chain_is(Region{"US", "en"}, {"en-US"}));
  EXPECT_TRUE(account_chain_is(Region{"gb", "EN"}, {"en-GB", "en-US"}));
  EXPECT_TRUE(
      account_chain_is(Region{"DE", "de-DE"}, {"de-DE", "en-DE", "en-US"}));
  EXPECT_TRUE(
      account_chain_is(Region{"HK", "zh-Hans"}, {"zh-HK", "en-HK", "en-US"}));
}

TEST_F(StoreLocaleChains, UseEnglishForAnAccountWithoutAUsableLanguage) {
  EXPECT_TRUE(account_chain_is(Region{"FR", ""}, {"en-FR", "en-US"}));
  EXPECT_TRUE(account_chain_is(Region{"FR", "??"}, {"en-FR", "en-US"}));
}

TEST(StoreLocaleParse, RefusesTextThatIsNotALocale) {
  for (const auto* text :
       {"", "  \t ", "De -dE  ", "abcdefghij-KLMNOPQR", "-FR", "fr-", "fr",
        "f-FR", "fr-F", "fr-FRA", "f1-FR", "fr-F1", "-fr-FR", "en--US-"})
    EXPECT_FALSE(StoreLocale::parse(text)) << text;
  EXPECT_FALSE(StoreLocale::parse(std::string_view{"fr-FR\0extra", 11}));
}

TEST(StoreLocaleForAccount, RefusesARegionThatIsNotACountry) {
  EXPECT_FALSE(StoreLocale::for_account(Region{"", "fr"}));
  EXPECT_FALSE(StoreLocale::for_account(Region{"FRA", "fr"}));
  EXPECT_FALSE(StoreLocale::for_account(Region{"F1", "fr"}));
}

TEST(StoreLocaleParts, AreReadInTheirCanonicalCase) {
  const auto british = *StoreLocale::parse("en-gb");
  EXPECT_EQ(british.language(), "en");
  EXPECT_EQ(british.country(), "GB");
  EXPECT_EQ(british.text(), "en-GB");
  EXPECT_EQ(british.lowercase(), "en-gb");
  EXPECT_EQ(StoreLocale::english_us().text(), "en-US");
}

TEST(StoreLocaleParts, CompareByLocale) {
  const auto british = *StoreLocale::parse("en-gb");
  EXPECT_EQ(british, *StoreLocale::parse("EN-GB"));
  EXPECT_NE(british, StoreLocale::english_us());
}

TEST(StoreLocaleParts, GiveTheEnglishOfTheSameCountry) {
  const auto british = *StoreLocale::parse("en-gb");
  EXPECT_EQ(british.english(), british);
  EXPECT_EQ(StoreLocale::parse("de-DE")->english(),
            *StoreLocale::parse("en-DE"));
}

TEST(StoreLocaleParts, KnowWhichCountryTheyAreIn) {
  const auto british = *StoreLocale::parse("en-gb");
  EXPECT_TRUE(british.is_in_country_of(Region{"gb", "fr"}));
  EXPECT_FALSE(british.is_in_country_of(Region{"US", "en"}));
}

TEST(StoreLocaleParts, ShareTheUsStoreInTheAmericasAndGbElsewhere) {
  EXPECT_EQ(StoreLocale::shared_store_country("ca"), "US");
  EXPECT_EQ(StoreLocale::shared_store_country("HU"), "GB");
}

}  // namespace
}  // namespace catalog_test
