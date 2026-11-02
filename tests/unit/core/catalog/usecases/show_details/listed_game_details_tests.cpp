#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::CacheEntry;
using libchiaking::catalog::CacheId;
using libchiaking::catalog::GameDetails;
using libchiaking::catalog::ServiceGeneration;
using Reason = libchiaking::catalog::ProductDetailsReason;
using Reply = DetailsRig::Reply;

class ListedGameDetails : public ::testing::Test {
 protected:
  static constexpr std::string_view complete_record =
      "details/en-gb/ep0109-cusa17003_00-fake0059title000";

  void SetUp() override {
    rig.http.on(
        "GET", DetailsRig::details_query_of(DetailsRig::complete_edition, "GB"),
        {Reply{200, DetailsRig::sold_as(DetailsRig::complete_edition)}});
    rig.http.on("GET",
                DetailsRig::details_query_of(DetailsRig::other_edition, "GB"),
                {Reply{200, DetailsRig::sold_as(DetailsRig::other_edition)}});
    rig.script_whole_store();
    rig.script_concept_store();
    user = rig.user(1, DetailsRig::gb());
    ASSERT_NO_FATAL_FAILURE(
        rig.hold_a_list_of_old_service_games(user->account_id()));
  }

  auto details_of(std::string_view id)
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ProductDetailsFailure> {
    return rig.catalog.details_of(*user, DetailsRig::product(id));
  }

  DetailsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user;
};

TEST_F(ListedGameDetails, AreAskedByTheProductOfTheListedEdition) {
  EXPECT_EQ(
      rig.catalog
          .explain(*user, DetailsRig::product(DetailsRig::complete_edition))
          .product_id(),
      DetailsRig::product(DetailsRig::complete_edition));
  const auto asked = rig.http.requests();
  const auto shown = details_of(DetailsRig::complete_edition);
  ASSERT_TRUE(shown);
  const GameDetails& details = shown.value();
  EXPECT_EQ(details.product, DetailsRig::product(DetailsRig::complete_edition));
  EXPECT_FALSE(details.concept_id);
  EXPECT_EQ(details.locale, DetailsRig::british());
  const auto layers = DetailsRig::layers_of(details);
  ASSERT_TRUE(layers);
  EXPECT_EQ(layers->background.url(), DetailsRig::concept_background);
  EXPECT_EQ(layers->hero.url(), DetailsRig::concept_hero);
  EXPECT_EQ(layers->logo.url(), DetailsRig::concept_logo);
  EXPECT_EQ(details.screenshots.size(), 7U);
  ASSERT_EQ(details.trailers.size(), 1U);
  EXPECT_EQ(details.trailers.front().url(), DetailsRig::product_trailer);
  ASSERT_TRUE(details.star_rating);
  EXPECT_EQ(details.star_rating->average_text, "4.74");
  ASSERT_TRUE(details.age_rating);
  EXPECT_EQ(details.age_rating->name, "PEGI_16");
  EXPECT_EQ(details.publisher, "Example Publisher");
  EXPECT_EQ(rig.http.requests(), asked + 1);
  const auto seen = rig.http.seen();
  ASSERT_GT(seen.size(), asked);
  EXPECT_EQ(seen[asked].url, std::string{DetailsRig::store_products} +
                                 DetailsRig::details_query_of(
                                     DetailsRig::complete_edition, "GB"));
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);
  EXPECT_TRUE(rig.has_record(complete_record));
  EXPECT_FALSE(rig.has_record(DetailsRig::concept_record));
}

TEST_F(ListedGameDetails, KeepEditionsApart) {
  const auto asked = rig.http.requests();
  const auto shown = details_of(DetailsRig::complete_edition);
  ASSERT_TRUE(shown);
  const auto edition = details_of(DetailsRig::other_edition);
  ASSERT_TRUE(edition);
  EXPECT_EQ(edition.value().product,
            DetailsRig::product(DetailsRig::other_edition));
  EXPECT_FALSE(edition.value().concept_id);
  EXPECT_EQ(rig.http.requests(), asked + 2);
  const auto again = details_of(DetailsRig::complete_edition);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked + 2);

  const auto kept = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().size(), 2U);
  for (const auto& entry : kept.value()) {
    EXPECT_TRUE(entry.product);
    EXPECT_FALSE(entry.concept_id);
    EXPECT_EQ(entry.locale, DetailsRig::british());
    EXPECT_EQ(entry.expires_at, rig.now() + std::chrono::hours{24});
  }
}

