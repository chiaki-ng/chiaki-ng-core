#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/cloud_stream/ps5_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchRefusal;
using catalog::LaunchVerdict;
using catalog::LibraryState;
using catalog::OwnershipState;
using catalog::PlayPath;
using catalog::Standing;
using libchiaking::users::Tier;

class Ps5GameRows : public ::testing::Test, public Ps5GamesCases {
 protected:
  static auto none_store_only(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto& row : games.visible())
      if (row.play_path() == PlayPath::store_only)
        return testing::AssertionFailure()
               << row.product_id().text() << " is store only";
    return testing::AssertionSuccess();
  }

  static auto ownership_left_unknown(const MyGames& games)
      -> testing::AssertionResult {
    const auto& row = game_of(games, show);
    if (row.ownership_state() != OwnershipState::unknown)
      return testing::AssertionFailure() << "ownership is known";
    if (row.play_path() != PlayPath::included)
      return testing::AssertionFailure() << "the row is not included";
    if (!row.needs_ownership_check())
      return testing::AssertionFailure() << "no ownership check is asked";
    if (row.needs_library_add())
      return testing::AssertionFailure() << "a library add is asked";
    if (row.standing() != Standing::ownership_unknown)
      return testing::AssertionFailure() << "the standing is not unknown";
    if (!games.explain(product(show)).could_not_check_ownership())
      return testing::AssertionFailure()
             << "the explanation does not say ownership was unchecked";
    if (!game_of(games, rivet).is_owned())
      return testing::AssertionFailure() << "the held copy is not owned";
    return testing::AssertionSuccess();
  }

  static auto all_hidden_by_tier(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto& row : games.hidden()) {
      if (row.visibility().hide_reason() != HideReason::tier_does_not_include)
        return testing::AssertionFailure()
               << row.product_id().text() << " is hidden for another reason";
      if (failed_rule(games.explain(row.product_id())) != Rule::tier)
        return testing::AssertionFailure()
               << row.product_id().text() << " did not fail on the tier";
    }
    return testing::AssertionSuccess();
  }
};

TEST_F(Ps5GameRows, ListAnOwnedFullGameOutsideTheCategoryAsOneOwnedRow) {
  const auto owned =
      ps5_games({{.id = rivet,
                  .product = rivet,
                  .name = "Rivet & Bolt: Split Worlds",
                  .title = "PPSA07028_00",
                  .sku = "EP0109-PPSA07028_00-FAKE0110TITLE000-E001"}});
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(count_on_ps5(games), 1U);
  const auto& row = game_of(games, rivet);
  EXPECT_TRUE(row.is_owned());
  EXPECT_TRUE(row.is_visible());
  EXPECT_TRUE(row.is_in_ps5_library());
  EXPECT_EQ(row.play_path(), PlayPath::owned);
  EXPECT_EQ(row.standing(), Standing::in_library);
  EXPECT_FALSE(row.inclusion().is_included());
  EXPECT_EQ(row.launch_identifier().text(), rivet);
  EXPECT_FALSE(row.library_offer());
  EXPECT_TRUE(LaunchVerdict::of(&row, std::nullopt).is_allowed());
  const auto told = games.explain(product(rivet));
  EXPECT_TRUE(told.is_listed());
  EXPECT_TRUE(has_step(told, Rule::own_listing));
  EXPECT_EQ(step_of(told, Rule::access_route).path, PlayPath::owned);
  EXPECT_EQ(step_of(told, Rule::ownership).outcome, Outcome::passed);
}

