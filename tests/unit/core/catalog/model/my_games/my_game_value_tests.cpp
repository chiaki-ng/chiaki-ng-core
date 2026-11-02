#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace catalog_test {
namespace {

using libchiaking::catalog::HeldAs;
using libchiaking::catalog::HideReason;
using libchiaking::catalog::ListStreamability;
using libchiaking::catalog::Standing;
using libchiaking::catalog::StreamRefusal;
using libchiaking::users::Tier;

class MyGameServices : public ValueCases {
 protected:
  static auto cloud_title() -> Title {
    return title(show, "League Diamond 26", Platform::ps5,
                 ServiceGeneration::ps5_cloud);
  }
};

class MyGameVisibility : public MyGameServices {};
class MyGameRefusal : public MyGameServices {};

TEST_F(MyGameServices, PlayAnOwnedOldServiceGameThroughTheSubscription) {
  const auto old_title = title(god_of_war, "Storm of Ages III Remastered",
                               Platform::ps4, ServiceGeneration::old_service);
  const auto owned_old =
      game(old_title, PlusInclusion::not_included(Source::old_service),
           PlayPath::old_service,
           owned(god_of_war, Platform::ps4, ServiceGeneration::old_service));
  ASSERT_TRUE(owned_old);
  const auto& classic = owned_old.value();
  EXPECT_EQ(classic.title().service(), ServiceGeneration::old_service);
  EXPECT_TRUE(classic.is_owned());
  EXPECT_FALSE(classic.inclusion().is_included());
  EXPECT_EQ(classic.play_path(), PlayPath::old_service);
  EXPECT_FALSE(classic.is_in_ps5_library());
  EXPECT_TRUE(classic.plays_through_subscription());
  EXPECT_EQ(classic.standing(), Standing::included);
  EXPECT_FALSE(classic.needs_library_add());
  EXPECT_FALSE(classic.needs_storefront_check());
  EXPECT_FALSE(classic.can_be_added_to_library());
  EXPECT_EQ(classic.launch_identifier().text(), god_of_war);
  EXPECT_EQ(classic.product_id(), product(god_of_war));
}

TEST_F(MyGameServices, AddAnIncludedCloudGameToTheLibrary) {
  const auto unowned =
      game(cloud_title(), PlusInclusion::with_premium(Source::store_tags),
           PlayPath::included);
  ASSERT_TRUE(unowned);
  EXPECT_TRUE(unowned.value().needs_library_add());
  EXPECT_TRUE(unowned.value().can_be_added_to_library());
  EXPECT_TRUE(unowned.value().needs_storefront_check());
  EXPECT_EQ(unowned.value().standing(), Standing::included);
}

TEST_F(MyGameServices, StandInTheLibraryOnceHeld) {
  const auto in_library =
      game(cloud_title(), PlusInclusion::with_premium(Source::store_tags),
           PlayPath::included,
           owned(show, Platform::ps5, ServiceGeneration::ps5_cloud,
                 HeldAs::trial_access));
  ASSERT_TRUE(in_library);
  EXPECT_EQ(in_library.value().standing(), Standing::in_library);
  EXPECT_TRUE(in_library.value().is_in_ps5_library());
  EXPECT_FALSE(in_library.value().needs_library_add());
  ASSERT_TRUE(in_library.value().ownership());
  EXPECT_EQ(in_library.value().ownership()->held_as(), HeldAs::trial_access);
  EXPECT_EQ(owned_rivet().standing(), Standing::in_library);
}

TEST_F(MyGameServices, StayInTheStoreUntilBought) {
  const auto store = game(title(bus, "Coach Simulator 21", Platform::ps5,
                                ServiceGeneration::ps5_cloud),
                          PlusInclusion::not_included(Source::store_tags),
                          PlayPath::store_only);
  ASSERT_TRUE(store);
  EXPECT_EQ(store.value().standing(), Standing::store);
  EXPECT_TRUE(store.value().needs_library_add());
  EXPECT_FALSE(store.value().can_be_added_to_library());
}

TEST_F(MyGameServices, MayStreamWhenTheListMarksThemUnstreamable) {
  const auto marked =
      game(title(rivet, "Rivet", Platform::ps5, ServiceGeneration::ps5_cloud)
               .with_streamability(ListStreamability::marked_unstreamable),
           PlusInclusion::not_included(Source::store_tags), PlayPath::owned,
           owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud));
  ASSERT_TRUE(marked);
  EXPECT_EQ(marked.value().standing(), Standing::may_stream);
  EXPECT_TRUE(marked.value().is_visible());
}

