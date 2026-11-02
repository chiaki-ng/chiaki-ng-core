#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string_view>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::NotAGame;
using catalog::Platform;
using catalog::ServiceGeneration;

class OwnedRecordRules : public ::testing::Test, public RuleCases {
 protected:
  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view classic =
      "UP0114-CUSA07031_00-FAKE0106TITLE000";

  static auto ps5_record(std::string_view name,
                         std::string_view package = "PS5GD", int feature = 3,
                         bool active = true, std::string_view sku = {})
      -> OwnedRecord {
    return record({.id = rivet,
                   .product = rivet,
                   .name = name,
                   .feature = feature,
                   .package = package,
                   .active = active,
                   .sku = sku});
  }

  static auto old_record(std::string_view name,
                         std::string_view package = "PS4GD") -> OwnedRecord {
    return record({.id = classic,
                   .product = classic,
                   .name = name,
                   .platform = "ps4",
                   .package = package});
  }

  static auto classic_on(std::string_view platform) -> OwnedRecord {
    return record({.id = classic,
                   .product = classic,
                   .name = "Classic",
                   .platform = platform});
  }
};

TEST_F(OwnedRecordRules, IgnoreDemoAndTrialInsideOtherWords) {
  for (const auto* name :
       {"Wraith's Echo", "Demolition Racer", "Ramps Rising", "Path of Trials",
        "Rivet & Bolt: Split Worlds", "DEMONSTRATION", "Trialogue"}) {
    EXPECT_FALSE(old_record(name).why_not_a_game()) << name;
    EXPECT_FALSE(ps5_record(name).why_not_a_game()) << name;
  }
  EXPECT_FALSE(ps5_record("Demolition Racer").why_not_a_game());
}

TEST_F(OwnedRecordRules, RefuseDemoAndTrialNamesOnEitherService) {
  EXPECT_EQ(old_record("Foo Demo").why_not_a_game(), NotAGame::demo_name);
  EXPECT_EQ(ps5_record("Foo Demo").why_not_a_game(), NotAGame::demo_name);
  EXPECT_EQ(old_record("Foo Trial").why_not_a_game(), NotAGame::trial_name);
  EXPECT_EQ(old_record("DEMO: Grand Circuit\xE2\x84\xA2 7").why_not_a_game(),
            NotAGame::demo_name);
  EXPECT_EQ(ps5_record("Foo (Trial)").why_not_a_game(), NotAGame::trial_name);
  EXPECT_EQ(ps5_record("Plus Pick Demo").why_not_a_game(), NotAGame::demo_name);
}

TEST_F(OwnedRecordRules, RefuseARecordWithoutGameDetails) {
  EXPECT_EQ(record({.id = rivet, .product = "", .name = "", .details = false})
                .why_not_a_game(),
            NotAGame::no_game_details);
  EXPECT_FALSE(old_record("Classic").why_not_a_game());
}

TEST_F(OwnedRecordRules, RefuseInactiveAddOnAndTrialRecords) {
  EXPECT_EQ(ps5_record("Rivet", "PS5GD", 3, false).why_not_a_game(),
            NotAGame::inactive);
  EXPECT_EQ(ps5_record("Costume Pack", "PS5AC", 0).why_not_a_game(),
            NotAGame::add_on);
  EXPECT_EQ(ps5_record("Rivet", "ps5gt", 1).why_not_a_game(),
            NotAGame::trial_package);
  EXPECT_EQ(
      ps5_record("Rivet", "PS5GD", 1, true, "GAME_TRIAL").why_not_a_game(),
      NotAGame::trial_sku);
}

TEST_F(OwnedRecordRules, RefuseMediaAndTrackingPackagesOnlyOnPs5) {
  EXPECT_FALSE(old_record("Netflix", "PSMEDIA").why_not_a_game());
  EXPECT_EQ(ps5_record("Netflix", "PSMEDIA").why_not_a_game(),
            NotAGame::media_app);
  EXPECT_FALSE(old_record("Tracker", "pstrack").why_not_a_game());
  EXPECT_EQ(ps5_record("Tracker", "pstrack").why_not_a_game(),
            NotAGame::tracking_placeholder);
}

