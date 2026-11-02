#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace catalog_api_test {
namespace {

using OwnedLibrary = CatalogApiTest;

TEST_F(OwnedLibrary, ThatPsRefusesToShowAsksForSignInAndIsNotKept) {
  Rig refusing;
  const auto barred = refusing.user();
  ps_as_usual(refusing);
  refusing.http.on("GET", std::string{owned_list}, {{403, ""}});
  const auto refused = refusing.catalog.process_library_and_list(*barred);
  ASSERT_FALSE(refused);
  EXPECT_TRUE(refused.error().needs_sign_in());
  EXPECT_FALSE(refusing.has_record("user/1/library"));
}

TEST_F(OwnedLibrary, ThatCannotBeReadIsNotGuessed) {
  Rig broken;
  const auto user = broken.user();
  ps_as_usual(broken);
  broken.http.on("GET", std::string{owned_list}, {{200, "not json"}});
  premium_includes(broken,
                   {premium_offer(comet, "Comet"),
                    premium_offer(rivet, "Rivet & Bolt: Split Worlds")});
  const auto unread = broken.catalog.process_library_and_list(*user);
  ASSERT_TRUE(unread);
  EXPECT_FALSE(unread.value()->ownership_is_known());
  EXPECT_TRUE(
      unread.value()->ownership_is_known_on(ServiceGeneration::ps5_cloud));
  EXPECT_TRUE(game_of(*unread.value(), rivet).is_owned());
  EXPECT_TRUE(game_of(*unread.value(), comet).needs_library_add());
  EXPECT_EQ(broken.http.count(ps5_owned_list), 1);
  const auto library = broken.catalog.library_of(*user);
  ASSERT_FALSE(library);
  EXPECT_FALSE(library.error().needs_sign_in());
  EXPECT_FALSE(library.error().is_cancelled());
  EXPECT_FALSE(broken.has_record("user/1/library"));
  EXPECT_TRUE(broken.has_record("streamable/gb-en"));
  EXPECT_TRUE(broken.catalog.get_streamable_list(*user));
}

TEST_F(OwnedLibrary, ThatCannotBeFetchedLeavesOwnershipUnknown) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig,
                  store_list({comet_listed, rivet_listed, listed_only_listed}),
                  store_list({comet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  rig.http.on("GET", std::string{owned_list}, {{500, "{}"}});
  rig.http.on("GET", std::string{ps5_owned_list}, {{500, "{}"}});
  premium_includes(rig, {premium_offer(comet, "Comet"),
                         premium_offer(rivet, "Rivet & Bolt: Split Worlds")});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, rivet, streams_notice);

  const auto games = loaded(rig, *user);
  EXPECT_EQ(games->library_state(), catalog::LibraryState::unavailable);
  EXPECT_FALSE(games->ownership_is_known());
  EXPECT_EQ(shown(*games), 3U);
  EXPECT_TRUE(games->hidden().empty());
  EXPECT_FALSE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));
  const auto& held = game_of(*games, rivet);
  EXPECT_FALSE(held.is_owned());
  EXPECT_EQ(held.ownership_state(), catalog::OwnershipState::unknown);
  EXPECT_TRUE(held.needs_ownership_check());
  EXPECT_FALSE(held.needs_library_add());
  EXPECT_TRUE(held.is_visible());
  EXPECT_EQ(held.standing(), Standing::ownership_unknown);
  EXPECT_EQ(held.play_path(), PlayPath::included);
  const auto& offered = game_of(*games, comet);
  EXPECT_TRUE(offered.needs_ownership_check());
  EXPECT_FALSE(offered.needs_library_add());
  EXPECT_TRUE(offered.can_be_added_to_library());
  EXPECT_EQ(offered.play_path(), PlayPath::included);
  EXPECT_EQ(offered.standing(), Standing::ownership_unknown);
  const auto& old_title = game_of(*games, classic);
  EXPECT_EQ(old_title.standing(), Standing::included);
  EXPECT_FALSE(old_title.needs_ownership_check());
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(classic)).cleared_launch());

  const auto asked = rig.http.requests();
  const auto unchecked = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(unchecked), LaunchRefusal::Kind::check_unavailable);
  EXPECT_TRUE(unchecked.refusal()->asks_for_ownership_check());
  ASSERT_TRUE(unchecked.refusal()->cause());
  EXPECT_EQ(unchecked.refusal()->cause()->reason(),
            catalog::LaunchCheckReason::ownership_unknown);
  const auto comet_unchecked = rig.catalog.can_launch(*user, product(comet));
  ASSERT_TRUE(comet_unchecked.refusal());
  EXPECT_TRUE(comet_unchecked.refusal()->asks_for_ownership_check());
  EXPECT_TRUE(
      rig.catalog.explain(*user, product(rivet)).could_not_check_ownership());
  EXPECT_TRUE(rig.catalog.explain(*user, product(rivet)).is_listed());
  EXPECT_FALSE(
      rig.catalog.explain(*user, product(classic)).could_not_check_ownership());
  EXPECT_EQ(rig.http.requests(), asked);
  const auto library = rig.catalog.library_of(*user);
  ASSERT_FALSE(library);
  EXPECT_FALSE(library.error().needs_sign_in());
  EXPECT_FALSE(library.error().is_cancelled());

  rig.http.before_everything(
      "GET", "internal/entitlements/EP0109-PPSA07028_00-FAKE0110TITLE000",
      {{200, rivet_owned}});
  std::vector<LaunchCheck> checks;
  const catalog::LaunchCheckObserver observer{
      [&checks](LaunchCheck check) { checks.push_back(check); }};
  const auto cleared =
      rig.catalog.clear_for_launch(*user, product(rivet), {}, observer);
  EXPECT_TRUE(cleared.is_allowed());
  ASSERT_TRUE(cleared.cleared_launch());
  ASSERT_TRUE(cleared.cleared_launch()->held_as);
  EXPECT_EQ(cleared.cleared_launch()->held_as->text(), rivet);
  EXPECT_EQ(checks,
            (std::vector{LaunchCheck::loading_library,
                         LaunchCheck::cloud_streaming, LaunchCheck::ownership,
                         LaunchCheck::entitlement, LaunchCheck::cloud_game,
                         LaunchCheck::exclusions, LaunchCheck::release}));
  EXPECT_EQ(
      rig.http.count("internal/entitlements/"
                     "EP0109-PPSA07028_00-FAKE0110TITLE000?fields=gameMeta"),
      2);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(store_app), 0);
  EXPECT_FALSE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).cleared_launch());
  const auto partly = loaded(rig, *user);
  EXPECT_FALSE(partly->ownership_is_known());
  EXPECT_TRUE(game_of(*partly, rivet).is_owned());
  EXPECT_EQ(game_of(*partly, rivet).standing(), Standing::in_library);
  EXPECT_EQ(game_of(*partly, comet).ownership_state(),
            catalog::OwnershipState::unknown);
  EXPECT_FALSE(rig.has_record("user/1/library"));

  const auto stores = rig.http.count(store_app);
  const auto unowned = rig.catalog.clear_for_launch(*user, product(comet));
  ASSERT_EQ(refusal_kind(unowned), LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(unowned.cleared_launch());
  EXPECT_FALSE(unowned.refusal()->asks_for_ownership_check());
  EXPECT_EQ(rig.http.count(
                "internal/entitlements/EP-PPSA10001_00-GAME?fields=gameMeta"),
            1);
  EXPECT_EQ(rig.http.count(store_app), stores);
  ASSERT_TRUE(unowned.refusal()->library_offer());
  EXPECT_EQ(unowned.refusal()->library_offer()->sku_id,
            "EP-PPSA10001_00-GAME-E001");
  const auto unbought =
      rig.catalog.clear_for_launch(*user, product(listed_only));
  EXPECT_EQ(refusal_kind(unbought), LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(rig.http.count(store_app), stores);
  const auto answer = rig.catalog.recheck_ownership(*user, product(comet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().kind(), OwnershipAnswer::Kind::not_owned);
  const auto classic_answer =
      rig.catalog.recheck_ownership(*user, product(classic));
  ASSERT_FALSE(classic_answer);
  EXPECT_EQ(classic_answer.error().reason(),
            catalog::OwnershipRecheckReason::ownership_unknown);

  rig.http.on("GET", std::string{ps5_owned_lookup}, {{500, "{}"}});
  const auto unreachable = rig.catalog.clear_for_launch(*user, product(comet));
  ASSERT_EQ(refusal_kind(unreachable), LaunchRefusal::Kind::check_unavailable);
  EXPECT_FALSE(unreachable.cleared_launch());
  EXPECT_FALSE(unreachable.refusal()->asks_for_ownership_check());
  ASSERT_TRUE(unreachable.refusal()->cause());
  EXPECT_FALSE(unreachable.refusal()->cause()->needs_sign_in());
  EXPECT_EQ(game_of(*loaded(rig, *user), comet).ownership_state(),
            catalog::OwnershipState::unknown);

  rig.http.on("GET", std::string{ps5_owned_lookup}, {{404, "{}"}});
  library_pages(rig, {{200, owned_page({rivet_owned})}});
  const auto recovered = loaded(rig, *user);
  EXPECT_TRUE(recovered->ownership_is_known());
  EXPECT_EQ(recovered->library_state(), catalog::LibraryState::fetched);
  EXPECT_TRUE(rig.has_record("user/1/library"));
  EXPECT_TRUE(game_of(*recovered, rivet).is_owned());
  EXPECT_EQ(game_of(*recovered, rivet).play_path(), PlayPath::owned);
  EXPECT_EQ(game_of(*recovered, comet).ownership_state(),
            catalog::OwnershipState::not_owned);
  EXPECT_TRUE(game_of(*recovered, comet).needs_library_add());
  EXPECT_EQ(recovered->find(product(listed_only)), nullptr);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(comet))),
            LaunchRefusal::Kind::add_to_library);
  EXPECT_FALSE(
      rig.catalog.explain(*user, product(comet)).could_not_check_ownership());
  EXPECT_EQ(rig.catalog.library_of(*user).value()->size(), 1U);
}