TEST_F(ListedGameDetails, AskAnUnlistedProductByItself) {
  ASSERT_TRUE(details_of(DetailsRig::complete_edition));
  ASSERT_TRUE(details_of(DetailsRig::other_edition));
  const auto asked = rig.http.requests();
  const auto unlisted = details_of(DetailsRig::remaster);
  ASSERT_TRUE(unlisted);
  EXPECT_FALSE(unlisted.value().concept_id);
  EXPECT_TRUE(DetailsRig::layers_of(unlisted.value()));
  EXPECT_EQ(unlisted.value().product,
            DetailsRig::product(DetailsRig::remaster));
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_TRUE(
      rig.has_record("details/en-gb/ep0109-ppsa17001_00-fake0056title000"));

  const auto ungrouped = details_of(DetailsRig::ghost_ship);
  ASSERT_FALSE(ungrouped);
  EXPECT_EQ(ungrouped.error().reason(), Reason::not_listed);
  EXPECT_EQ(rig.http.requests(), asked + 2);
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);

  const auto all = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(all);
  EXPECT_EQ(all.value().size(), 3U);
  EXPECT_EQ(std::ranges::count_if(all.value(),
                                  [&](const CacheEntry& entry) {
                                    return entry.product ==
                                               DetailsRig::product(
                                                   DetailsRig::remaster) &&
                                           !entry.concept_id;
                                  }),
            1);
}

TEST_F(ListedGameDetails, DropTheirPicturesWhenCleared) {
  ASSERT_TRUE(details_of(DetailsRig::complete_edition));
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  EXPECT_TRUE(
      rig.catalog.image_of(DetailsRig::image(DetailsRig::concept_logo)));
  EXPECT_TRUE(rig.catalog.clear_cache(CacheId::game_details));
  const auto pictures = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(pictures);
  EXPECT_TRUE(pictures.value().empty());
  EXPECT_FALSE(rig.has_record(complete_record));
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST(ListedGameEditions, TheStoreDoesNotSellShareOneConceptFetch) {
  DetailsRig rig;
  rig.the_store_sells_nothing();
  rig.script_concept_store();
  const auto user = rig.user(1, DetailsRig::gb());
  ASSERT_NO_FATAL_FAILURE(
      rig.hold_a_list_of_old_service_games(user->account_id()));
  rig.http.hold(std::string{DetailsRig::concept_media_query});
  std::optional<libchiaking::Expected<
      GameDetails, libchiaking::catalog::ProductDetailsFailure>>
      first;
  std::optional<libchiaking::Expected<
      GameDetails, libchiaking::catalog::ProductDetailsFailure>>
      second;
  auto one = DetailsRig::started(rig.threading, [&] {
    first = rig.catalog.details_of(
        *user, DetailsRig::product(DetailsRig::complete_edition));
  });
  rig.http.wait_until_held();
  auto other = DetailsRig::started(rig.threading, [&] {
    second = rig.catalog.details_of(
        *user, DetailsRig::product(DetailsRig::other_edition));
  });
  rig.http.release();
  static_cast<void>(one.join());
  static_cast<void>(other.join());
  ASSERT_TRUE(first);
  ASSERT_TRUE(*first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(*second);
  EXPECT_EQ(first->value().product,
            DetailsRig::product(DetailsRig::complete_edition));
  EXPECT_EQ(second->value().product,
            DetailsRig::product(DetailsRig::other_edition));
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_info_query), 1);
}

class MatchedOldServiceGame : public ::testing::Test {
 protected:
  static constexpr std::string_view owned_ps4 =
      "EP0109-CUSA17003_00-FAKE0059TITLE000";
  static constexpr std::string_view reserved =
      "EP0109-CUSA17003_00-FAKE0107TITLE000";
  static constexpr std::string_view shared_record =
      "details/en-gb/concept-900002";

  void SetUp() override {
    rig.http.on("GET", DetailsRig::details_query_of(owned_ps4, "GB"),
                {Reply{200, DetailsRig::not_sold(owned_ps4)}});
    rig.http.on("GET", DetailsRig::details_query_of(reserved, "GB"),
                {Reply{200, DetailsRig::not_sold(reserved)}});
    rig.script_whole_store();
    rig.script_concept_store();
    ASSERT_NO_FATAL_FAILURE(rig.hold_matched_games(
        RuleCases::list_of(
            {{.product = DetailsRig::remaster,
              .name = "Skyline Zero Ember Remastered",
              .concept_id = "900002"},
             RuleCases::on_old_service(
                 owned_ps4, "Skyline Zero Ember Complete Edition", "900002")}),
        RuleCases::library_of({
            {.id = reserved,
             .product = owned_ps4,
             .name = "Skyline Zero Ember",
             .platform = "ps4",
             .package = "PS4GS"},
            {.id = DetailsRig::remaster,
             .product = owned_ps4,
             .name = "Skyline Zero Ember",
             .active = false},
        })));
    user = rig.user(RuleCases::account.value(), DetailsRig::gb());
  }

