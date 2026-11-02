#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/cloud_stream/ps5_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::NotAGame;
using catalog::PlayPath;

class Ps5GameTrials : public ::testing::Test, public Ps5GamesCases {
 protected:
  static auto dropped_as_not_a_game(const MyGames& games, std::string_view id,
                                    NotAGame why) -> testing::AssertionResult {
    const auto told = games.explain(product(id));
    if (failed_rule(told) != Rule::counts_as_game)
      return testing::AssertionFailure()
             << id << " did not fail on counting as a game";
    if (told.first_failure()->not_a_game != why)
      return testing::AssertionFailure()
             << id << " is not a game for another reason";
    if (told.first_failure()->ownership != catalog::OwnershipId::parse(id))
      return testing::AssertionFailure() << id << " names another ownership";
    return testing::AssertionSuccess();
  }

  static auto held_refusal(std::string_view id)
      -> catalog::StreamRefusal::Circumstances {
    return catalog::StreamRefusal::Circumstances{
        std::nullopt, catalog::StreamRefusal::Holding::held_as(
                          *catalog::OwnershipId::parse(id))};
  }
};

TEST_F(Ps5GameTrials, ExplainAndLeaveOutRecordsThatAreNotStreamableGames) {
  constexpr std::string_view expired = "EP0100-PPSA87001_00-FAKE0128TITLE000";
  constexpr std::string_view disc = "EP0100-PPSA87002_00-FAKE0034TITLE000";
  constexpr std::string_view play_first =
      "EP0100-PPSA87003_00-FAKE0090TITLE000";
  constexpr std::string_view no_stream = "EP0100-PPSA87004_00-FAKE0079TITLE000";
  constexpr std::string_view inactive = "EP0100-PPSA87005_00-FAKE0065TITLE000";
  const auto owned = ps5_games({
      {.id = expired,
       .product = expired,
       .name = "Trial",
       .feature = Recorded::trial_feature,
       .active = false},
      {.id = disc,
       .product = disc,
       .name = "Disc",
       .feature = Recorded::disc_required_feature},
      {.id = play_first,
       .product = play_first,
       .name = "Play First",
       .feature = Recorded::play_first_trial_feature,
       .active = false},
      {.id = no_stream,
       .product = no_stream,
       .name = "No Stream",
       .streams = false},
      {.id = inactive,
       .product = inactive,
       .name = "Inactive",
       .active = false},
  });
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(count_on_ps5(games), 0U);
  const std::vector<std::pair<std::string_view, NotAGame>> expected{
      {expired, NotAGame::inactive},
      {disc, NotAGame::disc_required},
      {play_first, NotAGame::inactive},
      {no_stream, NotAGame::streaming_not_supported},
      {inactive, NotAGame::inactive},
  };
  for (const auto& [id, why] : expected)
    EXPECT_TRUE(dropped_as_not_a_game(games, id, why));
}

TEST_F(Ps5GameTrials, ListAnActiveStreamingTrialAsTrialAccess) {
  constexpr std::string_view trial = "EP0100-PPSA87001_00-FAKE0128TITLE000";
  const auto owned = ps5_games({
      {.id = trial,
       .product = trial,
       .name = "Trial",
       .feature = Recorded::trial_feature},
  });
  const auto games = ps5_games_of(&owned, nullptr);
  const auto& found = game_of(games, trial);
  EXPECT_EQ(count_on_ps5(games), 1U);
  EXPECT_TRUE(found.is_owned());
  EXPECT_EQ(found.play_path(), PlayPath::owned);
  ASSERT_TRUE(found.ownership());
  EXPECT_EQ(found.ownership()->held_as(), catalog::HeldAs::trial_access);
  EXPECT_EQ(found.launch_identifier().text(), trial);
  EXPECT_TRUE(games.explain(product(trial)).is_listed());
}