class OwnedLibrarySources : public CatalogApiTest {
 protected:
  static inline const std::string classic_owned =
      R"({"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","product_id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","active_flag":true,"feature_type":3,"entitlement_attributes":[{"platform_id":"ps4"}],"game_meta":{"name":"Classic","package_type":"PS4GD"}})";
  static inline const std::string comet_in_commerce =
      R"({"id":"EP-PPSA10001_00-GAME","product_id":"EP-PPSA10001_00-GAME","active_flag":true,"feature_type":3,"entitlement_attributes":[{"platform_id":"ps5"}],"game_meta":{"name":"Comet","package_type":"PS5GD","type":"PS5_GAME"}})";
};

TEST_F(OwnedLibrarySources,
       ReadPs5OwnershipFromThePs5LibraryAndLegacyCloudStreamFromCommerce) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}),
                  store_list({comet_listed}));
  library_holds(rig,
                owned_page({rivet_owned, comet_in_commerce, classic_owned}));
  premium_includes(rig, {premium_offer(comet, "Comet"),
                         premium_offer(rivet, "Rivet & Bolt: Split Worlds")});
  const auto library = rig.catalog.library_of(*user);
  ASSERT_TRUE(library);
  EXPECT_EQ(library.value()->size(), 2U);
  EXPECT_EQ(library.value()->ps5_ownership(), catalog::LibraryState::fetched);
  const auto* rift = library.value()->find(*catalog::OwnershipId::parse(rivet));
  ASSERT_NE(rift, nullptr);
  EXPECT_EQ(rift->service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(rift->streams(), true);
  EXPECT_EQ(rift->held_as(), catalog::HeldAs::full_game);
  const auto* old =
      library.value()->find(*catalog::OwnershipId::parse(classic));
  ASSERT_NE(old, nullptr);
  EXPECT_EQ(old->service(), ServiceGeneration::old_service);
  EXPECT_EQ(library.value()->find(*catalog::OwnershipId::parse(comet)),
            nullptr);
  EXPECT_EQ(rig.http.count(owned_list), 1);
  EXPECT_EQ(rig.http.count(ps5_owned_list), 1);
  const auto games = loaded(rig, *user);
  EXPECT_TRUE(games->ownership_is_known());
  EXPECT_TRUE(games->ownership_is_known_on(ServiceGeneration::ps5_cloud));
  EXPECT_TRUE(game_of(*games, rivet).is_owned());
  EXPECT_TRUE(game_of(*games, classic).is_owned());
  EXPECT_EQ(game_of(*games, comet).ownership_state(),
            catalog::OwnershipState::not_owned);
  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  ASSERT_TRUE(report->cloud_stream.owned);
  EXPECT_EQ(report->cloud_stream.owned->state, catalog::LibraryState::fetched);
  EXPECT_EQ(report->cloud_stream.owned->records_read, 3U);
  EXPECT_EQ(report->cloud_stream.owned->revision_restarts, 0U);
}

