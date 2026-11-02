#include "src/core/catalog/model/legacy_cloud_stream/games.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchVerdict;
using catalog::ListStreamability;
using catalog::Platform;
using catalog::PlayPath;
using catalog::ServiceGames;
using catalog::ServiceGeneration;
using catalog::Standing;
using catalog::StreamRefusal;
using libchiaking::users::Tier;
using Rule = catalog::Explanation::Rule;

class LegacyCloudStreamGames : public ::testing::Test, public RuleCases {
 protected:
  static constexpr std::string_view far_cry =
      "EP0100-CUSA07016_00-FAKE0038TITLE000";
  static constexpr std::string_view far_cry_gold =
      "EP0100-CUSA07017_00-FAKE0039TITLE000";
  static constexpr std::string_view far_cry_copy =
      "EP0100-CUSA07016_00-FAKE0107TITLE000";
  static constexpr std::string_view ps3_classic =
      "EP0109-NPEA07001_00-FAKE0113TITLE000";
  static constexpr std::string_view skyline =
      "EP0109-CUSA17003_00-FAKE0059TITLE000";
  static constexpr std::string_view skyline_copy =
      "EP0109-CUSA17003_00-FAKE0107TITLE000";
  static constexpr std::string_view remaster =
      "EP0109-PPSA17001_00-FAKE0056TITLE000";
  static constexpr std::string_view ps5_only =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";

  static auto legacy_cloud_stream_of(
      const catalog::Library& library, const catalog::StreamableList& list,
      std::optional<Tier> tier = std::nullopt,
      std::span<const StreamRefusal> refusals = {},
      catalog::LibraryState state = catalog::LibraryState::fetched)
      -> ServiceGames {
    catalog::MyGames::Inputs inputs;
    inputs.tier = tier;
    inputs.library = state;
    const catalog::RememberedRefusals remembered{account, refusals, tier,
                                                 at_ms(3000)};
    return catalog::legacy_cloud_stream::Games{library, list, inputs,
                                               remembered}
        .listed();
  }

  static auto row_of(const ServiceGames& games, std::string_view id)
      -> const ServiceGames::Listed& {
    const auto found = std::ranges::find_if(
        games.listed, [wanted = product(id)](const ServiceGames::Listed& row) {
          return row.game.product_id() == wanted;
        });
    EXPECT_NE(found, games.listed.end()) << id;
    if (found == games.listed.end()) throw std::out_of_range{std::string{id}};
    return *found;
  }

  static auto dropped_by(const ServiceGames& games, std::string_view id)
      -> std::optional<Rule> {
    const auto found = std::ranges::find(games.unlisted, product(id),
                                         &catalog::Explanation::product_id);
    return found != games.unlisted.end() ? failed_rule(*found) : std::nullopt;
  }

  static auto held_copy(std::string_view id, std::string_view under,
                        std::string_view name) -> Held {
    return Held{.id = id,
                .product = under,
                .name = name,
                .platform = "ps4",
                .package = "PS4GS"};
  }

  static auto all_hidden_by(const ServiceGames& games, Tier tier)
      -> testing::AssertionResult {
    for (const auto& row : games.listed) {
      const auto id = row.game.product_id().text();
      if (row.game.visibility().hide_reason() !=
          HideReason::tier_does_not_include)
        return testing::AssertionFailure()
               << id << " is not hidden for its tier";
      if (failed_rule(row.explanation) != Rule::tier)
        return testing::AssertionFailure()
               << id << " did not fail on the tier rule";
      if (row.explanation.first_failure()->tier != tier)
        return testing::AssertionFailure() << id << " failed on another tier";
    }
    return testing::AssertionSuccess();
  }

  static auto holds_the_streaming_copy(const ServiceGames& games)
      -> testing::AssertionResult {
    const auto& row = row_of(games, skyline);
    if (!row.game.is_owned())
      return testing::AssertionFailure() << "the row is not owned";
    if (row.game.ownership()->id() != skyline_copy)
      return testing::AssertionFailure()
             << "the row holds " << row.game.ownership()->id().text();
    if (row.game.ownership()->streams() != true)
      return testing::AssertionFailure() << "the held copy does not stream";
    if (!has_step(row.explanation, Rule::owned_duplicate))
      return testing::AssertionFailure() << "no owned duplicate step";
    return testing::AssertionSuccess();
  }
};

