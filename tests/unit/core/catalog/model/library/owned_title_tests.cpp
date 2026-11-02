#include "src/core/catalog/model/library/owned_record.hpp"
#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/detail/catalog/release_date.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::HeldAs;
using libchiaking::catalog::OwnedRecord;
using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::ReleaseDate;
using libchiaking::catalog::TitleNumber;

class OwnedTitles : public ValueCases {
 protected:
  static auto upgrade() -> OwnedTitle {
    OwnedTitle::Facts bundled{
        .id = *OwnershipId::parse("EP0109-CUSA07037_00-DISC")};
    bundled.name = "Bundle";
    bundled.release = ReleaseDate::unannounced();
    bundled.product = product("EP0109-PPSA07048_00-UPGRADE");
    return OwnedTitle::from(std::move(bundled)).value();
  }
};

TEST_F(OwnedTitles, RecordHowTheyAreHeld) {
  const auto trial = owned(show, Platform::ps5, ServiceGeneration::ps5_cloud,
                           HeldAs::trial_access);
  EXPECT_EQ(trial.held_as(), HeldAs::trial_access);
  EXPECT_TRUE(trial.release().is_unrestricted());
  EXPECT_EQ(trial.title_number(), *TitleNumber::from("PPSA37004"));
  EXPECT_TRUE(trial.is_held_under(product(show)));
  EXPECT_FALSE(trial.is_held_under(product(rivet)));
}

TEST_F(OwnedTitles, StayUnrestrictedWhenReleasedNoLaterThanUnannounced) {
  const auto trial = owned(show, Platform::ps5, ServiceGeneration::ps5_cloud,
                           HeldAs::trial_access);
  EXPECT_TRUE(trial.released_no_later_than(ReleaseDate::unannounced())
                  .release()
                  .is_unrestricted());
}

TEST_F(OwnedTitles, AreHeldUnderTheirIdAndTheirProduct) {
  const auto held = upgrade();
  EXPECT_EQ(held.title_number(), *TitleNumber::from("PPSA07048"));
  EXPECT_TRUE(held.is_held_under(product("EP0109-PPSA07048_00-UPGRADE")));
  EXPECT_TRUE(held.is_held_under(product("EP0109-CUSA07037_00-DISC")));
}

TEST_F(OwnedTitles, TakeAnEarlierRelease) {
  EXPECT_EQ(
      upgrade().released_no_later_than(ReleaseDate::at(at_ms(5000))).release(),
      ReleaseDate::at(at_ms(5000)));
}

TEST_F(OwnedTitles, AreReadFromARecordsFeatureType) {
  OwnedRecord::Fields fields;
  fields.id = std::string{rivet};
  fields.product_id = std::string{rivet};
  fields.feature_type = OwnedRecord::full_game_feature;
  const OwnedRecord record{fields};
  EXPECT_EQ(record.held_as(), HeldAs::full_game);
  EXPECT_TRUE(OwnershipId::parse(record.fields().id));
  EXPECT_TRUE(record.product_id());
  fields.feature_type = OwnedRecord::trial_access_feature;
  EXPECT_EQ(OwnedRecord{fields}.held_as(), HeldAs::trial_access);
  fields.feature_type = OwnedRecord::disc_upgrade_feature;
  EXPECT_EQ(OwnedRecord{fields}.held_as(), HeldAs::disc_upgrade);
}

TEST_F(OwnedTitles, AreAnotherGameForAnUnknownFeatureType) {
  OwnedRecord::Fields fields;
  fields.product_id = std::string{rivet};
  fields.feature_type = 7;
  fields.id = "not an id";
  EXPECT_EQ(OwnedRecord{fields}.held_as(), HeldAs::other_game);
  EXPECT_FALSE(OwnershipId::parse(OwnedRecord{fields}.fields().id));
}

}  // namespace
}  // namespace catalog_test
