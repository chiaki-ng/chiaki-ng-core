#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/launch_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchIdentifier;
using catalog::LaunchRefusal;
using catalog::Platform;
using catalog::PlayPath;
using catalog::PlusInclusion;
using catalog::ReleaseDate;
using catalog::ServiceGeneration;
using catalog::Visibility;
using libchiaking::users::Tier;

class LaunchVerdictForAnyMembership
    : public ::testing::TestWithParam<libchiaking::users::Membership>,
      public LaunchCases {
 protected:
  void SetUp() override { ASSERT_TRUE(now); }

  const std::optional<Time> now =
      ReleaseDate::parse_time("2026-10-07T12:00:00Z");
  const MyGames games = sample_games(GetParam());
};

TEST_P(LaunchVerdictForAnyMembership, AllowsAnOwnedGameWithOrWithoutAClock) {
  EXPECT_TRUE(launch(games, rivet, now).is_allowed());
  EXPECT_TRUE(launch(games, rivet, std::nullopt).is_allowed());
}

TEST_P(LaunchVerdictForAnyMembership, AsksToAddAnIncludedGameToTheLibrary) {
  EXPECT_EQ(kind_of(launch(games, show, now)), Kind::add_to_library);
}

TEST_P(LaunchVerdictForAnyMembership, RefusesAnUnlistedGame) {
  EXPECT_EQ(kind_of(launch(games, bus, now)), Kind::not_listed);
}

TEST_P(LaunchVerdictForAnyMembership, AllowsAnOldServiceTitle) {
  EXPECT_TRUE(launch(games, god_of_war, now).is_allowed());
}

TEST_P(LaunchVerdictForAnyMembership, RefusesAnUnannouncedPreorder) {
  EXPECT_EQ(kind_of(launch(games, preorder, now)), Kind::unannounced);
  EXPECT_EQ(kind_of(launch(games, preorder, std::nullopt)), Kind::unannounced);
}

TEST_P(LaunchVerdictForAnyMembership, HoldsADatedGameUntilItsRelease) {
  EXPECT_EQ(ReleaseDate::parse_time("2026-10-13T14:00:00Z"),
            at_ms(farming_release_ms));
  EXPECT_EQ(game_of(games, farming).play_path(), PlayPath::owned);
  EXPECT_TRUE(game_of(games, farming).inclusion().is_included());
  const auto early = launch(games, farming, now);
  EXPECT_EQ(kind_of(early), Kind::not_released);
  ASSERT_TRUE(early.refusal());
  EXPECT_EQ(early.refusal()->time(), at_ms(farming_release_ms));
  EXPECT_EQ(kind_of(launch(games, farming, std::nullopt)),
            Kind::no_trusted_clock);
  EXPECT_EQ(kind_of(launch(games, farming, at_ms(farming_release_ms))),
            Kind::not_released);
  EXPECT_TRUE(
      launch(games, farming, at_ms(farming_release_ms + 1)).is_allowed());
  EXPECT_EQ(kind_of(launch(games, farming, at_ms(farming_release_ms - 1))),
            Kind::not_released);
}

INSTANTIATE_TEST_SUITE_P(
    Memberships, LaunchVerdictForAnyMembership,
    ::testing::Values(libchiaking::users::Membership{},
                      libchiaking::users::Membership::member_of_unknown_tier(),
                      libchiaking::users::Membership{Tier::premium}));

class LaunchVerdictByMembership : public ::testing::Test, public LaunchCases {
 protected:
  void SetUp() override { ASSERT_TRUE(now); }

  const std::optional<Time> now =
      ReleaseDate::parse_time("2026-10-07T12:00:00Z");
};

TEST_F(LaunchVerdictByMembership, HidesEveryTitleWithoutPlusOrStreaming) {
  const auto without_plus =
      sample_games(Membership{Tier::none}, streaming_refused());
  for (const auto id : {rivet, show, god_of_war, preorder, farming})
    EXPECT_TRUE(hidden_by_tier(launch(without_plus, id, now))) << id;
}