  auto details_of(std::string_view id)
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ProductDetailsFailure> {
    return rig.catalog.details_of(*user, DetailsRig::product(id));
  }

  DetailsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user;
  std::optional<libchiaking::catalog::ConceptId> grouped =
      libchiaking::catalog::ConceptId::parse("900002");
};

TEST_F(MatchedOldServiceGame, IsShownByTheConceptOfItsTitle) {
  const auto games = rig.catalog.process_library_and_list(*user);
  ASSERT_TRUE(games);
  const auto* matched = games.value()->find(DetailsRig::product(owned_ps4));
  ASSERT_NE(matched, nullptr);
  ASSERT_TRUE(matched->ownership());
  EXPECT_EQ(matched->title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(matched->ownership()->service(), ServiceGeneration::old_service);
  EXPECT_FALSE(matched->ownership()->concept_id());
  EXPECT_EQ(matched->title().concept_id(), grouped);

  const auto asked = rig.http.requests();
  const auto shown = details_of(owned_ps4);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown.value().concept_id, grouped);
  const auto layers = DetailsRig::layers_of(shown.value());
  ASSERT_TRUE(layers);
  EXPECT_EQ(layers->hero.url(), DetailsRig::concept_hero);
  EXPECT_EQ(shown.value().product, DetailsRig::product(owned_ps4));
  EXPECT_EQ(rig.http.requests(), asked + 5);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::details_query_of(owned_ps4, "GB")), 1);
  EXPECT_LT(
      rig.http.first_asking(DetailsRig::details_query_of(owned_ps4, "GB")),
      rig.http.first_asking(DetailsRig::concept_media_query));
  EXPECT_TRUE(rig.has_record(shared_record));
}

TEST_F(MatchedOldServiceGame, SharesTheConceptRecordWithItsReservation) {
  ASSERT_TRUE(rig.catalog.process_library_and_list(*user));
  ASSERT_TRUE(details_of(owned_ps4));
  const auto asked = rig.http.requests();
  const auto by_ownership = details_of(reserved);
  ASSERT_TRUE(by_ownership);
  EXPECT_EQ(by_ownership.value().concept_id, grouped);
  EXPECT_EQ(by_ownership.value().product, DetailsRig::product(reserved));
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  const auto ps5 = details_of(DetailsRig::remaster);
  ASSERT_TRUE(ps5);
  EXPECT_FALSE(ps5.value().concept_id);
  EXPECT_TRUE(DetailsRig::layers_of(ps5.value()));
  EXPECT_EQ(ps5.value().product, DetailsRig::product(DetailsRig::remaster));
  EXPECT_EQ(rig.http.requests(), asked + 2);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  const auto kept = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().size(), 2U);
  EXPECT_EQ(std::ranges::count_if(kept.value(),
                                  [&](const CacheEntry& entry) {
                                    return entry.concept_id == grouped &&
                                           !entry.product;
                                  }),
            1);
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST(UnmatchedOwnedGame, IsStillAskedByProduct) {
  constexpr std::string_view reserved = "EP0109-CUSA37002_00-FAKE0107TITLE000";
  DetailsRig rig;
  rig.script_whole_store();
  rig.script_concept_store();
  ASSERT_NO_FATAL_FAILURE(rig.hold_matched_games(
      RuleCases::list_of({{.product = DetailsRig::ghost_ps5,
                           .name = "Ghost of Isles",
                           .concept_id = "900001"}}),
      RuleCases::library_of({{.id = reserved,
                              .product = DetailsRig::ghost_ship,
                              .name = "Ghost of Isles",
                              .platform = "ps4",
                              .package = "PS4GS"}})));
  const auto user = rig.user(RuleCases::account.value(), DetailsRig::gb());
  const auto games = rig.catalog.process_library_and_list(*user);
  ASSERT_TRUE(games);
  EXPECT_EQ(games.value()->find(DetailsRig::product(DetailsRig::ghost_ship)),
            nullptr);
  EXPECT_EQ(games.value()->find(DetailsRig::product(DetailsRig::ghost_ps5)),
            nullptr);

  const auto asked = rig.http.requests();
  const auto shown = rig.catalog.details_of(
      *user, DetailsRig::product(DetailsRig::ghost_ship));
  ASSERT_FALSE(shown);
  EXPECT_EQ(shown.error().reason(), Reason::not_listed);
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);
  const auto seen = rig.http.seen();
  for (auto at = asked; at < seen.size(); ++at) {
    EXPECT_NE(seen[at].url.find("CUSA37002"), std::string::npos)
        << seen[at].url;
    EXPECT_EQ(seen[at].url.find("900001"), std::string::npos) << seen[at].url;
  }
  EXPECT_TRUE(rig.sent_no_credential());
}

