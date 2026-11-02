#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/my_games/my_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchRefusal;
using catalog::LaunchVerdict;
using catalog::PlayPath;
using catalog::ServiceGeneration;
using catalog::Standing;
using catalog::StreamRefusal;
using libchiaking::users::Membership;
using libchiaking::users::Tier;
using Holding = StreamRefusal::Holding;

class MyGamesWithoutALibrary : public ::testing::Test, public MyGamesCases {
 protected:
  auto games_without(
      const catalog::Library& checked_alone, const Membership& membership = {},
      std::span<const StreamRefusal> refusals = {},
      const std::optional<libchiaking::users::CloudStreaming>& streaming =
          std::nullopt,
      const catalog::OwnedPs5Games* found = nullptr) const -> MyGames {
    return catalog::MyGamesAssembly::games_of(
        {.library = checked_alone,
         .library_state = catalog::LibraryState::unavailable,
         .list = &list,
         .ps5 = found,
         .category = &category},
        membership, streaming, refusals, 3, at_ms(3000));
  }

  auto with_library() const -> MyGames {
    return catalog::MyGamesAssembly::games_of({.library = library_of({}),
                                               .list = &list,
                                               .ps5 = &none_found,
                                               .category = &category},
                                              {}, std::nullopt, {}, 0,
                                              at_ms(3000));
  }

  auto hidden_below(Tier tier) const -> testing::AssertionResult {
    const auto below = games_without(nothing_checked, Membership{tier}, {},
                                     streaming_refused());
    const auto tier_alone = games_without(nothing_checked, Membership{tier});
    if (tier_alone.visible().size() != 1 || tier_alone.hidden().size() != 1)
      return testing::AssertionFailure()
             << "the tier alone shows " << tier_alone.visible().size()
             << " and hides " << tier_alone.hidden().size();
    if (tier_alone.hidden().front().title().service() !=
        ServiceGeneration::old_service)
      return testing::AssertionFailure()
             << "the tier alone hides a game outside the old service";
    if (!game_of(tier_alone, show).is_visible())
      return testing::AssertionFailure()
             << "the tier alone hides the included game";
    if (!below.visible().empty() || below.hidden().size() != 2)
      return testing::AssertionFailure()
             << "without streaming " << below.visible().size()
             << " are shown and " << below.hidden().size() << " hidden";
    if (game_of(below, show).visibility().hide_reason() !=
        HideReason::tier_does_not_include)
      return testing::AssertionFailure()
             << "the included game is hidden for another reason";
    if (LaunchVerdict::of(&game_of(below, show), at_ms(1)).refusal()->kind() !=
        LaunchRefusal::Kind::hidden)
      return testing::AssertionFailure()
             << "the included game is refused for another reason";
    return testing::AssertionSuccess();
  }

  const catalog::StreamableList list =
      list_of({on_old_service(god_of_war, "Storm of Ages III Remastered")});
  const catalog::cloud_stream::PremiumCategory category =
      category_of({included(show, "League Diamond 26")});
  const catalog::Library nothing_checked =
      catalog::Library::of(account, {}, {}, Time{}).value();
  const catalog::OwnedPs5Games none_found = ps5_games({});
  const MyGames games = games_without(nothing_checked);
  const catalog::Ps5Entitlement found_record = ps5_entitlement(
      {.id = show, .product = show, .name = "League Diamond 26"});
};

TEST_F(MyGamesWithoutALibrary, KnowNothingOfOwnership) {
  EXPECT_EQ(games.library_state(), catalog::LibraryState::unavailable);
  EXPECT_FALSE(games.ownership_is_known());
  EXPECT_EQ(games.inputs().library, catalog::LibraryState::unavailable);
  EXPECT_EQ(games.size(), 2U);
  EXPECT_EQ(games.visible().size(), 2U);
  EXPECT_TRUE(games.hidden().empty());
  const auto known = with_library();
  EXPECT_TRUE(known.ownership_is_known());
  EXPECT_NE(known.inputs(), games.inputs());
}

TEST_F(MyGamesWithoutALibrary, AskForAnOwnershipCheckOfAnIncludedGame) {
  const auto& included = game_of(games, show);
  EXPECT_FALSE(included.is_owned());
  EXPECT_EQ(included.ownership_state(), catalog::OwnershipState::unknown);
  EXPECT_TRUE(included.needs_ownership_check());
  EXPECT_FALSE(included.needs_library_add());
  EXPECT_TRUE(included.can_be_added_to_library());
  EXPECT_EQ(included.play_path(), PlayPath::included);
  EXPECT_EQ(included.standing(), Standing::ownership_unknown);
  EXPECT_EQ(included.holding(), Holding::unknown());
  const auto unchecked = LaunchVerdict::of(&included, at_ms(1));
  ASSERT_TRUE(unchecked.refusal());
  EXPECT_EQ(unchecked.refusal()->kind(),
            LaunchRefusal::Kind::check_unavailable);
  EXPECT_TRUE(unchecked.refusal()->asks_for_ownership_check());
  EXPECT_EQ(unchecked.refusal()->cause()->reason(),
            catalog::LaunchCheckReason::ownership_unknown);
  const auto why = games.explain(product(show));
  EXPECT_TRUE(why.is_listed());
  EXPECT_TRUE(why.could_not_check_ownership());
  const auto told =
      std::ranges::find(why.steps(), Rule::ownership, &Explanation::Step::rule);
  ASSERT_NE(told, why.steps().end());
  EXPECT_EQ(told->outcome, Explanation::Outcome::unchecked);
}