TEST_F(MyGameServices, RefuseAStoreOnlyPathWithOwnership) {
  EXPECT_EQ(game(cloud_title(), PlusInclusion::not_included(Source::store_tags),
                 PlayPath::store_only,
                 owned(show, Platform::ps5, ServiceGeneration::ps5_cloud))
                .error(),
            MyGame::Invalid::store_only_path_with_ownership);
}

TEST_F(MyGameVisibility, NamesWhyATitleIsHidden) {
  EXPECT_TRUE(Visibility::visible().is_visible());
  EXPECT_FALSE(Visibility::visible().hide_reason());
  const auto below_premium =
      Visibility::hidden(HideReason::tier_does_not_include);
  EXPECT_FALSE(below_premium.is_visible());
  EXPECT_EQ(below_premium.hide_reason(), HideReason::tier_does_not_include);
  EXPECT_FALSE(below_premium.because_of());
}

TEST_F(MyGameVisibility, NamesTheTitleThatHidesADuplicate) {
  const auto duplicate =
      Visibility::hidden(HideReason::already_owned, product(rivet));
  EXPECT_EQ(duplicate.because_of(), product(rivet));
  EXPECT_NE(duplicate, Visibility::hidden(HideReason::tier_does_not_include));
}

TEST_F(MyGameVisibility, HidesAGameAndCanShowItAgain) {
  const auto hidden_owned = game(
      cloud_title(), PlusInclusion::not_included(Source::store_tags),
      PlayPath::owned, owned(show, Platform::ps5, ServiceGeneration::ps5_cloud),
      Visibility::hidden(HideReason::tier_does_not_include));
  ASSERT_TRUE(hidden_owned);
  EXPECT_FALSE(hidden_owned.value().is_visible());
  EXPECT_EQ(hidden_owned.value().visibility().hide_reason(),
            HideReason::tier_does_not_include);
  EXPECT_TRUE(
      hidden_owned.value().with_visibility(Visibility::visible()).is_visible());
}

TEST_F(MyGameRefusal, IsRememberedOnTheGameItRefused) {
  const StreamRefusal::Circumstances then{
      Tier::premium, StreamRefusal::Holding::held_as(
                         *libchiaking::catalog::OwnershipId::parse(show))};
  const StreamRefusal refusal{account, product(show),
                              StreamRefusal::Kind::no_game_for_entitlement,
                              at_ms(5000), then};
  const auto played =
      game(cloud_title(), PlusInclusion::not_included(Source::store_tags),
           PlayPath::owned,
           owned(show, Platform::ps5, ServiceGeneration::ps5_cloud));
  ASSERT_TRUE(played);
  const auto remembered = played.value().with_refusal(refusal);
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().remembered_refusal(), refusal);
  EXPECT_EQ(remembered.value().with_refusal(std::nullopt).value(),
            played.value());
  const StreamRefusal elsewhere{account, product(rivet),
                                StreamRefusal::Kind::refused_by_stream_server,
                                at_ms(5000), then};
  EXPECT_EQ(played.value().with_refusal(elsewhere).error(),
            MyGame::Invalid::refusal_for_another_title);
}

}  // namespace
}  // namespace catalog_test