class StreamedPs4Copy : public ::testing::Test {
 protected:
  void SetUp() override {
    rig.script_whole_store();
    rig.script_concept_store_as("900001");
    ASSERT_NO_FATAL_FAILURE(
        rig.ps_holds_the_library(DetailsRig::owned_ghost_row));
    rig.http.on("GET", std::string{DetailsRig::app_library},
                {Reply{200, DetailsRig::streamed_copies()}});
    user = rig.user(1, DetailsRig::gb());
  }

  auto details_of(std::string_view id)
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ProductDetailsFailure> {
    return rig.catalog.details_of(*user, DetailsRig::product(id));
  }

  DetailsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user;
  std::optional<libchiaking::catalog::ConceptId> grouped =
      libchiaking::catalog::ConceptId::parse("900001");
};

TEST_F(StreamedPs4Copy, LearnsItsConceptWhenTheLibraryLoads) {
  const auto library = rig.catalog.library_of(*user);
  ASSERT_TRUE(library);
  ASSERT_EQ(library.value()->size(), 1U);
  const auto& copy = library.value()->titles().front();
  EXPECT_EQ(copy.id().text(), DetailsRig::ghost_reserved);
  EXPECT_FALSE(copy.concept_id());
  EXPECT_EQ(copy.service(), ServiceGeneration::old_service);
  EXPECT_EQ(library.value()->copy_concepts().size(), 1U);
  EXPECT_EQ(library.value()->concept_of_copy(
                DetailsRig::product(DetailsRig::ghost_ship)),
            grouped);
  EXPECT_EQ(library.value()->concept_of_copy(
                DetailsRig::product(DetailsRig::ghost_reserved)),
            grouped);
  EXPECT_FALSE(library.value()->concept_of_copy(
      DetailsRig::product(DetailsRig::ghost_ps5)));

  EXPECT_EQ(rig.http.count(DetailsRig::owned_list), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::app_library), 1);
  for (const auto& request : rig.http.seen()) {
    if (request.url.find(DetailsRig::app_library) == std::string::npos)
      continue;
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.url, DetailsRig::app_library_url);
    ASSERT_EQ(request.headers.size(), 2U);
    EXPECT_EQ(request.headers[0].name, "Authorization");
    EXPECT_EQ(request.headers[0].value, "Bearer app");
    EXPECT_EQ(request.headers[1].name, "Accept");
  }
}

TEST_F(StreamedPs4Copy, IsShownByItsLearnedConcept) {
  ASSERT_TRUE(rig.catalog.library_of(*user));
  const auto games = rig.catalog.process_library_and_list(*user);
  ASSERT_TRUE(games);
  EXPECT_EQ(games.value()->find(DetailsRig::product(DetailsRig::ghost_ship)),
            nullptr);
  EXPECT_EQ(games.value()->find(DetailsRig::product(DetailsRig::ghost_ps5)),
            nullptr);

  const auto asked = rig.http.requests();
  const auto shown = details_of(DetailsRig::ghost_ship);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown.value().concept_id, grouped);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_EQ(shown.value().product, DetailsRig::product(DetailsRig::ghost_ship));
  EXPECT_EQ(rig.http.requests(), asked + 5);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
  const auto by_entitlement = details_of(DetailsRig::ghost_reserved);
  const auto ps5 = details_of(DetailsRig::ghost_ps5);
  ASSERT_TRUE(by_entitlement);
  EXPECT_EQ(by_entitlement.value().concept_id, grouped);
  ASSERT_TRUE(ps5);
  EXPECT_EQ(ps5.value().concept_id, grouped);
  EXPECT_EQ(ps5.value().product, DetailsRig::product(DetailsRig::ghost_ps5));
  EXPECT_EQ(rig.http.requests(), asked + 7);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  EXPECT_TRUE(rig.has_record(DetailsRig::ghost_record));
  EXPECT_EQ(rig.http.count(DetailsRig::app_library), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::owned_list), 1);
  const auto kept = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept.value().size(), 1U);
  EXPECT_EQ(kept.value().front().concept_id, grouped);
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(StreamedPs4Copy, KeepsItsConceptAcrossARestart) {
  const auto library = rig.catalog.library_of(*user);
  ASSERT_TRUE(library);
  auto restarted = rig.restarted();
  const auto reread = restarted.library_of(*user);
  ASSERT_TRUE(reread);
  EXPECT_EQ(*reread.value(), *library.value());
  EXPECT_EQ(reread.value()->concept_of_copy(
                DetailsRig::product(DetailsRig::ghost_ship)),
            grouped);
  EXPECT_EQ(rig.http.count(DetailsRig::app_library), 1);
  EXPECT_EQ(rig.http.count(DetailsRig::owned_list), 1);
}