TEST_F(LaunchVerdictByMembership,
       KeepsOwnedGamesButHidesOldServiceTitlesWithoutPlus) {
  const auto tier_alone = sample_games(Membership{Tier::none});
  EXPECT_TRUE(launch(tier_alone, rivet, now).is_allowed());
  EXPECT_TRUE(hidden_by_tier(launch(tier_alone, god_of_war, now)));
}

TEST_F(LaunchVerdictByMembership,
       KeepsOldServiceTitlesButHidesPs5GamesWithoutStreaming) {
  const auto refused_alone =
      sample_games(Membership{Tier::premium}, streaming_refused());
  EXPECT_TRUE(launch(refused_alone, god_of_war, now).is_allowed());
  const auto verdict = launch(refused_alone, rivet, now);
  ASSERT_TRUE(verdict.refusal());
  EXPECT_EQ(verdict.refusal()->hide_reason(),
            HideReason::tier_does_not_include);
}

class LaunchVerdictOfNothing : public ::testing::Test, public LaunchCases {};

TEST_F(LaunchVerdictOfNothing, RefusesAMissingGameAsUnlisted) {
  EXPECT_EQ(kind_of(LaunchVerdict::of(
                nullptr, ReleaseDate::parse_time("2026-10-07T12:00:00Z"))),
            Kind::not_listed);
  EXPECT_FALSE(catalog::ProductId::parse(""));
}

TEST_F(LaunchVerdictOfNothing,
       CannotBeForgedThroughAnOwnedPathWithoutOwnership) {
  catalog::Title::Facts listed{.product = product(bus)};
  listed.name = "Coach Simulator 21";
  listed.platform = Platform::ps5;
  listed.service = ServiceGeneration::ps5_cloud;
  MyGame::Facts forged{
      .title = catalog::Title::from(std::move(listed)).value(),
      .inclusion =
          PlusInclusion::not_included(PlusInclusion::Source::store_lists),
      .path = PlayPath::owned,
      .visibility = Visibility::visible(),
      .launch_identifier = LaunchIdentifier::of(product(bus)),
  };
  EXPECT_EQ(MyGame::from(std::move(forged)).error(),
            MyGame::Invalid::owned_path_without_ownership);
}

class LaunchVerdictBeforeLaunch : public ::testing::Test, public LaunchCases {
 protected:
  void SetUp() override { ASSERT_TRUE(now); }

  auto before(std::string_view id, std::optional<Time> held_now) const
      -> LaunchVerdict {
    return LaunchVerdict::before_launch(games.find(product(id)), held_now);
  }

  auto judged_alike(std::string_view id, std::optional<Time> held) const
      -> testing::AssertionResult {
    if (kind_of(before(id, held)) == kind_of(launch(games, id, held)))
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << id << " is judged differently before the launch";
  }

  const MyGames games = sample_games(Membership{Tier::premium});
  const std::optional<Time> now =
      ReleaseDate::parse_time("2026-10-07T12:00:00Z");
};

TEST_F(LaunchVerdictBeforeLaunch, HoldsADatedTitleUntilItsRelease) {
  EXPECT_EQ(kind_of(before(farming, std::nullopt)), Kind::no_trusted_clock);
  EXPECT_EQ(game_of(games, farming).release().time(),
            at_ms(farming_release_ms));
  const auto early = before(farming, now);
  EXPECT_EQ(kind_of(early), Kind::not_released);
  ASSERT_TRUE(early.refusal());
  EXPECT_EQ(early.refusal()->time(), at_ms(farming_release_ms));
  EXPECT_EQ(kind_of(before(farming, at_ms(farming_release_ms - 1))),
            Kind::not_released);
  EXPECT_EQ(kind_of(before(farming, at_ms(farming_release_ms))),
            Kind::not_released);
  EXPECT_TRUE(before(farming, at_ms(farming_release_ms + 1)).is_allowed());
}

