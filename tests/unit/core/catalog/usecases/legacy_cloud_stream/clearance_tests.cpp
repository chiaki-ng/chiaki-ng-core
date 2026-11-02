#include "src/core/catalog/usecases/legacy_cloud_stream/clearance.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchCheck;
using catalog::Platform;
using catalog::ServiceGeneration;
using catalog::StreamRefusal;
using libchiaking::users::Tier;
using Clearance =
    libchiaking::catalog::usecases::legacy_cloud_stream::Clearance;
using Kind = catalog::LaunchRefusal::Kind;

class LegacyCloudStreamClearance : public ::testing::Test, protected RuleCases {
 protected:
  static constexpr std::string_view far_cry =
      "EP0100-CUSA07016_00-FAKE0038TITLE000";
  static constexpr std::string_view far_cry_gold =
      "EP0100-CUSA07017_00-FAKE0039TITLE000";
  static constexpr std::string_view far_cry_copy =
      "EP0100-CUSA07016_00-FAKE0107TITLE000";

  static auto shelf() -> catalog::StreamableList {
    return list_of({on_old_service(far_cry, "Wide Fire 5"),
                    on_old_service(far_cry_gold, "Wide Fire 5 Gold Edition")});
  }

  static auto held_copy(std::optional<std::string> active_date = std::nullopt)
      -> Held {
    return Held{.id = far_cry_copy,
                .product = far_cry,
                .name = "Wide Fire 5",
                .platform = "ps4",
                .package = "PS4GS",
                .active_date = std::move(active_date)};
  }

  static auto kind_of(const catalog::LaunchVerdict& verdict) -> Kind {
    return ClearanceRig::kind_of(verdict);
  }

  ClearanceRig rig;
};

TEST_F(LegacyCloudStreamClearance,
       ClearsAShelfTitleForAPremiumMemberWithoutAskingPs) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({}));
  Clearance clearance{rig.checks};
  const auto verdict = clearance.clear(
      *user, seen, game_of(*seen.games, far_cry), Tier::premium, {});
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  const auto& cleared = *verdict.cleared_launch();
  EXPECT_EQ(cleared.product, product(far_cry));
  EXPECT_EQ(cleared.service, ServiceGeneration::old_service);
  EXPECT_EQ(cleared.platform, Platform::ps4);
  EXPECT_EQ(cleared.identifier.text(), far_cry);
  EXPECT_FALSE(cleared.held_as);
  EXPECT_EQ(cleared.origin, catalog::ListOrigin::account_service);
  EXPECT_EQ(cleared.store.text(), "en-GB");
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance, CarriesAHeldLicenceWithTheClearedLaunch) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({held_copy()}));
  Clearance clearance{rig.checks};
  const auto verdict = clearance.clear(
      *user, seen, game_of(*seen.games, far_cry), Tier::premium, {});
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), far_cry_copy);
  EXPECT_EQ(verdict.cleared_launch()->identifier.text(), far_cry);
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance, HidesACoveredTitleBeforeTheTier) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({held_copy()}));
  Clearance clearance{rig.checks};
  const auto covered = clearance.clear(
      *user, seen, game_of(*seen.games, far_cry_gold), std::nullopt, {});
  EXPECT_EQ(kind_of(covered), Kind::hidden);
  EXPECT_EQ(covered.refusal()->hide_reason(), HideReason::already_owned);
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance, RefusesARememberedRefusalBeforeTheTier) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({held_copy()}));
  Clearance clearance{rig.checks};
  const std::vector<StreamRefusal> remembered{
      StreamRefusal{account,
                    product(far_cry),
                    StreamRefusal::Kind::refused_by_stream_server,
                    at_ms(2900),
                    {std::nullopt, StreamRefusal::Holding::not_held()}}};
  const auto games = std::make_shared<const catalog::MyGames>(
      catalog::MyGamesAssembly::games_of(
          {.library = library_of({}), .list = seen.list.value().get()}, {},
          std::nullopt, remembered, 1, at_ms(3000)));
  const ClearanceRig::View refused{games, seen.list, seen.category};
  const auto verdict = clearance.clear(*user, refused, game_of(*games, far_cry),
                                       Tier::premium, {});
  EXPECT_EQ(kind_of(verdict), Kind::refused_by_service);
  EXPECT_EQ(verdict.refusal()->time(), at_ms(2900));
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance,
       JudgesADatedRefusedTitleByTheFreshClockFirst) {
  const auto user = rig.user();
  const auto seen = rig.holding(
      *user, shelf(), library_of({held_copy("2031-01-01T00:00:00.000Z")}));
  const std::vector<StreamRefusal> remembered{StreamRefusal{
      account,
      product(far_cry),
      StreamRefusal::Kind::refused_by_stream_server,
      at_ms(2900),
      {std::nullopt, StreamRefusal::Holding::held_as(
                         *catalog::OwnershipId::parse(far_cry_copy))}}};
  const auto games = std::make_shared<const catalog::MyGames>(
      catalog::MyGamesAssembly::games_of(
          {.library = library_of({held_copy("2031-01-01T00:00:00.000Z")}),
           .list = seen.list.value().get()},
          {}, std::nullopt, remembered, 1, at_ms(3000)));
  const ClearanceRig::View refused{games, seen.list, seen.category};
  const auto& dated = game_of(*games, far_cry);
  EXPECT_TRUE(dated.remembered_refusal());
  EXPECT_TRUE(dated.release().time());
  Clearance clearance{rig.checks};
  EXPECT_EQ(kind_of(clearance.clear(*user, refused, dated, Tier::premium, {})),
            Kind::no_trusted_clock);
  rig.server.current = ClearanceRig::at("2031-01-01T00:00:00Z");
  EXPECT_EQ(kind_of(clearance.clear(*user, refused, dated, Tier::premium, {})),
            Kind::not_released);
  rig.server.current = ClearanceRig::at("2031-01-01T00:00:01Z");
  EXPECT_EQ(kind_of(clearance.clear(*user, refused, dated, Tier::premium, {})),
            Kind::refused_by_service);
  EXPECT_EQ(rig.server.refreshes, 3);
}