TEST(StreamedPs4CopyConcepts, LeaveTheLibraryAsBeforeWhenTheyCannotBeRead) {
  DetailsRig learned;
  ASSERT_NO_FATAL_FAILURE(
      learned.ps_holds_the_library(DetailsRig::owned_ghost_row));
  learned.http.on("GET", std::string{DetailsRig::app_library},
                  {Reply{200, DetailsRig::streamed_copies()}});
  const auto first = learned.user(1, DetailsRig::gb());
  const auto expected = learned.catalog.library_of(*first);
  ASSERT_TRUE(expected);
  EXPECT_EQ(expected.value()->copy_concepts().size(), 1U);

  for (const auto& reply : {Reply{500, ""}, Reply{401, "{}"}, Reply{403, "{}"},
                            Reply{200, "not json"}, Reply{200, "", {}, true}}) {
    SCOPED_TRACE(testing::Message()
                 << reply.status << " " << reply.body
                 << (reply.transport_error ? " unreachable" : ""));
    DetailsRig rig;
    rig.script_whole_store();
    ASSERT_NO_FATAL_FAILURE(
        rig.ps_holds_the_library(DetailsRig::owned_ghost_row));
    rig.http.on("GET", std::string{DetailsRig::app_library}, {reply});
    const auto user = rig.user(1, DetailsRig::gb());
    const auto library = rig.catalog.library_of(*user);
    ASSERT_TRUE(library);
    EXPECT_TRUE(library.value()->copy_concepts().empty());
    EXPECT_TRUE(std::ranges::equal(library.value()->titles(),
                                   expected.value()->titles()));
    EXPECT_TRUE(std::ranges::equal(library.value()->bundles(),
                                   expected.value()->bundles()));
    EXPECT_EQ(library.value()->fetched_at(), expected.value()->fetched_at());
    EXPECT_EQ(rig.http.count(DetailsRig::app_library), 1);
    EXPECT_EQ(rig.http.count(DetailsRig::owned_list), 1);
    EXPECT_EQ(user->sign_in_state(),
              libchiaking::users::SignInState::signed_in);
    EXPECT_TRUE(rig.has_record("user/1/library"));
    const auto again = rig.catalog.library_of(*user);
    EXPECT_TRUE(again);
    EXPECT_EQ(rig.http.count(DetailsRig::app_library), 1);

    const auto shown = rig.catalog.details_of(
        *user, DetailsRig::product(DetailsRig::ghost_ship));
    EXPECT_FALSE(shown);
    EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
    EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 0);
  }
}

TEST(StreamedPs4CopyConcepts, AreNotAskedForALibraryWithoutPs4Copies) {
  DetailsRig rig;
  ASSERT_NO_FATAL_FAILURE(
      rig.ps_holds_the_library("", DetailsRig::owned_ps5_row));
  const auto user = rig.user(1, DetailsRig::gb());
  const auto library = rig.catalog.library_of(*user);
  ASSERT_TRUE(library);
  ASSERT_EQ(library.value()->size(), 1U);
  EXPECT_EQ(rig.http.count(DetailsRig::app_library), 0);
  EXPECT_EQ(rig.http.count(DetailsRig::ps5_library), 1);
  EXPECT_EQ(library.value()->titles().front().service(),
            ServiceGeneration::ps5_cloud);
}

}  // namespace
}  // namespace catalog_test