class Ps5GamePlayFirstTrials : public Ps5GameTrials {
 protected:
  static constexpr std::string_view ended =
      "EP0100-PPSA87009_00-FAKE0090TITLE000";
  static constexpr std::string_view left =
      "EP0100-PPSA87012_00-FAKE0090TITLE000";
  static constexpr std::string_view full =
      "EP0100-PPSA87010_00-FAKE0043TITLE000";
  static constexpr std::string_view superseded =
      "EP0100-PPSA87010_00-FAKE0090TITLE000";
  static constexpr std::string_view no_stream =
      "EP0100-PPSA87011_00-FAKE0090TITLE000";

  const catalog::OwnedPs5Games owned = ps5_games({
      {.id = ended,
       .product = ended,
       .name = "Ended",
       .feature = Recorded::play_first_trial_feature,
       .play_time_used_up = true},
      {.id = left,
       .product = left,
       .name = "Time Left",
       .feature = Recorded::play_first_trial_feature,
       .play_time_used_up = false},
      {.id = full, .product = full, .name = "Full", .concept_id = "20010"},
      {.id = superseded,
       .product = superseded,
       .name = "Superseded",
       .feature = Recorded::play_first_trial_feature,
       .concept_id = "20010",
       .play_time_used_up = true},
      {.id = no_stream,
       .product = no_stream,
       .name = "No Stream",
       .feature = Recorded::play_first_trial_feature,
       .streams = false,
       .play_time_used_up = true},
  });
};

TEST_F(Ps5GamePlayFirstTrials, AreNotOwnedTitlesButAUsedUpOneIsAnEndedTrial) {
  ASSERT_EQ(owned.records().size(), 5U);
  EXPECT_EQ(owned.records()[0].why_not_a_game(), NotAGame::play_first_trial);
  EXPECT_FALSE(owned.records()[0].as_owned_title());
  EXPECT_FALSE(owned.records()[1].as_ended_trial());
  EXPECT_FALSE(owned.records()[4].as_ended_trial());
}

TEST_F(Ps5GamePlayFirstTrials, ListAUsedUpTrialAsAnEndedTrial) {
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(count_on_ps5(games), 3U);
  const auto& found = game_of(games, ended);
  EXPECT_TRUE(found.is_owned());
  EXPECT_EQ(found.play_path(), PlayPath::owned);
  ASSERT_TRUE(found.ownership());
  EXPECT_EQ(found.ownership()->held_as(), catalog::HeldAs::trial_access);
  EXPECT_TRUE(found.ownership()->trial_ended());
  EXPECT_TRUE(found.ownership()->is_play_first_trial());
  EXPECT_TRUE(games.explain(product(ended)).is_listed());
}

TEST_F(Ps5GamePlayFirstTrials, ListATrialWithTimeLeftAsRunning) {
  const auto games = ps5_games_of(&owned, nullptr);
  const auto& playing = game_of(games, left);
  ASSERT_TRUE(playing.ownership());
  EXPECT_EQ(playing.ownership()->held_as(), catalog::HeldAs::trial_access);
  EXPECT_FALSE(playing.ownership()->trial_ended());
  EXPECT_TRUE(playing.ownership()->is_play_first_trial());
  ASSERT_TRUE(game_of(games, full).ownership());
  EXPECT_FALSE(game_of(games, full).ownership()->trial_ended());
  EXPECT_FALSE(game_of(games, full).ownership()->is_play_first_trial());
}

TEST_F(Ps5GamePlayFirstTrials, DoNotMarkADemoAsAPlayFirstTrial) {
  const auto demo =
      ps5_entitlement({.id = "EP0100-PPSA87013_00-FAKE0128TITLE000",
                       .product = "EP0100-PPSA87013_00-FAKE0128TITLE000",
                       .name = "Demo",
                       .feature = Recorded::trial_feature})
          .as_owned_title();
  ASSERT_TRUE(demo);
  EXPECT_EQ(demo->held_as(), catalog::HeldAs::trial_access);
  EXPECT_FALSE(demo->is_play_first_trial());
  EXPECT_FALSE(demo->trial_ended());
}