TEST_F(LegacyCloudStreamClearance, RefusesAKnownLowerTierWithoutAsking) {
  for (const auto tier : {Tier::none, Tier::essential, Tier::extra}) {
    ClearanceRig fresh;
    const auto user = fresh.user();
    const auto seen = fresh.holding(*user, shelf(), library_of({}));
    Clearance clearance{fresh.checks};
    const auto verdict =
        clearance.clear(*user, seen, game_of(*seen.games, far_cry), tier, {});
    EXPECT_EQ(kind_of(verdict), Kind::needs_premium)
        << ::testing::PrintToString(tier);
    EXPECT_EQ(verdict.refusal()->tier_held(), tier);
    EXPECT_EQ(fresh.http.requests(), 0U);
  }
}

TEST_F(LegacyCloudStreamClearance, AsksAnUnknownTierOnceAndClearsPremium) {
  rig.membership_is("TIER_30");
  const auto member = rig.user();
  const auto seen = rig.holding(*member, shelf(), library_of({}));
  Clearance asking{rig.checks};
  EXPECT_TRUE(
      asking
          .clear(*member, seen, game_of(*seen.games, far_cry), std::nullopt, {})
          .is_allowed());
  EXPECT_EQ(rig.http.count(ClearanceRig::subscriptions), 1);
}

TEST_F(LegacyCloudStreamClearance, RefusesAnAskedLowerTier) {
  rig.membership_is("TIER_20");
  const auto lower = rig.user();
  const auto shown = rig.holding(*lower, shelf(), library_of({}));
  Clearance refusing{rig.checks};
  EXPECT_EQ(
      kind_of(refusing.clear(*lower, shown, game_of(*shown.games, far_cry),
                             std::nullopt, {})),
      Kind::needs_premium);
}

TEST_F(LegacyCloudStreamClearance,
       ReportsTheCheckUnavailableWhenTheTierCannotBeRead) {
  const auto unsure = rig.user();
  const auto listed = rig.holding(*unsure, shelf(), library_of({}));
  Clearance unanswered{rig.checks};
  EXPECT_EQ(
      kind_of(unanswered.clear(*unsure, listed, game_of(*listed.games, far_cry),
                               std::nullopt, {})),
      Kind::check_unavailable);
  EXPECT_GE(rig.http.count(ClearanceRig::subscriptions), 1);
  EXPECT_EQ(
      rig.http.requests(),
      static_cast<std::size_t>(rig.http.count(ClearanceRig::subscriptions)));
}

TEST_F(LegacyCloudStreamClearance, JudgesTheReleaseLastByPsClock) {
  const auto user = rig.user();
  const auto seen = rig.holding(
      *user, shelf(), library_of({held_copy("2031-01-01T00:00:00.000Z")}));
  const auto& dated = game_of(*seen.games, far_cry);
  Clearance clearance{rig.checks};
  EXPECT_EQ(kind_of(clearance.clear(*user, seen, dated, Tier::premium, {})),
            Kind::no_trusted_clock);
  rig.server.current = ClearanceRig::at("2030-12-31T00:00:00Z");
  const auto early = clearance.clear(*user, seen, dated, Tier::premium, {});
  EXPECT_EQ(kind_of(early), Kind::not_released);
  EXPECT_EQ(early.refusal()->time(), ClearanceRig::at("2031-01-01T00:00:00Z"));
  EXPECT_EQ(kind_of(clearance.clear(*user, seen, dated, Tier::extra, {})),
            Kind::needs_premium);
  rig.server.current = ClearanceRig::at("2031-01-02T00:00:00Z");
  EXPECT_TRUE(
      clearance.clear(*user, seen, dated, Tier::premium, {}).is_allowed());
  EXPECT_EQ(rig.http.requests(), 0U);
}