TEST_F(LaunchVerdictBeforeLaunch, JudgesUndatedAndUnlistedTitles) {
  EXPECT_EQ(kind_of(before(preorder, std::nullopt)), Kind::unannounced);
  EXPECT_EQ(kind_of(before(preorder, now)), Kind::unannounced);
  EXPECT_TRUE(before(rivet, std::nullopt).is_allowed());
  EXPECT_TRUE(before(rivet, now).is_allowed());
  EXPECT_EQ(kind_of(before(show, std::nullopt)), Kind::add_to_library);
  EXPECT_EQ(kind_of(before(bus, std::nullopt)), Kind::not_listed);
  EXPECT_EQ(kind_of(LaunchVerdict::before_launch(nullptr, std::nullopt)),
            Kind::not_listed);
}

TEST_F(LaunchVerdictBeforeLaunch, JudgesEveryTitleAsTheLaunchJudgesIt) {
  for (const auto id : {rivet, show, bus, god_of_war, preorder, farming})
    for (const auto held : {std::optional<Time>{}, now,
                            std::optional<Time>{at_ms(farming_release_ms)},
                            std::optional<Time>{at_ms(farming_release_ms + 1)}})
      EXPECT_TRUE(judged_alike(id, held));
}

class LaunchReleaseGate : public ::testing::Test, public LaunchCases {
 protected:
  static constexpr std::int64_t now_ms = 1'791'241'200'000;

  static auto gate_matches_refusal(const ReleaseDate& release,
                                   std::optional<Time> clock)
      -> testing::AssertionResult {
    if (release.blocks_launch(clock) ==
        LaunchRefusal::of_release(release, clock).has_value())
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "the gate and the refusal disagree"
           << (clock ? " with a clock" : " without a clock");
  }
};

TEST_F(LaunchReleaseGate, MatchesTheLaunchRefusal) {
  for (const auto release :
       {ReleaseDate::unrestricted(), ReleaseDate::at(at_ms(-5)),
        ReleaseDate::at(at_ms(now_ms - 1)), ReleaseDate::at(at_ms(now_ms)),
        ReleaseDate::at(at_ms(now_ms + 1)), ReleaseDate::unannounced()})
    for (const auto clock :
         {std::optional<Time>{at_ms(now_ms)}, std::optional<Time>{}})
      EXPECT_TRUE(gate_matches_refusal(release, clock));
}

TEST_F(LaunchReleaseGate, NamesWhyTheReleaseRefusesTheLaunch) {
  EXPECT_EQ(LaunchRefusal::of_release(ReleaseDate::at(at_ms(now_ms + 1)),
                                      at_ms(now_ms))
                ->kind(),
            Kind::not_released);
  EXPECT_EQ(
      LaunchRefusal::of_release(ReleaseDate::at(at_ms(now_ms)), std::nullopt)
          ->kind(),
      Kind::no_trusted_clock);
  EXPECT_EQ(LaunchRefusal::of_release(ReleaseDate::unannounced(), at_ms(now_ms))
                ->kind(),
            Kind::unannounced);
  EXPECT_FALSE(
      LaunchRefusal::of_release(ReleaseDate::unrestricted(), std::nullopt));
}

class LaunchVerdictOfHidden : public ::testing::Test, public LaunchCases {};

TEST_F(LaunchVerdictOfHidden, RefusesATrialAndAnUnlistedTitle) {
  const auto held = ps5_games({
      {.id = rivet,
       .product = rivet,
       .name = "Rivet & Bolt: Split Worlds",
       .concept_id = "10002"},
      {.id = "EP0109-PPSA07028_00-FAKE0112TITLE000",
       .product = "EP0109-PPSA07028_00-FAKE0112TITLE000",
       .name = "Rivet & Bolt: Split Worlds Trial",
       .feature = catalog::Ps5Entitlement::trial_feature,
       .concept_id = "10002"},
  });
  const auto games = ps5_games_of(&held, nullptr);
  EXPECT_TRUE(launch(games, rivet).is_allowed());
  EXPECT_EQ(kind_of(launch(games, "EP0109-PPSA07028_00-FAKE0112TITLE000")),
            Kind::not_listed);
  EXPECT_EQ(games.find(product(bus)), nullptr);
  EXPECT_EQ(kind_of(launch(games, bus)), Kind::not_listed);
  EXPECT_FALSE(games.explain(product(bus)).is_listed());
  EXPECT_EQ(games.explain(product(bus)).first_failure()->rule,
            catalog::Explanation::Rule::listed_by_service);
}

