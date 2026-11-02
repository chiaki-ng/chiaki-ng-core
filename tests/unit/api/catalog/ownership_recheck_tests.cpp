#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

class OwnershipRecheck : public CatalogApiTest {
 protected:
  static auto comet_named(std::string_view name) -> std::string {
    return R"({"id":"EP-PPSA10001_00-GAME","productId":"EP-PPSA10001_00-GAME","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"name":")" +
           std::string{name} +
           R"(","packageType":"PSGD"},"streamingSupported":true})";
  }

  static void comet_lookup_answers(Rig& rig, std::string record) {
    rig.http.before_everything("GET",
                               "internal/entitlements/EP-PPSA10001_00-GAME",
                               {{200, std::move(record)}});
  }

  void library_fails_with(Reply failure) {
    Rig failing;
    const auto unlucky = failing.user();
    ps_as_usual(failing);
    (void)loaded(failing, *unlucky);
    failing.http.on("GET", std::string{owned_lookup}, {{404, "{}"}});
    failing.http.on("GET", std::string{owned_list}, {std::move(failure)});
    const auto unknown =
        failing.catalog.recheck_ownership(*unlucky, product(comet));
    ASSERT_FALSE(unknown);
    EXPECT_FALSE(unknown.error().needs_sign_in());
    EXPECT_FALSE(unknown.error().is_cancelled());
    EXPECT_TRUE(unlucky->is_signed_in());
    EXPECT_FALSE(unlucky->is_expired());
    EXPECT_FALSE(game_of(*loaded(failing, *unlucky), comet).is_owned());
    EXPECT_TRUE(game_of(*loaded(failing, *unlucky), rivet).is_owned());
  }
};

TEST_F(OwnershipRecheck, AsksPsForOneTitleAndKeepsWhatItFinds) {
  Rig owned;
  const auto user = owned.user();
  ps_as_usual(owned);
  cloud_streaming_allowed(owned);
  (void)loaded(owned, *user);
  owned.advance(std::chrono::minutes{1});
  comet_lookup_answers(owned, comet_owned);
  const auto asked = owned.http.requests();
  const auto answer = owned.catalog.recheck_ownership(*user, product(comet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().kind(), OwnershipAnswer::Kind::owned);
  ASSERT_TRUE(answer.value().game());
  const auto& game = *answer.value().game();
  EXPECT_TRUE(game.is_owned());
  EXPECT_EQ(game.product_id(), product(comet));
  ASSERT_TRUE(game.ownership());
  EXPECT_EQ(game.ownership()->id().text(), comet);
  EXPECT_EQ(game.play_path(), PlayPath::owned);
  EXPECT_TRUE(game.inclusion().is_included());
  EXPECT_TRUE(game.is_visible());
  EXPECT_EQ(owned.http.requests(), asked + 1);
  EXPECT_EQ(owned.http.count(owned_list), 1);
  EXPECT_EQ(owned.http.count("users/me/internal/entitlements/"
                             "EP-PPSA10001_00-GAME?fields=gameMeta"),
            1);
  EXPECT_EQ(owned.http.count(owned_lookup), 0);
  EXPECT_EQ(owned.http.count(all_ps5_list), 1);
  EXPECT_EQ(owned.http.count(old_service_stores), 1);
  EXPECT_EQ(owned.http.count(store_app), 0);
  EXPECT_EQ(owned.http.count("response_type=token"), 1);
  const auto library = owned.catalog.library_of(*user);
  ASSERT_TRUE(library);
  EXPECT_EQ(library.value()->size(), 2U);
  EXPECT_EQ(library.value()->fetched_at(), owned.now());
  EXPECT_EQ(owned.http.requests(), asked + 1);
  auto restarted_catalog = restarted(owned);
  EXPECT_TRUE(
      game_of(*restarted_catalog.process_library_and_list(*user).value(), comet)
          .is_owned());
}

TEST_F(OwnershipRecheck, AnswersNotOwnedAfterReadingTheWholeLibrary) {
  Rig missing;
  const auto other = missing.user();
  ps_as_usual(missing);
  (void)loaded(missing, *other);
  const auto not_owned =
      missing.catalog.recheck_ownership(*other, product(comet));
  ASSERT_TRUE(not_owned);
  EXPECT_EQ(not_owned.value().kind(), OwnershipAnswer::Kind::not_owned);
  EXPECT_FALSE(not_owned.value().game());
  EXPECT_EQ(missing.http.count(ps5_owned_lookup), 1);
  EXPECT_EQ(missing.http.count(owned_lookup), 0);
  EXPECT_EQ(missing.http.count(owned_list), 2);
  EXPECT_EQ(missing.http.count(ps5_owned_list), 2);
}

TEST_F(OwnershipRecheck, AnswersThatAnAddOnIsNotAGame) {
  Rig extra;
  const auto buyer = extra.user();
  ps_as_usual(extra);
  (void)loaded(extra, *buyer);
  comet_lookup_answers(extra, comet_add_on);
  const auto add_on = extra.catalog.recheck_ownership(*buyer, product(comet));
  ASSERT_TRUE(add_on);
  EXPECT_EQ(add_on.value().kind(), OwnershipAnswer::Kind::not_a_game);
  EXPECT_EQ(add_on.value().why_not_a_game(), catalog::NotAGame::other_feature);
  EXPECT_FALSE(game_of(*loaded(extra, *buyer), comet).is_owned());
}

TEST_F(OwnershipRecheck, AsksForSignInWhenTheLibraryIsRefused) {
  Rig refusing;
  const auto barred = refusing.user();
  ps_as_usual(refusing);
  (void)loaded(refusing, *barred);
  refusing.http.on("GET", std::string{owned_lookup}, {{404, "{}"}});
  refusing.http.on("GET", std::string{owned_list}, {{401, ""}});
  const auto refused =
      refusing.catalog.recheck_ownership(*barred, product(comet));
  ASSERT_FALSE(refused);
  EXPECT_TRUE(refused.error().needs_sign_in());
}

TEST_F(OwnershipRecheck, StaysUnansweredWhenTheLibraryFailsOnTheServer) {
  library_fails_with(Reply{500, "{}"});
}

TEST_F(OwnershipRecheck, StaysUnansweredWhenTheLibraryCannotBeReached) {
  library_fails_with(Reply{0, "", {}, true});
}

TEST_F(OwnershipRecheck, CountsADemoNamedTitleAsOwned) {
  Rig sampler;
  const auto trying = sampler.user();
  ps_as_usual(sampler);
  (void)loaded(sampler, *trying);
  comet_lookup_answers(sampler, comet_named("Comet Demo"));
  const auto demo = sampler.catalog.recheck_ownership(*trying, product(comet));
  ASSERT_TRUE(demo);
  EXPECT_EQ(demo.value().kind(), OwnershipAnswer::Kind::owned);
  EXPECT_TRUE(game_of(*loaded(sampler, *trying), comet).is_owned());
}

TEST_F(OwnershipRecheck, CountsAFullGameWhoseNameStartsWithDemoAsOwned) {
  Rig racer;
  const auto driver = racer.user();
  ps_as_usual(racer);
  (void)loaded(racer, *driver);
  comet_lookup_answers(racer, comet_named("Demolition Racer"));
  const auto full = racer.catalog.recheck_ownership(*driver, product(comet));
  ASSERT_TRUE(full);
  EXPECT_EQ(full.value().kind(), OwnershipAnswer::Kind::owned);
  ASSERT_TRUE(full.value().game());
  EXPECT_TRUE(full.value().game()->is_owned());
}

TEST_F(OwnershipRecheck, RefusesATitleThatIsNotListed) {
  Rig unlisted;
  const auto asker = unlisted.user();
  ps_as_usual(unlisted);
  const auto unknown =
      unlisted.catalog.recheck_ownership(*asker, product(nowhere));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().reason(),
            catalog::OwnershipRecheckReason::not_listed);
  EXPECT_EQ(unlisted.http.count(owned_lookup), 0);
}