TEST_F(MyGamesWithoutALibrary, StillLaunchOldServiceTitles) {
  const auto& classic_title = game_of(games, god_of_war);
  EXPECT_EQ(classic_title.play_path(), PlayPath::old_service);
  EXPECT_EQ(classic_title.standing(), Standing::included);
  EXPECT_FALSE(classic_title.needs_ownership_check());
  EXPECT_FALSE(classic_title.needs_library_add());
  EXPECT_EQ(classic_title.ownership_state(), catalog::OwnershipState::unknown);
  EXPECT_TRUE(LaunchVerdict::of(&classic_title, at_ms(1)).is_allowed());
  EXPECT_FALSE(games.explain(product(god_of_war)).could_not_check_ownership());
}

TEST_F(MyGamesWithoutALibrary, DifferFromALibraryThatHoldsNothing) {
  const auto known = with_library();
  EXPECT_EQ(game_of(known, show).ownership_state(),
            catalog::OwnershipState::not_owned);
  EXPECT_TRUE(game_of(known, show).needs_library_add());
  EXPECT_TRUE(game_of(known, show).can_be_added_to_library());
  EXPECT_EQ(game_of(known, show).standing(), Standing::included);
  EXPECT_FALSE(known.explain(product(show)).could_not_check_ownership());
  const auto verdict = LaunchVerdict::of(&game_of(known, show), at_ms(1));
  EXPECT_EQ(verdict.refusal()->kind(), LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(verdict.refusal()->asks_for_ownership_check());
}

TEST_F(MyGamesWithoutALibrary,
       HideIncludedGamesBelowPremiumOnlyWithoutStreaming) {
  for (const auto tier : {Tier::none, Tier::essential, Tier::extra})
    EXPECT_TRUE(hidden_below(tier));
}

TEST_F(MyGamesWithoutALibrary, ShowEverythingToPremium) {
  EXPECT_EQ(games_without(nothing_checked, Membership{Tier::premium})
                .visible()
                .size(),
            2U);
}

TEST_F(MyGamesWithoutALibrary, ConfirmAGameOneCheckedRecordHolds) {
  const auto found = found_record.as_owned_title();
  ASSERT_TRUE(found);
  const auto one_checked = nothing_checked.with(*found, at_ms(2500));
  const auto rechecked = catalog::OwnedPs5Games::unread(at_ms(2500))
                             .with(found_record, at_ms(2500));
  const auto partly =
      games_without(one_checked, {}, {}, std::nullopt, &rechecked);
  EXPECT_FALSE(partly.ownership_is_known());
  EXPECT_NE(partly.inputs(), games.inputs());
  const auto& confirmed = game_of(partly, show);
  EXPECT_TRUE(confirmed.is_owned());
  EXPECT_EQ(confirmed.ownership_state(), catalog::OwnershipState::owned);
  EXPECT_FALSE(confirmed.needs_ownership_check());
  EXPECT_EQ(confirmed.standing(), Standing::in_library);
  EXPECT_TRUE(LaunchVerdict::of(&confirmed, at_ms(1)).is_allowed());
  EXPECT_FALSE(partly.explain(product(show)).could_not_check_ownership());
  ASSERT_TRUE(partly.holding_of(product(show)).id());
  EXPECT_EQ(partly.holding_of(product(show)).id()->text(), show);
  EXPECT_EQ(partly.holding_of(product(rivet)), Holding::unknown());
  EXPECT_EQ(partly.holding_of(product(bus)), Holding::unknown());
}

TEST_F(MyGamesWithoutALibrary, RememberARefusalOnlyUntilTheGameIsFoundHeld) {
  const auto found = found_record.as_owned_title();
  ASSERT_TRUE(found);
  const auto one_checked = nothing_checked.with(*found, at_ms(2500));
  const auto rechecked = catalog::OwnedPs5Games::unread(at_ms(2500))
                             .with(found_record, at_ms(2500));
  const std::vector<StreamRefusal> refused{
      StreamRefusal{account,
                    product(show),
                    StreamRefusal::Kind::refused_by_stream_server,
                    at_ms(2900),
                    {std::nullopt, Holding::not_held()}}};
  EXPECT_TRUE(game_of(games_without(nothing_checked, {}, refused), show)
                  .remembered_refusal());
  EXPECT_FALSE(
      game_of(games_without(one_checked, {}, refused, std::nullopt, &rechecked),
              show)
          .remembered_refusal());
}

TEST_F(MyGamesWithoutALibrary, AreRebuiltOnlyWithTheSameOwnershipKnowledge) {
  std::vector<MyGame> listed{games.visible().begin(), games.visible().end()};
  EXPECT_TRUE(MyGames::of(account, listed, games.inputs()));
  EXPECT_EQ(MyGames::of(account, listed, {}).error(),
            MyGames::Invalid::ownership_knowledge);
  const auto known = with_library();
  const std::vector<MyGame> judged{known.visible().begin(),
                                   known.visible().end()};
  EXPECT_EQ(MyGames::of(account, judged, games.inputs()).error(),
            MyGames::Invalid::ownership_knowledge);
}

TEST_F(MyGamesWithoutALibrary, NeverGuessAPathFromUncheckedOwnership) {
  const auto& included = game_of(games, show);
  auto guess = MyGame::Facts{.title = included.title(),
                             .inclusion = included.inclusion(),
                             .path = PlayPath::store_only,
                             .visibility = catalog::Visibility::visible(),
                             .launch_identifier = included.launch_identifier()};
  guess.ownership_checked = false;
  EXPECT_EQ(MyGame::from(guess).error(),
            MyGame::Invalid::store_only_path_without_ownership_check);
  guess.path = PlayPath::unknown;
  EXPECT_TRUE(MyGame::from(guess));
  guess.ownership_checked = true;
  EXPECT_EQ(MyGame::from(guess).error(),
            MyGame::Invalid::unknown_path_with_known_ownership);
}

}  // namespace
}  // namespace catalog_test
