#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/my_games/my_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::LaunchRefusal;
using catalog::LaunchVerdict;
using catalog::StreamRefusal;
using libchiaking::users::Membership;
using libchiaking::users::Tier;
using Holding = StreamRefusal::Holding;

class MyGamesRememberedRefusals : public ::testing::Test, public MyGamesCases {
 protected:
  auto games_from(const catalog::OwnedPs5Games& held_games,
                  std::span<const StreamRefusal> refusals,
                  std::uint64_t revision, Time now,
                  const Membership& membership = {}) const -> MyGames {
    const auto library =
        base.with_ps5_part(held_games.games(), held_games.state());
    return catalog::MyGamesAssembly::games_of(
        {.library = library, .ps5 = &held_games, .category = &category},
        membership, std::nullopt, refusals, revision, now);
  }

  auto games_with(std::span<const StreamRefusal> refusals, Time now,
                  const Membership& membership = {}) const -> MyGames {
    return games_from(owned, refusals, 7, now, membership);
  }

  auto refusal(Holding holding, std::optional<Tier> tier = std::nullopt,
               StreamRefusal::Kind kind =
                   StreamRefusal::Kind::refused_by_stream_server) const
      -> std::vector<StreamRefusal> {
    return {StreamRefusal{
        account, product(rivet), kind, refused_at, {tier, holding}}};
  }