TEST_F(Ps5GameRows, AskToAddAnUnclaimedPremiumGameWithTheExactOffer) {
  const auto owned = ps5_games({});
  const auto category = premium();
  const auto games = ps5_games_of(&owned, &category);
  const auto& row = game_of(games, show);
  EXPECT_FALSE(row.is_owned());
  EXPECT_EQ(row.play_path(), PlayPath::included);
  EXPECT_EQ(row.ownership_state(), OwnershipState::not_owned);
  EXPECT_TRUE(row.inclusion().is_included());
  EXPECT_EQ(row.inclusion().source(),
            catalog::PlusInclusion::Source::store_tags);
  EXPECT_TRUE(row.needs_library_add());
  EXPECT_TRUE(row.can_be_added_to_library());
  EXPECT_EQ(row.launch_identifier().text(), show);
  ASSERT_TRUE(row.library_offer());
  EXPECT_EQ(row.library_offer()->sku_id, "SKU1");
  EXPECT_EQ(row.library_offer()->entitlement_ids,
            std::vector<std::string>{std::string{show}});
  EXPECT_EQ(row.library_offer()->entitled_through_subscription, true);
  const auto verdict = LaunchVerdict::of(&row, std::nullopt);
  ASSERT_TRUE(verdict.refusal());
  EXPECT_EQ(verdict.refusal()->kind(), LaunchRefusal::Kind::add_to_library);
  const auto told = games.explain(product(show));
  EXPECT_TRUE(told.is_listed());
  EXPECT_EQ(step_of(told, Rule::premium_branding).outcome, Outcome::passed);
  EXPECT_EQ(step_of(told, Rule::premium_entitlement).ownership,
            catalog::OwnershipId::parse(show));
  EXPECT_EQ(step_of(told, Rule::included_with_plus).outcome, Outcome::passed);
  EXPECT_EQ(step_of(told, Rule::access_route).path, PlayPath::included);
  EXPECT_EQ(step_of(told, Rule::ownership).outcome, Outcome::noted);
}

TEST_F(Ps5GameRows, LeaveOutAProductNeitherIncludedNorOwned) {
  const auto owned = ps5_games({});
  const auto category = premium();
  const auto games = ps5_games_of(&owned, &category);
  EXPECT_EQ(count_on_ps5(games), 4U);
  EXPECT_EQ(games.find(product(unbranded)), nullptr);
  EXPECT_EQ(games.find(product(trial_only)), nullptr);
  EXPECT_EQ(failed_rule(games.explain(product(unbranded))),
            Rule::premium_branding);
  const auto trial = games.explain(product(trial_only));
  EXPECT_EQ(failed_rule(trial), Rule::premium_entitlement);
  EXPECT_EQ(step_of(trial, Rule::premium_branding).outcome, Outcome::passed);
  EXPECT_TRUE(none_store_only(games));
}

TEST_F(Ps5GameRows, JoinOwnedAndPremiumByEntitlement) {
  const auto category = premium();
  const auto by_entitlement = ps5_games(
      {{.id = bus, .product = bus, .name = "Coach Simulator 21 Next Stop"}});
  const auto games = ps5_games_of(&by_entitlement, &category);
  const auto& joined = game_of(games, bus);
  EXPECT_TRUE(joined.is_owned());
  EXPECT_EQ(joined.play_path(), PlayPath::owned);
  EXPECT_TRUE(joined.inclusion().is_included());
  EXPECT_EQ(joined.launch_identifier().text(), bus);
  EXPECT_FALSE(joined.library_offer());
  EXPECT_TRUE(
      has_step(games.explain(product(bus)), Rule::joined_by_entitlement));
  EXPECT_EQ(count_on_ps5(games), 4U);
}

TEST_F(Ps5GameRows, JoinOwnedAndPremiumByProduct) {
  const auto category = premium();
  const auto by_product = ps5_games({{.id = bus_claim,
                                      .product = bus,
                                      .name = "Coach Simulator 21 Next Stop"}});
  const auto claimed = ps5_games_of(&by_product, &category);
  const auto& through = game_of(claimed, bus);
  EXPECT_TRUE(through.is_owned());
  EXPECT_EQ(through.launch_identifier().text(), bus_claim);
  EXPECT_TRUE(has_step(claimed.explain(product(bus)), Rule::joined_by_product));
  EXPECT_EQ(claimed.find(product(bus_claim)), &through);
  EXPECT_EQ(count_on_ps5(claimed), 4U);
}