TEST_F(LaunchVerdictOfHidden, RefusesADuplicateProduct) {
  const auto first = directly(
      rivet, Platform::ps5, ServiceGeneration::ps5_cloud, PlayPath::store_only);
  EXPECT_EQ(MyGames::of(account, {first, first}, {}).error(),
            MyGames::Invalid::duplicate_product);
}

TEST_F(LaunchVerdictOfHidden, AsksToBuyAStoreOnlyGame) {
  const auto first = directly(
      rivet, Platform::ps5, ServiceGeneration::ps5_cloud, PlayPath::store_only);
  EXPECT_EQ(kind_of(LaunchVerdict::of(&first, at_ms(1))), Kind::buy_to_stream);
}

TEST_F(LaunchVerdictOfHidden, NamesWhyAHiddenGameIsHidden) {
  const auto first = directly(
      rivet, Platform::ps5, ServiceGeneration::ps5_cloud, PlayPath::store_only);
  const auto hidden = first.with_visibility(
      Visibility::hidden(HideReason::duplicate_launch, product(show)));
  const auto verdict = LaunchVerdict::of(&hidden, at_ms(1));
  EXPECT_EQ(kind_of(verdict), Kind::hidden);
  ASSERT_TRUE(verdict.refusal());
  EXPECT_EQ(verdict.refusal()->hide_reason(), HideReason::duplicate_launch);
}

TEST(LaunchRefusalOwnershipCheck, IsAskedForOnlyWhenOwnershipIsUnknown) {
  using RefusalKind = LaunchRefusal::Kind;
  const auto unchecked = LaunchRefusal::ownership_unknown();
  EXPECT_EQ(unchecked.kind(), RefusalKind::check_unavailable);
  EXPECT_TRUE(unchecked.asks_for_ownership_check());
  ASSERT_TRUE(unchecked.cause());
  EXPECT_EQ(unchecked.cause()->reason(),
            catalog::LaunchCheckReason::ownership_unknown);
  EXPECT_FALSE(unchecked.cause()->needs_sign_in());
  EXPECT_FALSE(unchecked.cause()->is_cancelled());
  const auto down = LaunchRefusal::check_unavailable(
      catalog::LaunchCheckFailure{catalog::LaunchCheckReason::unavailable});
  EXPECT_FALSE(down.asks_for_ownership_check());
  EXPECT_FALSE(LaunchRefusal::add_to_library().asks_for_ownership_check());
}

class ClearedLaunchVerdict : public ::testing::Test, public LaunchCases {};

TEST_F(ClearedLaunchVerdict, CarriesWhatPlayNeeds) {
  const auto game = directly(rivet, Platform::ps5, ServiceGeneration::ps5_cloud,
                             PlayPath::store_only);
  const auto store = catalog::StoreLocale::parse("en-GB");
  ASSERT_TRUE(store);
  const catalog::ClearedLaunch launch{
      .product = product(rivet),
      .service = ServiceGeneration::ps5_cloud,
      .platform = Platform::ps5,
      .identifier = game.launch_identifier(),
      .held_as = catalog::OwnershipId::parse(rivet),
      .store = *store,
      .origin = catalog::ListOrigin::public_store,
  };
  const auto cleared = LaunchVerdict::cleared(launch);
  EXPECT_TRUE(cleared.is_allowed());
  EXPECT_FALSE(cleared.refusal());
  ASSERT_TRUE(cleared.cleared_launch());
  EXPECT_EQ(*cleared.cleared_launch(), launch);
}

TEST_F(ClearedLaunchVerdict, IsMissingFromAPlainAllowanceOrARefusal) {
  EXPECT_TRUE(LaunchVerdict::allowed().is_allowed());
  EXPECT_FALSE(LaunchVerdict::allowed().cleared_launch());
  EXPECT_FALSE(
      LaunchVerdict::refused(LaunchRefusal::not_listed()).cleared_launch());
}

}  // namespace
}  // namespace catalog_test