TEST_F(Ps5GamePlayFirstTrials, LeaveOutASupersededOrUnstreamableTrial) {
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(games.find(product(superseded)), nullptr);
  EXPECT_EQ(failed_rule(games.explain(product(superseded))),
            Rule::trial_superseded);
  EXPECT_EQ(games.find(product(no_stream)), nullptr);
  const auto told = games.explain(product(no_stream));
  EXPECT_EQ(failed_rule(told), Rule::counts_as_game);
  EXPECT_EQ(told.first_failure()->not_a_game,
            NotAGame::streaming_not_supported);
}

TEST_F(Ps5GameTrials, ListOnlyATrialAsEndedByARememberedTrialEndedRefusal) {
  constexpr std::string_view trial = "EP0100-PPSA87014_00-FAKE0131TITLE000";
  constexpr std::string_view other = "EP0100-PPSA87015_00-FAKE0131TITLE000";
  const auto owned = ps5_games({
      {.id = trial,
       .product = trial,
       .name = "Trial",
       .feature = Recorded::trial_feature},
      {.id = other,
       .product = other,
       .name = "Other Trial",
       .feature = Recorded::play_first_trial_feature},
      {.id = rivet, .product = rivet, .name = "Rivet & Bolt: Split Worlds"},
  });
  using Kind = catalog::StreamRefusal::Kind;
  const std::vector<catalog::StreamRefusal> refusals{
      {account, product(trial), Kind::trial_ended, at_ms(2500),
       held_refusal(trial)},
      {account, product(other), Kind::refused_by_stream_server, at_ms(2500),
       held_refusal(other)},
      {account, product(rivet), Kind::trial_ended, at_ms(2500),
       held_refusal(rivet)},
  };
  auto library = library_of({}).with_ps5_part(owned.games(), owned.state());
  const auto games = catalog::MyGamesAssembly::games_of(
      {.library = library, .ps5 = &owned}, {}, std::nullopt, refusals, 1,
      at_ms(3000));

  const auto& ended = game_of(games, trial);
  ASSERT_TRUE(ended.ownership());
  EXPECT_TRUE(ended.ownership()->trial_ended());
  EXPECT_FALSE(ended.ownership()->is_play_first_trial());
  ASSERT_TRUE(ended.remembered_refusal());
  EXPECT_TRUE(ended.remembered_refusal()->ends_the_trial());
  EXPECT_EQ(
      step_of(games.explain(product(trial)), Rule::remembered_refusal).outcome,
      Outcome::noted);
  const auto& refused = game_of(games, other);
  ASSERT_TRUE(refused.ownership());
  EXPECT_FALSE(refused.ownership()->trial_ended());
  EXPECT_TRUE(refused.remembered_refusal());
  const auto& full = game_of(games, rivet);
  ASSERT_TRUE(full.ownership());
  EXPECT_FALSE(full.ownership()->trial_ended());
  EXPECT_FALSE(full.remembered_refusal());
}

TEST_F(Ps5GameTrials, LetAPremiumRowSupersedeAnEndedTrial) {
  const auto category = premium();
  const auto trials = ps5_games({
      {.id = bus_claim,
       .product = bus,
       .name = "Coach Simulator 21 Next Stop",
       .feature = Recorded::play_first_trial_feature,
       .play_time_used_up = true},
  });
  const auto games = ps5_games_of(&trials, &category);
  const auto& row = game_of(games, bus);
  EXPECT_FALSE(row.is_owned());
  EXPECT_TRUE(row.inclusion().is_included());
  EXPECT_EQ(failed_rule(games.explain(product(bus_claim))),
            Rule::trial_superseded);
}

