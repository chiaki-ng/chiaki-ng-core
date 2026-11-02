#include "src/core/catalog/model/list/title_name.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::OwnedTitle;
using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::TitleName;

class TitleNameWords : public ::testing::Test {
 protected:
  static auto held(std::string_view name)
      -> libchiaking::Expected<OwnedTitle, OwnedTitle::Invalid> {
    OwnedTitle::Facts facts{
        .id = *OwnershipId::parse("EP0109-PPSA07028_00-FAKE0110TITLE000")};
    facts.name = std::string{name};
    return OwnedTitle::from(std::move(facts));
  }
};

TEST_F(TitleNameWords, IgnoreDemoAndTrialInsideOtherWords) {
  for (const auto* name :
       {"Wraith's Echo", "Demolition Racer", "Ramps Rising", "Path of Trials",
        "Rivet & Bolt: Split Worlds", "DEMONSTRATION", "Trialogue"}) {
    const TitleName title{name};
    EXPECT_FALSE(title.is_demo()) << name;
    EXPECT_FALSE(title.is_trial()) << name;
    EXPECT_FALSE(title.is_digital_extra()) << name;
    EXPECT_TRUE(held(name)) << name;
  }
}

TEST_F(TitleNameWords, MarkADemoAndRefuseItAsAnOwnedTitle) {
  EXPECT_TRUE(TitleName{"Foo Demo"}.is_demo());
  EXPECT_EQ(held("Foo Demo").error(), OwnedTitle::Invalid::demo_name);
  EXPECT_TRUE(TitleName{"DEMO: Grand Circuit\xE2\x84\xA2 7"}.is_demo());
  EXPECT_EQ(held("DEMO: Grand Circuit\xE2\x84\xA2 7").error(),
            OwnedTitle::Invalid::demo_name);
  EXPECT_TRUE(TitleName{"Racer DEMO"}.is_demo());
  EXPECT_FALSE(TitleName{"Wraith's Echo"}.is_demo());
}

TEST_F(TitleNameWords, MarkATrialAndRefuseItAsAnOwnedTitle) {
  EXPECT_TRUE(TitleName{"Foo Trial"}.is_trial());
  EXPECT_EQ(held("Foo Trial").error(), OwnedTitle::Invalid::trial_name);
  EXPECT_TRUE(TitleName{"Foo (Trial)"}.is_trial());
  EXPECT_EQ(held("Foo (Trial)").error(), OwnedTitle::Invalid::trial_name);
}

TEST_F(TitleNameWords, TreatANameWithBothWordsAsADemo) {
  EXPECT_EQ(held("Demo Trial").error(), OwnedTitle::Invalid::demo_name);
}

TEST(TitleNameHasWord, MatchesWholeWordsOnly) {
  EXPECT_TRUE(TitleName{"demo_x"}.has_word("demo"));
  EXPECT_FALSE(TitleName{"demo2"}.has_word("demo"));
  EXPECT_FALSE(TitleName{"demo"}.has_word(""));
}

TEST(TitleNameDigitalExtras, AreRecognisedByName) {
  for (const auto* extra :
       {"Odyssey Art Book", "Odyssey Artbook", "The Art of Odyssey",
        "Odyssey Soundtrack", "Digital Content Viewer", "Odyssey ContentViewer",
        "Odyssey Bonus Content", "Odyssey BonusContent"})
    EXPECT_TRUE(TitleName{extra}.is_digital_extra()) << extra;
}

TEST(TitleNameDigitalExtras, LeaveGamesAndBonusEditionsAlone) {
  EXPECT_FALSE(
      TitleName{"METAL GEAR SOLID: MASTER COLLECTION Vol.1 BONUS CONTENT"}
          .is_digital_extra());
  EXPECT_FALSE(TitleName{"The Bright Getaway"}.is_digital_extra());
  EXPECT_FALSE(TitleName{"Rivet"}.is_digital_extra());
}

TEST(TitleNameNormalized, LowersTrimsAndDropsMarks) {
  EXPECT_EQ(TitleName{"  Grand Circuit\xE2\x84\xA2\t7 \n"}.normalized(),
            "grand circuit 7");
  EXPECT_EQ(TitleName{"Skyline\xC2\xAE Zero Ember"}.normalized(),
            "skyline zero ember");
}

TEST(TitleNameNormalized, StopsAtItsLimit) {
  EXPECT_EQ(TitleName{std::string(400, 'A')}.normalized().size(),
            TitleName::normalized_limit);
}

TEST(TitleNameSquashed, KeepsOnlyLatinLettersAndDigits) {
  EXPECT_EQ(TitleName{"Rivet & Bolt: Split Worlds"}.squashed(),
            "rivetboltsplitworlds");
  EXPECT_EQ(TitleName{"Rivet &amp; Bolt Split Worlds"}.squashed(),
            "rivetampboltsplitworlds");
  EXPECT_EQ(TitleName{"Rivet & Bolt: Split Worlds \xE3\x83\xA9\xE3\x83\x81"}
                .squashed(),
            "rivetboltsplitworlds");
}

TEST(TitleNameSquashed, StopsAtItsLimit) {
  EXPECT_EQ(TitleName{std::string(400, 'a')}.squashed().size(),
            TitleName::squashed_limit);
}

TEST(TitleNameWithoutEditionWords, DropsEditionWords) {
  EXPECT_EQ(
      TitleName{"Storm of Ages Digital Deluxe Edition"}.without_edition_words(),
      "stormofages");
  EXPECT_EQ(TitleName{"STORM OF AGES"}.without_edition_words(), "stormofages");
  EXPECT_EQ(TitleName{"Game: Gold-Standard GOTY Legendary Special Ultimate "
                      "Premium Complete"}
                .without_edition_words(),
            "game");
  EXPECT_EQ(TitleName{"Grand Circuit\xE2\x84\xA2 7"}.without_edition_words(),
            "grandcircuit7");
}

TEST(TitleNameWithoutEditionWords, KeepsEditionWordsInsideOtherWords) {
  EXPECT_EQ(TitleName{"Deluxeville"}.without_edition_words(), "deluxeville");
}

TEST(TitleNameWithoutEditionWords, KeepsOtherScripts) {
  EXPECT_EQ(TitleName{"Rivet \xE3\x83\xA9\xE3\x83\x81 Edition"}
                .without_edition_words(),
            "rivet\xE3\x83\xA9\xE3\x83\x81");
}

}  // namespace
}  // namespace catalog_test