TEST_F(LegacyCloudStreamGames, OfferAShelfTitleWithoutALicence) {
  const auto games = legacy_cloud_stream_of(
      library_of({}), list_of({on_old_service(far_cry, "Wide Fire 5"),
                               on_old_service(ps3_classic, "Rivet & Bolt")}));
  EXPECT_EQ(games.listed.size(), 2U);
  EXPECT_TRUE(games.unlisted.empty());
  const auto& offered = row_of(games, far_cry);
  EXPECT_EQ(offered.game.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(offered.game.title().platform(), Platform::ps4);
  EXPECT_TRUE(offered.game.is_visible());
  EXPECT_FALSE(offered.game.is_owned());
  EXPECT_EQ(offered.game.play_path(), PlayPath::old_service);
  EXPECT_EQ(offered.game.standing(), Standing::included);
  EXPECT_TRUE(offered.game.inclusion().is_included());
  EXPECT_EQ(offered.game.launch_identifier().text(), far_cry);
  EXPECT_FALSE(offered.game.needs_library_add());
  EXPECT_FALSE(offered.game.needs_storefront_check());
  EXPECT_TRUE(LaunchVerdict::of(&offered.game, std::nullopt).is_allowed());
  EXPECT_TRUE(offered.explanation.is_listed());
  EXPECT_TRUE(has_step(offered.explanation, Rule::included_with_plus));
  EXPECT_TRUE(has_step(offered.explanation, Rule::already_owned));
  EXPECT_EQ(row_of(games, ps3_classic).game.title().platform(), Platform::ps3);
}

TEST_F(LegacyCloudStreamGames, HideAShelfTitleNoPlanCovers) {
  auto dropped = on_old_service(far_cry, "Wide Fire 5");
  dropped.streams = false;
  const auto list =
      list_of({dropped, on_old_service(skyline, "Skyline Zero Ember")});
  const auto* listed = list.find(product(far_cry));
  ASSERT_NE(listed, nullptr);
  EXPECT_EQ(listed->title.streamability(),
            ListStreamability::marked_unstreamable);

  const auto games = legacy_cloud_stream_of(library_of({}), list);
  EXPECT_EQ(games.listed.size(), 2U);
  EXPECT_TRUE(games.unlisted.empty());
  const auto& hidden = row_of(games, far_cry);
  EXPECT_FALSE(hidden.game.is_visible());
  EXPECT_EQ(hidden.game.visibility().hide_reason(), HideReason::not_in_service);
  EXPECT_FALSE(hidden.game.inclusion().is_included());
  EXPECT_EQ(failed_rule(hidden.explanation), Rule::included_with_plus);
  EXPECT_FALSE(has_step(hidden.explanation, Rule::already_owned));
  EXPECT_FALSE(LaunchVerdict::of(&hidden.game, std::nullopt).is_allowed());
  EXPECT_TRUE(row_of(games, skyline).game.is_visible());
}

TEST_F(LegacyCloudStreamGames, ShowAShelfTitleNoPlanCoversWhenItsCopyIsHeld) {
  auto dropped = on_old_service(far_cry, "Wide Fire 5");
  dropped.streams = false;
  const auto list =
      list_of({dropped, on_old_service(skyline, "Skyline Zero Ember")});
  const auto held = legacy_cloud_stream_of(
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5")}), list);
  const auto& licensed = row_of(held, far_cry);
  EXPECT_TRUE(licensed.game.is_visible());
  EXPECT_TRUE(licensed.game.is_owned());
  EXPECT_TRUE(LaunchVerdict::of(&licensed.game, std::nullopt).is_allowed());
}

TEST_F(LegacyCloudStreamGames, LicenseAShelfTitleThroughItsHeldCopy) {
  const auto list = list_of({on_old_service(far_cry, "Wide Fire 5")});
  const auto games = legacy_cloud_stream_of(
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5"),
                  {.id = far_cry,
                   .product = far_cry,
                   .name = "Wide Fire 5",
                   .platform = "ps4",
                   .package = "PS4GD"}}),
      list);
  EXPECT_EQ(games.listed.size(), 1U);
  EXPECT_TRUE(games.unlisted.empty());
  const auto& licensed = row_of(games, far_cry);
  EXPECT_TRUE(licensed.game.is_owned());
  EXPECT_TRUE(licensed.game.is_visible());
  EXPECT_EQ(licensed.game.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(licensed.game.play_path(), PlayPath::old_service);
  EXPECT_EQ(licensed.game.launch_identifier().text(), far_cry);
  EXPECT_TRUE(has_step(licensed.explanation, Rule::merged_into_listing));
  EXPECT_TRUE(has_step(licensed.explanation, Rule::owned_duplicate));
  EXPECT_FALSE(has_step(licensed.explanation, Rule::already_owned));
}

TEST_F(LegacyCloudStreamGames, CoverTheShelfEditionsBesideALicensedOne) {
  const auto games = legacy_cloud_stream_of(
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5")}),
      list_of({on_old_service(far_cry, "Wide Fire 5"),
               on_old_service(far_cry_gold, "Wide Fire 5 Gold Edition")}));
  const auto& covered = row_of(games, far_cry_gold);
  EXPECT_FALSE(covered.game.is_visible());
  EXPECT_FALSE(covered.game.is_owned());
  EXPECT_EQ(covered.game.visibility().hide_reason(), HideReason::already_owned);
  EXPECT_EQ(covered.game.visibility().because_of(), product(far_cry));
  EXPECT_EQ(failed_rule(covered.explanation), Rule::already_owned);
  EXPECT_TRUE(row_of(games, far_cry).game.is_visible());
}

TEST_F(LegacyCloudStreamGames, HideEveryTitleBelowPremium) {
  const auto list =
      list_of({on_old_service(far_cry, "Wide Fire 5"),
               on_old_service(far_cry_gold, "Wide Fire 5 Gold Edition")});
  const auto library =
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5")});
  for (const auto tier : {Tier::none, Tier::essential, Tier::extra}) {
    const auto games = legacy_cloud_stream_of(library, list, tier);
    EXPECT_EQ(games.listed.size(), 2U);
    EXPECT_TRUE(all_hidden_by(games, tier));
    EXPECT_TRUE(row_of(games, far_cry).game.is_owned());
  }
}

TEST_F(LegacyCloudStreamGames, ShowTitlesOnPremium) {
  const auto list =
      list_of({on_old_service(far_cry, "Wide Fire 5"),
               on_old_service(far_cry_gold, "Wide Fire 5 Gold Edition")});
  const auto library =
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5")});
  const auto premium = legacy_cloud_stream_of(library, list, Tier::premium);
  EXPECT_TRUE(row_of(premium, far_cry).game.is_visible());
  EXPECT_TRUE(has_step(row_of(premium, far_cry).explanation, Rule::tier));
}

TEST_F(LegacyCloudStreamGames, HoldTheStreamingCopyOverTheBoughtOne) {
  const auto list = list_of({on_old_service(
      skyline, "Skyline Zero Ember Complete Edition", "900002")});
  const Held bought{.id = skyline,
                    .product = skyline,
                    .name = "Skyline Zero Ember",
                    .platform = "ps4",
                    .package = "PS4GD"};
  const auto streaming = held_copy(skyline_copy, skyline, "Skyline Zero Ember");
  EXPECT_TRUE(holds_the_streaming_copy(
      legacy_cloud_stream_of(library_of({bought, streaming}), list)));
  EXPECT_TRUE(holds_the_streaming_copy(
      legacy_cloud_stream_of(library_of({streaming, bought}), list)));
  const auto alone = legacy_cloud_stream_of(library_of({bought}), list);
  const auto& held = row_of(alone, skyline);
  ASSERT_TRUE(held.game.ownership());
  EXPECT_EQ(held.game.ownership()->id(), skyline);
  EXPECT_FALSE(held.game.ownership()->streams());
}

TEST_F(LegacyCloudStreamGames,
       LeaveOutAHeldCopyNoShelfCarriesThoughARelatedEntryIsListed) {
  const auto list = list_of({{.product = remaster,
                              .name = "Skyline Zero Ember Remastered",
                              .concept_id = "900002",
                              .cover = "https://cover/remaster.png"}});
  const auto library = library_of({
      held_copy(skyline_copy, skyline, "Skyline Zero Ember"),
      {.id = remaster,
       .product = skyline,
       .name = "Skyline Zero Ember",
       .active = false},
  });
  const auto games = legacy_cloud_stream_of(library, list);
  EXPECT_TRUE(games.listed.empty());
  EXPECT_EQ(dropped_by(games, skyline), Rule::listed_by_service);
  const auto why = std::ranges::find(games.unlisted, product(skyline),
                                     &catalog::Explanation::product_id);
  ASSERT_NE(why, games.unlisted.end());
  ASSERT_FALSE(why->steps().empty());
  const auto& matched = why->steps().front();
  EXPECT_EQ(matched.rule, Rule::library_match);
  EXPECT_EQ(matched.outcome, catalog::Explanation::Outcome::passed);
  EXPECT_EQ(matched.product, product(remaster));
  ASSERT_TRUE(matched.ownership);
  EXPECT_EQ(matched.ownership->text(), skyline_copy);
}

TEST_F(LegacyCloudStreamGames, ListAHeldCopyThatAStoreAppListStreamsAsItself) {
  constexpr std::string_view held = "UP0114-CUSA07036_00-FAKE0089TITLE000";
  const auto list = list_of(
      {{.product = held, .name = "Owner Stream", .ps5 = false, .ps4 = true}});
  EXPECT_TRUE(legacy_cloud_stream_of(library_of({}), list).listed.empty());
  const auto games = legacy_cloud_stream_of(library_of({{.id = held,
                                                         .product = held,
                                                         .name = "Owner Stream",
                                                         .platform = "ps4",
                                                         .package = "PS4GD"}}),
                                            list);
  EXPECT_EQ(games.listed.size(), 1U);
  EXPECT_TRUE(games.unlisted.empty());
  const auto& listed = row_of(games, held);
  EXPECT_TRUE(listed.game.is_owned());
  EXPECT_TRUE(listed.game.is_visible());
  EXPECT_EQ(listed.game.title().streamability(), ListStreamability::listed);
  EXPECT_EQ(listed.game.launch_identifier().text(), held);
}

TEST_F(LegacyCloudStreamGames, DropAHeldCopyTheListDoesNotStreamFromEveryList) {
  constexpr std::string_view supplement =
      "UP0114-CUSA07034_00-FAKE0125TITLE000";
  const std::initializer_list<Listed> store{
      {.product = supplement,
       .name = "Supplement Only",
       .ps5 = false,
       .ps4 = true,
       .streams = std::nullopt},
      {.product = supplement,
       .name = "Supplement Only",
       .shelf = catalog::Shelf::plus_games,
       .ps5 = false,
       .ps4 = true,
       .streams = std::nullopt},
  };
  const auto library = library_of({{.id = supplement,
                                    .product = supplement,
                                    .name = "Supplement Only",
                                    .platform = "ps4",
                                    .package = "PS4GD"}});
  const auto signed_in = legacy_cloud_stream_of(library, list_of(store));
  EXPECT_TRUE(signed_in.listed.empty());
  EXPECT_EQ(dropped_by(signed_in, supplement), Rule::listed_by_service);
  const auto fallback = legacy_cloud_stream_of(
      library, list_of(store, catalog::ListOrigin::public_store));
  EXPECT_TRUE(fallback.listed.empty());
  EXPECT_EQ(dropped_by(fallback, supplement), Rule::listed_by_service);
}

TEST_F(LegacyCloudStreamGames, LeavePs5CloudRecordsAndListingsAlone) {
  const auto list =
      list_of({on_old_service(far_cry, "Wide Fire 5"),
               {.product = ps5_only, .name = "League Diamond 26"}});
  const auto games = legacy_cloud_stream_of(
      library_of({
          {.id = ps5_only, .product = ps5_only, .name = "League Diamond 26"},
          {.id = far_cry,
           .product = far_cry,
           .name = "Wide Fire 5",
           .package = "PSGD"},
          {.id = "EP0100-PPSA07039_00-FAKE0040TITLE000",
           .product = far_cry,
           .name = "Wide Fire 5"},
      }),
      list);
  EXPECT_EQ(games.listed.size(), 1U);
  EXPECT_TRUE(games.unlisted.empty());
  const auto& offered = row_of(games, far_cry);
  EXPECT_FALSE(offered.game.is_owned());
  EXPECT_TRUE(offered.game.is_visible());
  EXPECT_EQ(offered.game.title().service(), ServiceGeneration::old_service);
}

TEST_F(LegacyCloudStreamGames, LeaveOutACopyOfATitleTheStoreListsForPs5) {
  const auto games = legacy_cloud_stream_of(
      library_of({{.id = "EP0109-CUSA37001_00-FAKE0137TITLE000",
                   .product = ps5_only,
                   .name = "League Diamond 26",
                   .platform = "ps4",
                   .package = "PS4GD"}}),
      list_of({{.product = ps5_only, .name = "League Diamond 26"}}));
  EXPECT_TRUE(games.listed.empty());
  EXPECT_EQ(dropped_by(games, ps5_only), Rule::service);
}

TEST_F(LegacyCloudStreamGames, NameTheRuleThatDroppedAnUnlistedRecord) {
  constexpr std::string_view unlisted = "UP0114-CUSA07032_00-FAKE0076TITLE000";
  const auto games = legacy_cloud_stream_of(
      library_of({
          {.id = unlisted,
           .product = unlisted,
           .name = "Unlisted Game",
           .platform = "ps4",
           .package = "PS4GD"},
          {.id = "EP0100-CUSA07016_00-FAKE0129TITLE000",
           .product = far_cry,
           .name = "Wide Fire 5",
           .platform = "ps4",
           .feature = catalog::OwnedRecord::trial_access_feature,
           .package = "PS4GD"},
          {.id = far_cry,
           .product = far_cry,
           .name = "Wide Fire 5",
           .platform = "ps4",
           .package = "PS4GD"},
      }),
      list_of({on_old_service(far_cry, "Wide Fire 5")}));
  EXPECT_EQ(dropped_by(games, unlisted), Rule::library_match);
  EXPECT_EQ(dropped_by(games, far_cry), Rule::trial_superseded);
  const auto& licensed = row_of(games, far_cry);
  ASSERT_TRUE(licensed.game.ownership());
  EXPECT_EQ(licensed.game.ownership()->id(), far_cry);
  EXPECT_FALSE(has_step(licensed.explanation, Rule::owned_duplicate));
}

TEST_F(LegacyCloudStreamGames, RememberARefusalOfAnUnlicensedTitle) {
  using Holding = StreamRefusal::Holding;
  const auto list = list_of({on_old_service(far_cry, "Wide Fire 5")});
  const std::vector<StreamRefusal> unlicensed{
      StreamRefusal{account,
                    product(far_cry),
                    StreamRefusal::Kind::refused_by_stream_server,
                    at_ms(2900),
                    {std::nullopt, Holding::not_held()}}};
  const auto refused =
      legacy_cloud_stream_of(library_of({}), list, std::nullopt, unlicensed);
  EXPECT_TRUE(row_of(refused, far_cry).game.remembered_refusal());
  EXPECT_TRUE(
      has_step(row_of(refused, far_cry).explanation, Rule::remembered_refusal));
  EXPECT_FALSE(LaunchVerdict::of(&row_of(refused, far_cry).game, std::nullopt)
                   .is_allowed());
}

TEST_F(LegacyCloudStreamGames, ForgetARefusalOnceTheTitleIsLicensed) {
  using Holding = StreamRefusal::Holding;
  const auto list = list_of({on_old_service(far_cry, "Wide Fire 5")});
  const std::vector<StreamRefusal> unlicensed{
      StreamRefusal{account,
                    product(far_cry),
                    StreamRefusal::Kind::refused_by_stream_server,
                    at_ms(2900),
                    {std::nullopt, Holding::not_held()}}};
  const auto licensed_since = legacy_cloud_stream_of(
      library_of({held_copy(far_cry_copy, far_cry, "Wide Fire 5")}), list,
      std::nullopt, unlicensed);
  EXPECT_FALSE(row_of(licensed_since, far_cry).game.remembered_refusal());
}

TEST_F(LegacyCloudStreamGames, KeepARefusalWhileTheLibraryIsUnknown) {
  using Holding = StreamRefusal::Holding;
  const auto list = list_of({on_old_service(far_cry, "Wide Fire 5")});
  const std::vector<StreamRefusal> unlicensed{
      StreamRefusal{account,
                    product(far_cry),
                    StreamRefusal::Kind::refused_by_stream_server,
                    at_ms(2900),
                    {std::nullopt, Holding::not_held()}}};
  const auto unknown =
      legacy_cloud_stream_of(library_of({}), list, std::nullopt, unlicensed,
                             catalog::LibraryState::unavailable);
  EXPECT_TRUE(row_of(unknown, far_cry).game.remembered_refusal());
  EXPECT_EQ(row_of(unknown, far_cry).game.ownership_state(),
            catalog::OwnershipState::unknown);
  EXPECT_FALSE(row_of(unknown, far_cry).game.needs_ownership_check());
}

}  // namespace
}  // namespace catalog_test