class LegacyCloudStreamClearanceProgress : public LegacyCloudStreamClearance {
 protected:
  std::vector<LaunchCheck> checks;
  catalog::LaunchCheckObserver observer{
      [this](LaunchCheck check) { checks.push_back(check); }};
  libchiaking::catalog::usecases::CheckProgress progress{observer};
};

TEST_F(LegacyCloudStreamClearanceProgress,
       TellsTheMembershipAndTheReleaseAsTheyStart) {
  rig.membership_is("TIER_30");
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({}));
  Clearance clearance{rig.checks};
  const auto& game = game_of(*seen.games, far_cry);
  EXPECT_TRUE(clearance.clear(*user, seen, game, std::nullopt, {}, progress)
                  .is_allowed());
  EXPECT_EQ(checks,
            (std::vector{LaunchCheck::membership, LaunchCheck::release}));
}

TEST_F(LegacyCloudStreamClearanceProgress, TellsOnlyTheReleaseForAKnownTier) {
  rig.membership_is("TIER_30");
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({}));
  Clearance clearance{rig.checks};
  const auto& game = game_of(*seen.games, far_cry);
  EXPECT_TRUE(clearance.clear(*user, seen, game, Tier::premium, {}, progress)
                  .is_allowed());
  EXPECT_EQ(checks, (std::vector{LaunchCheck::release}));
}

TEST_F(LegacyCloudStreamClearanceProgress, TellsNothingForAKnownLowerTier) {
  rig.membership_is("TIER_30");
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({}));
  Clearance clearance{rig.checks};
  const auto& game = game_of(*seen.games, far_cry);
  EXPECT_EQ(
      kind_of(clearance.clear(*user, seen, game, Tier::extra, {}, progress)),
      Kind::needs_premium);
  EXPECT_TRUE(checks.empty());
}

TEST_F(LegacyCloudStreamClearanceProgress, TellsNothingForAHiddenTitle) {
  const auto owner = rig.user();
  const auto covered = rig.holding(*owner, shelf(), library_of({held_copy()}));
  Clearance standing{rig.checks};
  EXPECT_EQ(kind_of(standing.clear(*owner, covered,
                                   game_of(*covered.games, far_cry_gold),
                                   std::nullopt, {}, progress)),
            Kind::hidden);
  EXPECT_TRUE(checks.empty());
}

TEST_F(LegacyCloudStreamClearance, ReadsOwnershipFromTheListedTitle) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({held_copy()}));
  const auto held = Clearance::ownership(game_of(*seen.games, far_cry));
  ASSERT_TRUE(held);
  EXPECT_TRUE(held.value().is_owned());
  EXPECT_EQ(held.value().game()->ownership()->id(), far_cry_copy);
  const auto covered = Clearance::ownership(game_of(*seen.games, far_cry_gold));
  ASSERT_TRUE(covered);
  EXPECT_FALSE(covered.value().is_owned());
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance,
       ReportsOwnershipUnknownWhenTheLibraryIsUnavailable) {
  const auto user = rig.user();
  const auto seen = rig.holding(*user, shelf(), library_of({held_copy()}));
  const auto unchecked = catalog::MyGamesAssembly::games_of(
      {.library = catalog::Library::of(account, {}, {}, Time{}).value(),
       .library_state = catalog::LibraryState::unavailable,
       .list = seen.list.value().get()},
      {}, std::nullopt, {}, 0, at_ms(3000));
  const auto unknown = Clearance::ownership(game_of(unchecked, far_cry));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().reason(),
            catalog::Unanswered::Reason::ownership_unknown);
  EXPECT_EQ(rig.http.requests(), 0U);
}

TEST_F(LegacyCloudStreamClearance, NeverAsksTheStorefront) {
  const auto store = Clearance::storefront();
  ASSERT_FALSE(store);
  EXPECT_EQ(store.error().reason(), catalog::Unanswered::Reason::rejected);
  EXPECT_EQ(rig.http.requests(), 0U);
}

}  // namespace
}  // namespace catalog_test
