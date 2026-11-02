#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace catalog_api_test {
namespace {

using RegionalStore = CatalogApiTest;
using CloudAccessCheck = CatalogApiTest;
using Unreachable = CatalogApiTest;

TEST_F(RegionalStore, OfTheAccountCountryIsReadWithoutSigningIn) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  old_service_opens(rig);
  rig.http.on("GET", std::string{old_service_stores}, {{404, "{}"}});
  rig.http.on(
      "GET", "container/GB/en/19/STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"EP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  cloud_streaming_allowed(rig);
  const auto list = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(list);
  EXPECT_EQ(list.value()->origin(), ListOrigin::regional_store);
  EXPECT_EQ(list.value()->region().text(), "en-GB");
  EXPECT_TRUE(list.value()->covers(product("EP-CUSA00002_00-X")));
  EXPECT_TRUE(rig.has_record("streamable/gb-en"));
  EXPECT_EQ(user->region(), (users::Region{"GB", "en"}));
  EXPECT_EQ(rig.http.count("/user/session"), 0);
  EXPECT_EQ(rig.http.count(old_service_stores), 0);
  const auto games = loaded(rig, *user);
  EXPECT_TRUE(game_of(*games, rivet).is_owned());
  const auto verdict = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->origin, ListOrigin::regional_store);
  EXPECT_EQ(rig.http.count("STORE-MSF192014-APOLLOROOT"), 1);
  EXPECT_TRUE(rig.has_record("user/1/library"));

  const auto access = rig.catalog.cloud_access_of(*user);
  ASSERT_TRUE(access);
  EXPECT_EQ(access.value().availability(),
            CloudAccess::Availability::region_unsupported);
  EXPECT_FALSE(access.value().listing());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
}

TEST_F(RegionalStore, IsTheSharedPublicStoreForACountryWithoutOne) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user(1, users::Region{"JP", "ja"});
  rig.http.on("GET", "container/JP/ja/19/STORE-MSF192014-APOLLOROOT",
              {{404, "{}"}});
  rig.http.on(
      "GET", "container/GB/en/19/STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"EP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  cloud_streaming_allowed(rig);
  const auto list = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(list);
  EXPECT_EQ(list.value()->origin(), ListOrigin::public_store);
  EXPECT_TRUE(list.value()->is_complete());
  EXPECT_EQ(list.value()->region().text(), "ja-JP");
  EXPECT_TRUE(list.value()->covers(product("EP-CUSA00002_00-X")));
  EXPECT_FALSE(rig.has_record("streamable/jp-ja"));
  EXPECT_EQ(rig.http.count("/user/session"), 0);
  const auto games = loaded(rig, *user);
  EXPECT_TRUE(game_of(*games, rivet).is_owned());
  const auto verdict = rig.catalog.can_launch(*user, product(rivet));
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->origin, ListOrigin::public_store);
}

TEST_F(RegionalStore, ReplacesAnUnreachableOldServiceButIsNeverKept) {
  Rig rig;
  const auto user = rig.user(1, users::Region{"GB", "en"});
  rig.http.on("GET", "response_type=code", {{503, ""}});
  rig.http.on("GET", "STORE-MSF192014-APOLLOROOT", {{503, ""}});
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  const auto games = rig.catalog.process_library_and_list(*user);
  ASSERT_TRUE(games);
  EXPECT_TRUE(game_of(*games.value(), rivet).is_owned());
  const auto list = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(list);
  EXPECT_EQ(list.value()->origin(), ListOrigin::public_store);
  EXPECT_FALSE(list.value()->is_complete());
  EXPECT_FALSE(rig.has_record("streamable/gb-en"));
  EXPECT_TRUE(rig.has_record("user/1/library"));
}

TEST_F(RegionalStore, FallsBackToTheUsStoreInTheTravellersLocale) {
  Rig abroad;
  const auto traveller = abroad.user(1, users::Region{"BR", "pt-BR"});
  abroad.http.on("GET", "container/BR/pt/19/STORE-MSF192018-APOLLOROOT",
                 {{404, "{}"}});
  abroad.http.on(
      "GET", "container/US/en/19/STORE-MSF192018-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"UP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  ps5_store_lists(abroad, store_list({comet_listed}));
  library_holds(abroad, owned_page({}));
  const auto fallback = abroad.catalog.get_streamable_list(*traveller);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback.value()->origin(), ListOrigin::public_store);
  EXPECT_TRUE(fallback.value()->is_complete());
  EXPECT_EQ(fallback.value()->region().text(), "pt-BR");
  EXPECT_EQ(fallback.value()->settled_locale().text(), "pt-BR");
  EXPECT_EQ(abroad.http.count("locale=pt-br&"), 6);
  EXPECT_EQ(abroad.http.count("/user/session"), 0);
  EXPECT_TRUE(abroad.records.names().empty());
}