TEST_F(Ps5GameTrials, LetAFullGameOfTheSameProductOrConceptSupersedeATrial) {
  constexpr std::string_view full = "EP0100-PPSA87006_00-FAKE0043TITLE000";
  constexpr std::string_view by_concept =
      "EP0100-PPSA87006_00-FAKE0128TITLE000";
  constexpr std::string_view same_product =
      "EP0100-PPSA87007_00-FAKE0130TITLE000";
  constexpr std::string_view other = "EP0100-PPSA87008_00-FAKE0128TITLE000";
  const auto owned = ps5_games({
      {.id = by_concept,
       .product = by_concept,
       .name = "Trial",
       .feature = Recorded::trial_feature,
       .concept_id = "20001"},
      {.id = full, .product = full, .name = "Full", .concept_id = "20001"},
      {.id = same_product,
       .product = full,
       .name = "Trial",
       .feature = Recorded::trial_feature},
      {.id = other,
       .product = other,
       .name = "Other Trial",
       .feature = Recorded::trial_feature,
       .concept_id = "20002"},
  });
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(count_on_ps5(games), 2U);
  const auto& kept = game_of(games, full);
  ASSERT_TRUE(kept.ownership());
  EXPECT_EQ(kept.ownership()->held_as(), catalog::HeldAs::full_game);
  EXPECT_EQ(kept.launch_identifier().text(), full);
  EXPECT_EQ(games.find(product(by_concept)), nullptr);
  EXPECT_EQ(failed_rule(games.explain(product(by_concept))),
            Rule::trial_superseded);
  ASSERT_TRUE(game_of(games, other).ownership());
  EXPECT_EQ(game_of(games, other).ownership()->held_as(),
            catalog::HeldAs::trial_access);
}

TEST_F(Ps5GameTrials, LaunchAPremiumGameThroughTheCatalogOverATrial) {
  const auto category = premium();
  const auto trials = ps5_games({
      {.id = bus,
       .product = bus,
       .name = "Coach Simulator 21 Next Stop",
       .feature = Recorded::trial_feature},
      {.id = bus_claim,
       .product = bus,
       .name = "Coach Simulator 21 Next Stop",
       .feature = Recorded::trial_feature},
      {.id = comet_deluxe,
       .product = comet_deluxe,
       .name = "Comet",
       .feature = Recorded::trial_feature,
       .concept_id = "10001"},
  });
  const auto games = ps5_games_of(&trials, &category);
  EXPECT_EQ(count_on_ps5(games), 4U);
  const auto& row = game_of(games, bus);
  EXPECT_FALSE(row.is_owned());
  EXPECT_TRUE(row.needs_library_add());
  EXPECT_TRUE(row.inclusion().is_included());
  EXPECT_FALSE(game_of(games, comet).is_owned());
  EXPECT_EQ(games.find(product(comet_deluxe)), nullptr);
  EXPECT_EQ(failed_rule(games.explain(product(comet_deluxe))),
            Rule::trial_superseded);
}

TEST_F(Ps5GameTrials, LetABundleSupersedeATrialOfOneOfItsGames) {
  const auto category = premium();
  constexpr std::string_view child_trial =
      "EP0100-PPSA57002_00-FAKE0019TITLE000";
  const auto bundled = ps5_games({
      {.id = child_trial,
       .product = game_one,
       .name = "Game One",
       .feature = Recorded::trial_feature},
  });
  const auto with_bundle = ps5_games_of(&bundled, &category);
  EXPECT_EQ(count_on_ps5(with_bundle), 4U);
  EXPECT_EQ(with_bundle.find(product(child_trial)), nullptr);
  EXPECT_EQ(failed_rule(with_bundle.explain(product(child_trial))),
            Rule::trial_superseded);
}