TEST_F(Ps5GameRows, KeepOwnedAndPremiumApartWhenOnlyTheConceptMatches) {
  const auto category = premium();
  const auto by_concept = ps5_games({{.id = comet_deluxe,
                                      .product = comet_deluxe,
                                      .name = "Comet",
                                      .concept_id = "10001"}});
  const auto apart = ps5_games_of(&by_concept, &category);
  EXPECT_EQ(count_on_ps5(apart), 5U);
  EXPECT_TRUE(game_of(apart, comet_deluxe).is_owned());
  EXPECT_FALSE(game_of(apart, comet).is_owned());
  EXPECT_TRUE(game_of(apart, comet).needs_library_add());
}

TEST_F(Ps5GameRows, LeavePremiumOwnershipUnknownWhileTheLibraryIs) {
  const auto category = premium();
  for (const auto state : {LibraryState::unavailable, LibraryState::partial}) {
    const auto owned =
        ps5_games({{.id = rivet, .product = rivet, .name = "Rivet"}}, state);
    EXPECT_TRUE(ownership_left_unknown(ps5_games_of(&owned, &category)));
  }
  const auto unread = ps5_games_of(nullptr, &category);
  EXPECT_EQ(game_of(unread, show).ownership_state(), OwnershipState::unknown);
}

TEST_F(Ps5GameRows, HideEveryRowWhenCloudStreamingIsRefused) {
  const auto category = premium();
  const auto owned =
      ps5_games({{.id = rivet, .product = rivet, .name = "Rivet"}});
  for (const auto& refused :
       {streaming_refused(),
        cloud_streaming(
            libchiaking::users::CloudBenefits::Authorisation::granted)}) {
    const auto games =
        ps5_games_of(&owned, &category, refused,
                     libchiaking::users::Membership{Tier::premium});
    EXPECT_TRUE(games.visible().empty());
    EXPECT_EQ(count_on_ps5(games), 5U);
    EXPECT_TRUE(all_hidden_by_tier(games));
    EXPECT_EQ(game_of(games, rivet).play_path(), PlayPath::owned);
  }
}

TEST_F(Ps5GameRows, ShowRowsWhenCloudStreamingIsAllowedOrUnknown) {
  const auto category = premium();
  const auto owned =
      ps5_games({{.id = rivet, .product = rivet, .name = "Rivet"}});
  const auto allowed = ps5_games_of(&owned, &category, streaming_allowed());
  EXPECT_TRUE(game_of(allowed, rivet).is_visible());
  EXPECT_EQ(step_of(allowed.explain(product(rivet)), Rule::tier).outcome,
            Outcome::passed);
  const auto unknown = ps5_games_of(
      &owned, &category,
      libchiaking::users::CloudStreaming::unknown(
          libchiaking::users::Unanswered{
              libchiaking::users::Unanswered::Reason::unavailable}));
  EXPECT_TRUE(unknown.hidden().empty());
  EXPECT_EQ(unknown.visible().size(), 5U);
  const auto unread = ps5_games_of(&owned, &category);
  EXPECT_FALSE(has_step(unread.explain(product(rivet)), Rule::tier));
}

TEST_F(Ps5GameRows, LeaveLegacyCloudStreamRowsAsTheyAreWithoutPs5Sources) {
  const auto list = list_of({on_old_service(far_cry, "Wide Fire 5")});
  const auto alone = games_of(library_of({}), list);
  const auto category = premium();
  const auto owned =
      ps5_games({{.id = rivet, .product = rivet, .name = "Rivet"}});
  const auto with_ps5 =
      ps5_games_of(&owned, &category, std::nullopt, {}, &list);
  EXPECT_EQ(game_of(with_ps5, far_cry), game_of(alone, far_cry));
  EXPECT_EQ(with_ps5.explain(product(far_cry)),
            alone.explain(product(far_cry)));
  EXPECT_EQ(count_on_ps5(alone), 0U);
  EXPECT_EQ(count_on_ps5(with_ps5), 5U);
  const auto without_list = ps5_games_of(&owned, &category);
  EXPECT_EQ(without_list.find(product(far_cry)), nullptr);
  EXPECT_EQ(count_on_ps5(without_list), 5U);
}

}  // namespace
}  // namespace catalog_test