TEST_F(RegionalStore, AnIncompletePs5ListIsNeverKept) {
  Rig partial;
  const auto reader = partial.user();
  old_service_lists(partial);
  partial.http.on("GET", std::string{all_ps5_list}, {{500, ""}});
  partial.http.on("GET", "imagic/gameslist", {{200, "[]"}});
  library_holds(partial, owned_page({rivet_owned}));
  const auto incomplete = partial.catalog.process_library_and_list(*reader);
  EXPECT_TRUE(incomplete);
  EXPECT_FALSE(
      partial.catalog.get_streamable_list(*reader).value()->is_complete());
  EXPECT_FALSE(partial.has_record("streamable/gb-en"));
  EXPECT_TRUE(partial.has_record("user/1/library"));
  EXPECT_EQ(partial.http.count(old_service_stores), 2);
}

TEST_F(CloudAccessCheck, IsShownByTheOldServiceAndHeldForAQuarterHour) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  const auto access = rig.catalog.cloud_access_of(*user);
  ASSERT_TRUE(access);
  EXPECT_TRUE(access.value().is_available());
  EXPECT_EQ(access.value().account(), AccountId{1});
  ASSERT_TRUE(access.value().listing());
  EXPECT_EQ(access.value().listing()->title_count, 1U);
  EXPECT_TRUE(access.value().listing()->complete);
  EXPECT_EQ(access.value().listing()->origin, ListOrigin::account_service);
  EXPECT_EQ(access.value().checked_at(), rig.now());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(all_ps5_list), 0);

  rig.advance(std::chrono::minutes{14});
  EXPECT_TRUE(rig.catalog.cloud_access_of(*user));
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  rig.advance(std::chrono::minutes{1});
  const auto again = rig.catalog.cloud_access_of(*user);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().checked_at(), rig.now());
  EXPECT_EQ(rig.http.count(old_service_stores), 2);

  (void)loaded(rig, *user);
  EXPECT_EQ(rig.http.count(old_service_stores), 3);
  EXPECT_TRUE(rig.catalog.cloud_access_of(*user));
  EXPECT_EQ(rig.http.count(old_service_stores), 3);
}

TEST_F(Unreachable, PsLeavesNothingAnsweredAndKeepsOnlyTheCategories) {
  Rig down;
  const auto user = down.user();
  const auto unanswered = down.catalog.process_library_and_list(*user);
  ASSERT_FALSE(unanswered);
  EXPECT_FALSE(unanswered.error().is_cancelled());
  EXPECT_FALSE(unanswered.error().needs_sign_in());
  EXPECT_EQ(down.records.names().size(), 2U);
  EXPECT_TRUE(down.has_record("included/en-gb"));
  EXPECT_TRUE(down.has_record("for_sale/en-gb"));
  EXPECT_EQ(refusal_kind(down.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(refusal_kind(down.catalog.clear_for_launch(*user, product(rivet))),
            LaunchRefusal::Kind::check_unavailable);
}

TEST_F(Unreachable, ACallerThatLeavesIsAnsweredCancelledAndNothingIsKept) {
  Rig leaving;
  const auto walker = leaving.user();
  ps_as_usual(leaving);
  libchiaking::platform::StopSource gone;
  gone.request_stop();
  const auto cancelled =
      leaving.catalog.process_library_and_list(*walker, gone.token());
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is_cancelled());
  EXPECT_TRUE(leaving.records.names().empty());
  const auto verdict =
      leaving.catalog.clear_for_launch(*walker, product(rivet), gone.token());
  ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::check_unavailable);
  ASSERT_TRUE(verdict.refusal()->cause());
  EXPECT_TRUE(verdict.refusal()->cause()->is_cancelled());
  EXPECT_EQ(shown(*loaded(leaving, *walker)), 3U);
  const auto rechecked =
      leaving.catalog.recheck_ownership(*walker, product(comet), gone.token());
  ASSERT_FALSE(rechecked);
  EXPECT_TRUE(rechecked.error().is_cancelled());
  EXPECT_FALSE(game_of(*loaded(leaving, *walker), comet).is_owned());
}

TEST_F(Unreachable, ACloudAccessCallerThatLeavesKeepsNothing) {
  libchiaking::platform::StopSource gone;
  gone.request_stop();
  Rig absent;
  const auto visitor = absent.user();
  ps_as_usual(absent);
  const auto access = absent.catalog.cloud_access_of(*visitor, gone.token());
  ASSERT_FALSE(access);
  EXPECT_TRUE(access.error().is_cancelled());
  EXPECT_TRUE(absent.records.names().empty());
}

}  // namespace
}  // namespace catalog_api_test