TEST_F(Ps5GameTrials, NameTheTrialVersionAGameForSaleOffers) {
  constexpr std::string_view sold_game = "UP0113-PPSA37006_00-FAKE0126TITLE000";
  constexpr std::string_view demo_game = "EP0100-PPSA77002_00-FAKE0032TITLE000";
  constexpr std::string_view demo = "EP0100-PPSA77002_00-FAKE0031TITLE000";
  const auto sold = for_sale_of({
      {.product = sold_game,
       .name = "Hail to the Rainbow",
       .skus = {{.sku = "UP0113-PPSA37006_00-FAKE0126TITLE000-U001",
                 .branded = false,
                 .entitlements = {{.id = sold_game}}},
                {.sku = "UP0113-PPSA37006_00-FAKE0126TITLE000-U003",
                 .branded = false,
                 .entitlements = {{.id = sold_game,
                                   .feature = Feature::play_first_trial}}}}},
      {.product = demo_game,
       .name = "Demo Game",
       .skus = {{.sku = "SKU1",
                 .branded = false,
                 .entitlements = {{.id = demo_game},
                                  {.id = demo, .feature = Feature::trial}}}}},
      {.product = rivet,
       .name = "Rivet",
       .skus = {{.sku = "SKU1",
                 .branded = false,
                 .entitlements = {{.id = rivet},
                                  {.id = demo, .feature = Feature::trial}}}}},
  });
  const auto included_rivet = category_of({included(rivet, "Rivet")});
  const auto owned = ps5_games({});
  const auto games =
      ps5_games_of(&owned, &included_rivet, std::nullopt, {}, nullptr, &sold);
  EXPECT_EQ(game_of(games, sold_game).play_path(), PlayPath::store_only);
  EXPECT_EQ(game_of(games, sold_game).trial_offer(),
            catalog::TrialOffer::play_first_trial);
  EXPECT_EQ(game_of(games, demo_game).trial_offer(), catalog::TrialOffer::demo);
  EXPECT_EQ(game_of(games, rivet).trial_offer(), catalog::TrialOffer::none);
  const auto holding = ps5_games({{.id = demo,
                                   .product = demo,
                                   .name = "Demo Game Demo",
                                   .feature = Recorded::trial_feature}});
  const auto held =
      ps5_games_of(&holding, &included_rivet, std::nullopt, {}, nullptr, &sold);
  EXPECT_EQ(game_of(held, demo_game).trial_offer(), catalog::TrialOffer::none);
  EXPECT_TRUE(game_of(held, demo).is_owned());
}

TEST_F(Ps5GameTrials, StopOfferingAUsedPlayFirstTrialButStillOfferADemo) {
  constexpr std::string_view used_game = "EP0107-PPSA17005_00-FAKE0037TITLE000";
  constexpr std::string_view both_game = "EP0108-PPSA17004_00-FAKE0118TITLE000";
  constexpr std::string_view both_demo = "EP0108-PPSA17004_00-FAKE0031TITLE000";
  const auto sold = for_sale_of({
      {.product = used_game,
       .name = "Lumen Shade: Expedition 33",
       .skus = {{.sku = "SKU1",
                 .branded = false,
                 .entitlements = {{.id = used_game}}},
                {.sku = "SKU3",
                 .branded = false,
                 .entitlements = {{.id = used_game,
                                   .feature = Feature::play_first_trial}}}}},
      {.product = both_game,
       .name = "Directive 8020",
       .skus = {{.sku = "SKU1",
                 .branded = false,
                 .entitlements = {{.id = both_game},
                                  {.id = both_demo, .feature = Feature::trial},
                                  {.id = both_game,
                                   .feature = Feature::play_first_trial}}}}},
  });
  const auto owned = ps5_games({});
  const auto fresh =
      ps5_games_of(&owned, nullptr, std::nullopt, {}, nullptr, &sold);
  EXPECT_EQ(game_of(fresh, used_game).trial_offer(),
            catalog::TrialOffer::play_first_trial);
  EXPECT_EQ(game_of(fresh, both_game).trial_offer(),
            catalog::TrialOffer::play_first_trial);
  const std::vector used{product(both_game), product(used_game)};
  const auto noted =
      ps5_games_of(&owned, nullptr, std::nullopt, {}, nullptr, &sold, used);
  EXPECT_EQ(game_of(noted, used_game).trial_offer(), catalog::TrialOffer::none);
  EXPECT_EQ(game_of(noted, both_game).trial_offer(), catalog::TrialOffer::demo);
  EXPECT_FALSE(noted.was_computed_from(fresh.inputs()));
}

}  // namespace
}  // namespace catalog_test