  const catalog::cloud_stream::PremiumCategory category =
      category_of({included(rivet, "Rivet & Bolt: Split Worlds"),
                   included(bus, "Coach Simulator 21 Next Stop")});
  const catalog::OwnedPs5Games owned = ps5_games(
      {{.id = rivet, .product = rivet, .name = "Rivet & Bolt: Split Worlds"},
       {.id = bus, .product = bus, .name = "Coach Simulator 21 Next Stop"}});
  const catalog::OwnedPs5Games refetched = ps5_games(
      {{.id = rivet, .product = rivet, .name = "Rivet & Bolt: Split Worlds"}});
  const catalog::OwnedPs5Games lost = ps5_games(
      {{.id = bus, .product = bus, .name = "Coach Simulator 21 Next Stop"}});
  const catalog::Library base = library_of({});
  const Holding held = Holding::held_as(*catalog::OwnershipId::parse(rivet));
  const Time refused_at = at_ms(10'000);
  const Time soon = refused_at + std::chrono::hours{1};
  const std::vector<StreamRefusal> mine = refusal(held);
};

TEST_F(MyGamesRememberedRefusals, BelongToTheRefusedTitle) {
  const auto games = games_with(mine, soon);
  EXPECT_EQ(games.inputs().refusals_revision, 7U);
  const auto& refused = game_of(games, rivet);
  ASSERT_TRUE(refused.remembered_refusal());
  EXPECT_EQ(refused.remembered_refusal()->refused_at(), refused_at);
  EXPECT_TRUE(refused.is_visible());
  EXPECT_FALSE(game_of(games, bus).remembered_refusal());
  const auto verdict = LaunchVerdict::of(&refused, std::nullopt);
  ASSERT_TRUE(verdict.refusal());
  EXPECT_EQ(verdict.refusal()->kind(), LaunchRefusal::Kind::refused_by_service);
  EXPECT_EQ(verdict.refusal()->time(), refused_at);
  EXPECT_TRUE(
      LaunchVerdict::of(&game_of(games, bus), std::nullopt).is_allowed());
  EXPECT_TRUE(
      has_step(games.explain(product(rivet)), Rule::remembered_refusal));
}

TEST_F(MyGamesRememberedRefusals, BelongToOneUser) {
  const std::vector<StreamRefusal> another_user{
      StreamRefusal{libchiaking::AccountId{43},
                    product(rivet),
                    StreamRefusal::Kind::refused_by_stream_server,
                    refused_at,
                    {std::nullopt, held}}};
  EXPECT_FALSE(
      game_of(games_with(another_user, soon), rivet).remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreForgottenAfterADay) {
  EXPECT_FALSE(
      game_of(games_with(mine, refused_at + std::chrono::hours{25}), rivet)
          .remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreForgottenOnceTheTierIsKnownToBeHigher) {
  EXPECT_FALSE(game_of(games_with(mine, soon, Membership{Tier::premium}), rivet)
                   .remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, MadeAsPremiumAreKeptForADay) {
  const auto as_premium = refusal(held, Tier::premium);
  EXPECT_TRUE(
      game_of(games_with(as_premium, soon), rivet).remembered_refusal());
  EXPECT_TRUE(
      game_of(games_with(as_premium, soon, Membership{Tier::premium}), rivet)
          .remembered_refusal());
  EXPECT_FALSE(
      game_of(games_with(as_premium, refused_at + std::chrono::hours{25}),
              rivet)
          .remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreForgottenOnceTheTitleIsHeldSince) {
  const auto as_premium_before = refusal(Holding::not_held(), Tier::premium);
  EXPECT_FALSE(
      game_of(games_with(as_premium_before, soon), rivet).remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreForgottenWhenAnotherCopyWasHeld) {
  const auto older_library =
      refusal(Holding::held_as(*catalog::OwnershipId::parse(bus)), std::nullopt,
              StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_FALSE(
      game_of(games_with(older_library, soon), rivet).remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreKeptWhileTheSameCopyIsHeldAfterARefetch) {
  const auto games = games_with(mine, soon);
  const auto same_title_still_held =
      refusal(held, std::nullopt, StreamRefusal::Kind::no_game_for_entitlement);
  const auto after_refetch =
      games_from(refetched, same_title_still_held, 8, soon);
  EXPECT_EQ(after_refetch.inputs().library_fetched_at,
            games.inputs().library_fetched_at);
  EXPECT_EQ(game_of(after_refetch, rivet).remembered_refusal(),
            same_title_still_held.front());
  const auto later = refetched.with(
      ps5_entitlement(
          {.id = bus, .product = bus, .name = "Coach Simulator 21 Next Stop"}),
      soon);
  EXPECT_NE(later.fetched_at(), owned.fetched_at());
  EXPECT_TRUE(game_of(games_from(later, same_title_still_held, 9, soon), rivet)
                  .remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, AreForgottenOnceTheCopyIsLost) {
  const auto same_title_still_held =
      refusal(held, std::nullopt, StreamRefusal::Kind::no_game_for_entitlement);
  const auto after_loss = games_from(lost, same_title_still_held, 10, soon);
  EXPECT_FALSE(game_of(after_loss, rivet).is_owned());
  EXPECT_FALSE(game_of(after_loss, rivet).remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, MadeWhileUnownedAreKeptOnlyWhileUnowned) {
  const auto while_unowned = refusal(Holding::not_held());
  EXPECT_TRUE(game_of(games_from(lost, while_unowned, 11, soon), rivet)
                  .remembered_refusal());
  EXPECT_FALSE(
      game_of(games_with(while_unowned, soon), rivet).remembered_refusal());
}

TEST_F(MyGamesRememberedRefusals, CompareAgainstTheHoldingOfEachTitle) {
  const auto games = games_with(mine, soon);
  const auto same_title_still_held =
      refusal(held, std::nullopt, StreamRefusal::Kind::no_game_for_entitlement);
  const auto after_loss = games_from(lost, same_title_still_held, 10, soon);
  EXPECT_EQ(games.holding_of(product(rivet)), held);
  EXPECT_EQ(after_loss.holding_of(product(rivet)), Holding::not_held());
  EXPECT_EQ(games.holding_of(product(show)), Holding::not_held());
}

TEST_F(MyGamesRememberedRefusals, OfAnUnlistedTitleTouchNothing) {
  const std::vector<StreamRefusal> unlisted{
      StreamRefusal{account,
                    product(show),
                    StreamRefusal::Kind::refused_by_stream_server,
                    refused_at,
                    {std::nullopt, held}}};
  const auto untouched = games_with(unlisted, soon);
  EXPECT_FALSE(game_of(untouched, rivet).remembered_refusal());
  EXPECT_FALSE(game_of(untouched, bus).remembered_refusal());
}

}  // namespace
}  // namespace catalog_test