TEST_F(OwnedLibrarySources, LeavePs5OwnershipUnknownWhenThePs5LibraryFails) {
  Rig lost;
  const auto unsure = lost.user();
  old_service_lists(lost);
  ps5_store_lists(lost, store_list({comet_listed, rivet_listed}),
                  store_list({comet_listed}));
  library_holds(lost, owned_page({rivet_owned, classic_owned}));
  premium_includes(lost, {premium_offer(comet, "Comet"),
                          premium_offer(rivet, "Rivet & Bolt: Split Worlds")});
  lost.http.on("GET", std::string{ps5_owned_list}, {{500, ""}});
  const auto partly = lost.catalog.library_of(*unsure);
  ASSERT_TRUE(partly);
  EXPECT_EQ(partly.value()->size(), 1U);
  EXPECT_EQ(partly.value()->ps5_ownership(),
            catalog::LibraryState::unavailable);
  const auto unknown = loaded(lost, *unsure);
  EXPECT_EQ(unknown->library_state(), catalog::LibraryState::fetched);
  EXPECT_TRUE(unknown->ownership_is_known());
  EXPECT_FALSE(unknown->ownership_is_known_on(ServiceGeneration::ps5_cloud));
  EXPECT_TRUE(game_of(*unknown, classic).is_owned());
  EXPECT_EQ(game_of(*unknown, rivet).ownership_state(),
            catalog::OwnershipState::unknown);
  EXPECT_EQ(game_of(*unknown, comet).ownership_state(),
            catalog::OwnershipState::unknown);
  EXPECT_EQ(refusal_kind(lost.catalog.can_launch(*unsure, product(rivet))),
            LaunchRefusal::Kind::check_unavailable);
  const auto lost_report = lost.catalog.how_fetched(*unsure);
  ASSERT_TRUE(lost_report);
  ASSERT_TRUE(lost_report->cloud_stream.owned);
  EXPECT_EQ(lost_report->cloud_stream.owned->state,
            catalog::LibraryState::unavailable);
  EXPECT_EQ(lost.http.count(owned_list), 1);
  EXPECT_GE(lost.http.count(ps5_owned_list), 2);

  lost.http.on("GET", std::string{ps5_owned_list},
               {{200, owned_page({rivet_owned})}});
  const auto found = loaded(lost, *unsure);
  EXPECT_TRUE(found->ownership_is_known_on(ServiceGeneration::ps5_cloud));
  EXPECT_TRUE(game_of(*found, rivet).is_owned());
  EXPECT_TRUE(game_of(*found, classic).is_owned());
  EXPECT_EQ(lost.http.count(owned_list), 1);
}

}  // namespace
}  // namespace catalog_api_test