TEST_F(OwnershipRecheck, FindsAClaimUnderAnotherIdInTheWholeLibrary) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({bus_listed, rivet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  premium_includes(rig,
                   {premium_offer(bus_listing, "Coach Simulator 21 Next Stop",
                                  bus_claim_id)});
  const auto before = loaded(rig, *user);
  EXPECT_FALSE(game_of(*before, bus_listing).is_owned());

  rig.advance(std::chrono::minutes{1});
  library_pages(rig, {{200, owned_page({rivet_owned, other_bus, bus_claim})}});
  const auto answer =
      rig.catalog.recheck_ownership(*user, product(bus_listing));
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer.value().is_owned());
  ASSERT_TRUE(answer.value().game());
  const auto& game = *answer.value().game();
  EXPECT_EQ(game.product_id(), product(bus_listing));
  ASSERT_TRUE(game.ownership());
  EXPECT_EQ(game.ownership()->id().text(), bus_claim_id);
  EXPECT_TRUE(game.is_visible());
  EXPECT_TRUE(game.title().is_known_as(product(bus_claim_id)));
  EXPECT_EQ(rig.http.count(ps5_owned_lookup), 1);
  EXPECT_EQ(rig.http.count(owned_lookup), 0);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(rig.http.count(ps5_owned_list), 2);
  const auto after = loaded(rig, *user);
  EXPECT_EQ(game_of(*after, bus_listing), game);
  EXPECT_EQ(game_of(*after, bus_claim_id), game);
  EXPECT_EQ(rig.http.count(owned_list), 2);
}

TEST_F(OwnershipRecheck, AnswersNotOwnedWhenNoClaimMatchesTheListing) {
  Rig unmatched;
  const auto other = unmatched.user();
  old_service_lists(unmatched);
  ps5_store_lists(unmatched, store_list({bus_listed, rivet_listed}));
  library_holds(unmatched, owned_page({rivet_owned}));
  premium_includes(unmatched,
                   {premium_offer(bus_listing, "Coach Simulator 21 Next Stop",
                                  bus_claim_id)});
  (void)loaded(unmatched, *other);
  library_pages(unmatched, {{200, owned_page({rivet_owned, other_bus})}});
  const auto miss =
      unmatched.catalog.recheck_ownership(*other, product(bus_listing));
  ASSERT_TRUE(miss);
  EXPECT_EQ(miss.value().kind(), OwnershipAnswer::Kind::not_owned);
  EXPECT_FALSE(game_of(*loaded(unmatched, *other), bus_listing).is_owned());
}

}  // namespace
}  // namespace catalog_api_test