TEST_F(OwnedRecordRules, RefuseDigitalExtrasOnlyOnPs5) {
  for (const auto* extra :
       {"Odyssey Art Book", "Odyssey Artbook", "The Art of Odyssey",
        "Odyssey Soundtrack", "Digital Content Viewer", "Odyssey ContentViewer",
        "Odyssey Bonus Content", "Odyssey BonusContent"}) {
    EXPECT_FALSE(old_record(extra).why_not_a_game()) << extra;
    EXPECT_EQ(ps5_record(extra).why_not_a_game(), NotAGame::digital_extra)
        << extra;
  }
  EXPECT_FALSE(
      ps5_record("METAL GEAR SOLID: MASTER COLLECTION Vol.1 BONUS CONTENT")
          .why_not_a_game());
  EXPECT_FALSE(ps5_record("The Bright Getaway").why_not_a_game());
}

TEST_F(OwnedRecordRules, BecomeAnOwnedTitleOnlyWhenTheyAreAGame) {
  EXPECT_FALSE(ps5_record("Foo Demo").as_owned_title());
  EXPECT_TRUE(ps5_record("Rivet").as_owned_title());
}

TEST_F(OwnedRecordRules, RankTheRecordThatLaunchesBestFirst) {
  const auto full = record({.id = "EP0109-PPSA07048_00-GAME",
                            .product = "EP0109-PPSA07048_00-GAME"});
  const auto bundle = record({.id = "EP0109-PPSA07048_00-GAME",
                              .product = "EP0109-PPSA07048_00-BUNDLE",
                              .feature = 1});
  const auto disc = record({.id = "EP0109-CUSA07037_00-DISC",
                            .product = "EP0109-CUSA07037_00-UPGRADE",
                            .feature = 5,
                            .package = "PS4GS"});
  EXPECT_EQ(full.stream_rank(), 10);
  EXPECT_EQ(bundle.stream_rank(), 6);
  EXPECT_EQ(disc.stream_rank(), 1);
  EXPECT_TRUE(full.streams_better_than(bundle));
  EXPECT_FALSE(bundle.streams_better_than(full));
  EXPECT_TRUE(bundle.streams_better_than(disc));
  EXPECT_FALSE(full.streams_better_than(full));
}

TEST_F(OwnedRecordRules, BreakARankTieByPackageThenProduct) {
  const auto disc = record({.id = "EP0109-CUSA07037_00-DISC",
                            .product = "EP0109-CUSA07037_00-UPGRADE",
                            .feature = 5,
                            .package = "PS4GS"});
  const auto download = record({.id = "EP0109-CUSA07037_00-DISC",
                                .product = "EP0109-CUSA07037_00-UPGRADE",
                                .feature = 5,
                                .package = "PS4GX"});
  EXPECT_TRUE(disc.streams_better_than(download));
  EXPECT_FALSE(download.streams_better_than(disc));
  const auto earlier = record({.id = "EP0109-CUSA07037_00-DISC",
                               .product = "EP0109-CUSA07037_00-AAAAAAA",
                               .feature = 5,
                               .package = "PS4GX"});
  EXPECT_TRUE(earlier.streams_better_than(download));
  EXPECT_FALSE(download.streams_better_than(earlier));
}

TEST_F(OwnedRecordRules, NameTheServiceFromThePlatformAttribute) {
  EXPECT_EQ(classic_on("PS4").service(), ServiceGeneration::old_service);
  EXPECT_EQ(classic_on("ps3").service(), ServiceGeneration::old_service);
  EXPECT_EQ(classic_on("ps3").platform(), Platform::ps3);
  EXPECT_EQ(classic_on("PS5").service(), ServiceGeneration::ps5_cloud);
}

TEST_F(OwnedRecordRules, FallBackToTheProductWhenThePlatformIsMissing) {
  EXPECT_EQ(classic_on("").service(), ServiceGeneration::old_service);
  EXPECT_EQ(classic_on("").platform(), Platform::ps4);
  EXPECT_EQ(record({.id = "UP0114-NPUB37002_00-OTHER",
                    .product = "UP0114-NPUB37002_00-OTHER",
                    .name = "Other",
                    .platform = ""})
                .service(),
            ServiceGeneration::ps5_cloud);
}

TEST_F(OwnedRecordRules, TreatAPs5OwnershipIdAsPs5Native) {
  const auto native = record(
      {.id = rivet, .product = classic, .name = "Rivet", .platform = "ps4"});
  EXPECT_TRUE(native.is_ps5_native());
  EXPECT_EQ(native.service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(native.platform(), Platform::ps5);
  EXPECT_FALSE(record({.id = classic,
                       .product = rivet,
                       .name = "Classic",
                       .platform = "ps4"})
                   .is_ps5_native());
  EXPECT_TRUE(
      record({.id = "", .product = rivet, .name = "Rivet"}).is_ps5_native());
}

}  // namespace
}  // namespace catalog_test
