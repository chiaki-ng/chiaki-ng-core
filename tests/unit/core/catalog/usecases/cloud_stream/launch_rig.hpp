#pragma once

#include "src/core/catalog/model/cloud_stream/exclusions.hpp"
#include "src/core/catalog/usecases/cloud_stream/clearance.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {

class CloudStreamCases : public RuleCases {
 public:
  using Kind = libchiaking::catalog::LaunchRefusal::Kind;
  using Reason = libchiaking::catalog::LaunchCheckReason;
  using Clearance = libchiaking::catalog::usecases::cloud_stream::Clearance;

  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr std::string_view bundle =
      "EP0109-PPSA47001_00-FAKE0133TITLE000";
  static constexpr std::string_view first =
      "EP0109-PPSA47002_00-FAKE0042TITLE000";
  static constexpr std::string_view second =
      "EP0109-PPSA47003_00-FAKE0116TITLE000";

  static auto show_record() -> std::string {
    return ClearanceRig::ps5_game(show, "League Diamond 26");
  }

  static auto kind_of(const libchiaking::catalog::LaunchVerdict& verdict)
      -> Kind {
    return ClearanceRig::kind_of(verdict);
  }

  static auto store() -> libchiaking::catalog::StreamableList {
    return list_of({on_old_service("EP0100-CUSA07016_00-FAKE0038TITLE000",
                                   "Wide Fire 5")});
  }

  static auto premium() -> libchiaking::catalog::cloud_stream::PremiumCategory {
    return plausible_category_of(
        {included(show, "League Diamond 26"),
         Offered{.product = bundle,
                 .name = "Two Game Bundle",
                 .skus = {Sold{.sku = "BUNDLESKU",
                               .entitlements = {Granted{.id = first},
                                                Granted{.id = second}}}}}});
  }

  static auto owning_rivet(
      std::optional<std::string> playable_from = std::nullopt)
      -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games({{.id = rivet,
                       .product = rivet,
                       .name = "Rivet & Bolt: Split Worlds",
                       .title = "PPSA07028_00",
                       .preorder_core = playable_from.has_value(),
                       .playable_from = std::move(playable_from)}});
  }

  static auto owning_nothing() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games({});
  }

  static auto owning_unknown() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games({}, libchiaking::catalog::LibraryState::unavailable);
  }

  static auto owning_the_first_game() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games({{.id = first, .product = first, .name = "First Game"}});
  }

  static auto owning_both_games() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games(
        {{.id = first, .product = first, .name = "First Game"},
         {.id = second, .product = second, .name = "Second Game"}});
  }

  static auto holding_a_trial() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games(
        {{.id = rivet,
          .product = rivet,
          .name = "Rivet & Bolt: Split Worlds",
          .feature = libchiaking::catalog::Ps5Entitlement::trial_feature,
          .title = "PPSA07028_00"}});
  }

  static auto holding_an_ended_trial() -> libchiaking::catalog::OwnedPs5Games {
    return ps5_games(
        {{.id = rivet,
          .product = rivet,
          .name = "Rivet & Bolt: Split Worlds",
          .feature =
              libchiaking::catalog::Ps5Entitlement::play_first_trial_feature,
          .title = "PPSA07028_00",
          .play_time_used_up = true}});
  }

  static auto premium_with_an_empty_bundle_sku()
      -> libchiaking::catalog::cloud_stream::PremiumCategory {
    return plausible_category_of({Offered{
        .product = bundle,
        .name = "Two Game Bundle",
        .skus = {
            Sold{.sku = "BUNDLESKU",
                 .entitlements = {Granted{.id = first}, Granted{.id = second}}},
            Sold{.sku = "EMPTYSKU"}}}});
  }

  static auto preordered(std::string_view playable) -> std::string {
    return ClearanceRig::ps5_game_titled(
        rivet, "PPSA07028_00",
        R"(,"preorderCoreFlag":true,"skuMeta":{"playabilityDate":")" +
            std::string{playable} + R"("})");
  }

  static auto entitlement_reading(std::string_view facts) -> std::string {
    return std::string{R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000",)"} +
           std::string{facts} + "}";
  }

  static auto trial_reading(std::string_view active, std::string_view streaming)
      -> std::string {
    return entitlement_reading(
        std::string{
            R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":)"} +
        std::string{active} +
        R"(,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSGD"},"streamingSupported":)" +
        std::string{streaming});
  }
};

class CloudStreamLaunch : public CloudStreamCases {
 public:
  CloudStreamLaunch() {
    rig.cloud_streaming_allowed();
    rig.nothing_excluded();
    rig.resolver_lists_nothing();
    rig.ps5_record_answers(
        200, ClearanceRig::ps5_game_titled(rivet, "PPSA07028_00"));
  }

  static auto refused_as(std::string_view record) -> Kind {
    CloudStreamLaunch launch;
    launch.rig.ps5_record_answers(200, std::string{record});
    const auto verdict = launch.clear(owning_rivet(), rivet);
    EXPECT_FALSE(verdict.is_allowed()) << record;
    EXPECT_EQ(launch.rig.http.count(ClearanceRig::resolver), 0) << record;
    EXPECT_EQ(launch.rig.http.count(ClearanceRig::store_app), 0) << record;
    return kind_of(verdict);
  }

  auto seen(const libchiaking::catalog::OwnedPs5Games& owned)
      -> ClearanceRig::View {
    return rig.holding(*user, store(), owned, category);
  }

  auto clear(const libchiaking::catalog::OwnedPs5Games& owned,
             std::string_view title,
             std::optional<libchiaking::users::CloudStreaming> streaming =
                 std::nullopt) -> libchiaking::catalog::LaunchVerdict {
    checks.clear();
    return clearance.clear(
        *user, seen(owned), product(title), std::move(streaming), {},
        libchiaking::catalog::usecases::CheckProgress{observer});
  }

  auto held(const libchiaking::catalog::OwnedPs5Games& owned,
            std::string_view title) -> libchiaking::catalog::LaunchVerdict {
    checks.clear();
    return clearance.can_launch(*user, seen(owned), product(title));
  }

  auto listed_as_ended(const libchiaking::catalog::OwnedPs5Games& owned)
      -> bool {
    const auto listed = seen(owned);
    const auto* game = listed.games->find(product(rivet));
    EXPECT_NE(game, nullptr);
    if (game == nullptr) return false;
    EXPECT_TRUE(game->ownership());
    if (!game->ownership()) return false;
    return game->ownership()->trial_ended();
  }

  auto gk() const -> int { return rig.http.count("gaikai"); }

  ClearanceRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user{rig.user(true)};
  libchiaking::catalog::cloud_stream::PremiumCategory category{premium()};
  test_support::FakeLog log;
  Clearance clearance{rig.http, rig.clock, rig.wall,  &log,
                      rig.held, rig.list,  rig.checks};
  std::vector<libchiaking::catalog::LaunchCheck> checks;
  libchiaking::catalog::LaunchCheckObserver observer{
      [this](libchiaking::catalog::LaunchCheck check) {
        checks.push_back(check);
      }};
};

}  // namespace catalog_test
