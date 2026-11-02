#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace catalog_test {

class LaunchCases : public RuleCases {
 public:
  using Kind = libchiaking::catalog::LaunchRefusal::Kind;
  using LaunchVerdict = libchiaking::catalog::LaunchVerdict;
  using MyGame = libchiaking::catalog::MyGame;
  using MyGames = libchiaking::catalog::MyGames;
  using Membership = libchiaking::users::Membership;

  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr std::string_view god_of_war =
      "UP0114-CUSA07009_00-FAKE0051TITLE000";
  static constexpr std::string_view ps3_classic =
      "UP0114-NPUB37002_00-FAKE0021TITLE000";
  static constexpr std::string_view preorder =
      "UP0114-PPSA07045_00-FAKE0097TITLE000";
  static constexpr std::string_view farming =
      "EP0106-PPSA27018_00-FAKE0041TITLE000";
  static constexpr std::string_view crow =
      "UP0114-PPSA07054_00-FAKE0025TITLE000";
  static constexpr std::string_view crow_ownership =
      "UP0114-NPWR79060_00-FAKE0026TITLE000";
  static constexpr std::int64_t farming_release_ms = 1791900000000;

  static auto kind_of(const LaunchVerdict& verdict) -> std::optional<Kind> {
    return verdict.refusal() ? std::optional{verdict.refusal()->kind()}
                             : std::nullopt;
  }

  static auto launch(const MyGames& games, std::string_view id,
                     std::optional<Time> trusted_now = at_ms(1))
      -> LaunchVerdict {
    return LaunchVerdict::of(games.find(product(id)), trusted_now);
  }

  static auto hidden_by_tier(const LaunchVerdict& verdict)
      -> testing::AssertionResult {
    if (kind_of(verdict) != Kind::hidden)
      return testing::AssertionFailure() << "the launch is not hidden";
    if (verdict.refusal()->hide_reason() !=
        libchiaking::catalog::HideReason::tier_does_not_include)
      return testing::AssertionFailure() << "it is hidden for another reason";
    return testing::AssertionSuccess();
  }

  static auto sample_list() -> libchiaking::catalog::StreamableList {
    return list_of({
        on_old_service(god_of_war, "Storm of Ages III Remastered"),
        on_old_service(ps3_classic, "Classic"),
        on_old_service(crow, "Raven Hollow"),
        {.product = bus, .name = "Coach Simulator 21"},
    });
  }

  static auto sample_premium()
      -> libchiaking::catalog::cloud_stream::PremiumCategory {
    return category_of({included(show, "League Diamond 26"),
                        included(farming, "Harvest Simulator 25")});
  }

  static auto sample_ps5_games() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games({
        {.id = rivet, .product = rivet, .name = "Rivet & Bolt: Split Worlds"},
        {.id = preorder,
         .product = preorder,
         .name = "Next Ember",
         .preorder_core = true},
        {.id = farming,
         .product = farming,
         .name = "Harvest Simulator 25",
         .preorder_core = true,
         .playable_from = "2026-10-13T14:00:00Z"},
    });
  }

  static auto sample_games(
      const Membership& membership = {},
      const std::optional<libchiaking::users::CloudStreaming>& streaming =
          std::nullopt) -> MyGames {
    const auto list = sample_list();
    const auto category = sample_premium();
    const auto owned = sample_ps5_games();
    return ps5_games_of(&owned, &category, streaming, membership, &list);
  }

  static auto directly(std::string_view id,
                       libchiaking::catalog::Platform platform,
                       libchiaking::catalog::ServiceGeneration service,
                       libchiaking::catalog::PlayPath path,
                       libchiaking::catalog::ListStreamability streamability =
                           libchiaking::catalog::ListStreamability::listed)
      -> MyGame {
    libchiaking::catalog::Title::Facts listed{.product = product(id)};
    listed.name = "Direct";
    listed.platform = platform;
    listed.service = service;
    listed.streamability = streamability;
    auto title = libchiaking::catalog::Title::from(std::move(listed));
    EXPECT_TRUE(title) << id;
    MyGame::Facts facts{
        .title = std::move(title).value(),
        .inclusion = libchiaking::catalog::PlusInclusion::not_included(
            libchiaking::catalog::PlusInclusion::Source::store_lists),
        .path = path,
        .visibility = libchiaking::catalog::Visibility::visible(),
        .launch_identifier =
            libchiaking::catalog::LaunchIdentifier::of(product(id)),
    };
    auto game = MyGame::from(std::move(facts));
    EXPECT_TRUE(game) << id;
    return std::move(game).value();
  }
};

}  // namespace catalog_test
