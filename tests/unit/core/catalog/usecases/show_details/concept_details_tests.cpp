#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::CacheId;
using libchiaking::catalog::GameDetails;
using libchiaking::catalog::StoreLocale;
using Reason = libchiaking::catalog::ConceptDetailsReason;
using ProductReason = libchiaking::catalog::ProductDetailsReason;
using Reply = DetailsRig::Reply;
using Json = DetailsRig::Json;
using Fixtures = DetailsRig::Fixtures;

class ConceptDetails : public ::testing::Test {
 protected:
  void SetUp() override {
    rig.the_store_sells_nothing();
    rig.script_concept_store();
    user = rig.user(1, DetailsRig::gb());
    ASSERT_NO_FATAL_FAILURE(
        rig.hold_a_list_of_old_service_games(user->account_id()));
  }

  auto by_concept()
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ConceptDetailsFailure> {
    return rig.catalog.details_of(*user, grouped);
  }

  auto by_product(std::string_view id)
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ProductDetailsFailure> {
    return rig.catalog.details_of(*user, DetailsRig::product(id));
  }

  static auto without_product(GameDetails details) -> GameDetails {
    details.product = std::nullopt;
    return details;
  }

  DetailsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user;
  const libchiaking::catalog::ConceptId grouped =
      DetailsRig::concept_id("900002");
};

TEST_F(ConceptDetails, AreReadFromTheFourPublicConceptQueries) {
  EXPECT_EQ(
      rig.catalog
          .explain(*user, DetailsRig::product(DetailsRig::complete_edition))
          .product_id(),
      DetailsRig::product(DetailsRig::complete_edition));
  const auto asked = rig.http.requests();
  const auto shown = by_concept();
  ASSERT_TRUE(shown);
  EXPECT_FALSE(shown.value().product);
  EXPECT_EQ(shown.value().concept_id, grouped);
  EXPECT_EQ(shown.value().locale, DetailsRig::british());
  const auto layers = DetailsRig::layers_of(shown.value());
  ASSERT_TRUE(layers);
  EXPECT_EQ(layers->background.url(), DetailsRig::concept_background);
  EXPECT_EQ(layers->hero.url(), DetailsRig::concept_hero);
  EXPECT_EQ(layers->logo.url(), DetailsRig::concept_logo);
  EXPECT_EQ(shown.value().screenshots.size(), 7U);
  EXPECT_TRUE(shown.value().star_rating);
  EXPECT_TRUE(shown.value().age_rating);
  EXPECT_EQ(rig.http.requests(), asked + 4);
  for (const auto query : DetailsRig::concept_queries)
    EXPECT_EQ(rig.http.count(query), 1) << query;
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 0);
  const auto seen = rig.http.seen();
  for (auto at = asked; at < seen.size(); ++at) {
    EXPECT_EQ(seen[at].method, "GET") << seen[at].url;
    EXPECT_TRUE(seen[at].url.starts_with(DetailsRig::graphql)) << seen[at].url;
    EXPECT_NE(seen[at].url.find("900002"), std::string::npos) << seen[at].url;
    bool localised = false;
    for (const auto& header : seen[at].headers)
      localised = localised || (header.name == "x-psn-store-locale-override" &&
                                header.value == "en-GB");
    EXPECT_TRUE(localised) << seen[at].url;
  }
  EXPECT_TRUE(rig.has_record(DetailsRig::concept_record));
}

TEST_F(ConceptDetails, ShareTheirRecordWithAProductTheStoreDoesNotSell) {
  const auto asked = rig.http.requests();
  const auto shown = by_concept();
  ASSERT_TRUE(shown);
  const auto sold = by_product(DetailsRig::complete_edition);
  ASSERT_TRUE(sold);
  EXPECT_EQ(sold.value().product,
            DetailsRig::product(DetailsRig::complete_edition));
  EXPECT_EQ(without_product(sold.value()), shown.value());
  EXPECT_EQ(rig.http.requests(), asked + 5);
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
}

TEST_F(ConceptDetails, AreKeptAcrossARestart) {
  const auto shown = by_concept();
  ASSERT_TRUE(shown);
  ASSERT_TRUE(by_product(DetailsRig::complete_edition));
  const auto asked = rig.http.requests();
  const auto again = by_concept();
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), shown.value());
  auto restarted = rig.restarted();
  const auto after_restart = restarted.details_of(*user, grouped);
  ASSERT_TRUE(after_restart);
  EXPECT_EQ(after_restart.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked);
  const auto kept = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept.value().size(), 1U);
  EXPECT_EQ(kept.value().front().concept_id, grouped);
  EXPECT_FALSE(kept.value().front().product);
}

TEST_F(ConceptDetails, DropTheirPicturesWhenCleared) {
  const auto shown = by_concept();
  ASSERT_TRUE(shown);
  const auto layers = DetailsRig::layers_of(shown.value());
  ASSERT_TRUE(layers);
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  EXPECT_TRUE(rig.catalog.image_of(layers->logo));
  EXPECT_TRUE(rig.catalog.clear_cache(CacheId::game_details));
  const auto pictures = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(pictures);
  EXPECT_TRUE(pictures.value().empty());
  EXPECT_FALSE(rig.has_record(DetailsRig::concept_record));
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(ConceptDetails, AreTakenFromAnEditionAskedFirst) {
  const auto sold = by_product(DetailsRig::other_edition);
  const auto asked = rig.http.requests();
  const auto shown = by_concept();
  ASSERT_TRUE(sold);
  ASSERT_TRUE(shown);
  EXPECT_FALSE(shown.value().product);
  EXPECT_EQ(rig.http.requests(), asked);
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 1);
  EXPECT_EQ(without_product(sold.value()), shown.value());
}

TEST(ConceptDetailsLocale, FollowsTheRegionOfEachUser) {
  const auto grouped = DetailsRig::concept_id("900002");
  DetailsRig rig;
  rig.script_concept_store();
  const auto user = rig.user();
  const auto shown = rig.catalog.details_of(*user, grouped);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown.value().locale, StoreLocale::english_us());
  EXPECT_TRUE(rig.has_record("details/en-us/concept-900002"));
  const auto regional = rig.user(2, DetailsRig::gb());
  const auto there = rig.catalog.details_of(*regional, grouped);
  ASSERT_TRUE(there);
  EXPECT_EQ(there.value().locale, DetailsRig::british());
  EXPECT_TRUE(rig.has_record(DetailsRig::concept_record));
  EXPECT_EQ(rig.http.count(DetailsRig::concept_media_query), 2);
}

TEST(ConceptDetailsKeyArt, FallsBackToGameHubArtWithoutABackgroundOrHero) {
  const auto grouped = DetailsRig::concept_id("900002");
  for (const auto role : {"BACKGROUND_LAYER_ART", "HERO_CHARACTER"}) {
    DetailsRig rig;
    rig.script_concept_store();
    auto root = Fixtures::json(Fixtures::text("concept_media_900002.json"));
    auto& media = root["data"]["conceptRetrieve"]["media"];
    auto kept = Json::array();
    for (const auto& entry : Json::elements(media))
      if (Json::text_at(entry, "role") != role)
        Json::elements(kept).push_back(entry);
    EXPECT_EQ(Json::elements(kept).size() + 1, Json::elements(media).size())
        << role;
    media = std::move(kept);
    rig.http.on("GET", std::string{DetailsRig::concept_media_query},
                {Reply{200, Json::dump(root)}});
    const auto user = rig.user();
    const auto shown = rig.catalog.details_of(*user, grouped);
    ASSERT_TRUE(shown) << role;
    const auto cover = DetailsRig::game_hub_of(shown.value());
    EXPECT_FALSE(DetailsRig::layers_of(shown.value())) << role;
    ASSERT_TRUE(cover) << role;
    EXPECT_EQ(cover->cover.url(), DetailsRig::concept_cover) << role;
    ASSERT_TRUE(cover->logo) << role;
    EXPECT_EQ(cover->logo->url(), DetailsRig::concept_logo) << role;
    EXPECT_EQ(shown.value().screenshots.size(), 7U) << role;
    EXPECT_TRUE(shown.value().star_rating) << role;
    EXPECT_TRUE(shown.value().age_rating) << role;
  }
}

class ConceptDetailsRefusals : public ::testing::Test {
 protected:
  const libchiaking::catalog::ConceptId grouped =
      DetailsRig::concept_id("900002");
};

TEST_F(ConceptDetailsRefusals, AnswerAnUnknownConceptLikeAnUnsoldProduct) {
  DetailsRig unknown;
  unknown.script_whole_store();
  unknown.script_concept_store();
  unknown.http.on("GET", std::string{DetailsRig::concept_media_query},
                  {Reply{200, R"({"data":{"conceptRetrieve":null}})"}});
  unknown.the_store_answers(DetailsRig::not_sold(DetailsRig::remaster));
  const auto user = unknown.user();
  const auto by_concept = unknown.catalog.details_of(*user, grouped);
  const auto by_product = unknown.catalog.details_of(
      *user, DetailsRig::product(DetailsRig::remaster));
  ASSERT_FALSE(by_concept);
  ASSERT_FALSE(by_product);
  EXPECT_EQ(by_concept.error().reason(), Reason::not_listed);
  EXPECT_EQ(by_product.error().reason(), ProductReason::not_listed);
  EXPECT_EQ(by_concept.error().http_status(), by_product.error().http_status());
  EXPECT_TRUE(unknown.records.names().empty());

  unknown.http.on("GET", std::string{DetailsRig::concept_media_query},
                  {Reply{400, ""}});
  const auto refused = unknown.catalog.details_of(*user, grouped);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), Reason::rejected);
}

TEST_F(ConceptDetailsRefusals, ShowTheConceptButKeepNothingWithoutStars) {
  DetailsRig partial;
  partial.script_concept_store();
  partial.http.on("GET", std::string{DetailsRig::concept_stars_query},
                  {Reply{400, ""}});
  const auto user = partial.user();
  const auto shown = partial.catalog.details_of(*user, grouped);
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_FALSE(shown.value().star_rating);
  EXPECT_TRUE(partial.records.names().empty());
}

TEST_F(ConceptDetailsRefusals, AskNothingOnceStopped) {
  DetailsRig stopped;
  stopped.script_concept_store();
  const auto user = stopped.user();
  const auto state = std::make_shared<test_support::AtomicStop>();
  state->stopped.store(true);
  const auto never = stopped.catalog.details_of(
      *user, grouped, libchiaking::platform::StopToken{state});
  ASSERT_FALSE(never);
  EXPECT_TRUE(never.error().is_cancelled());
  EXPECT_EQ(stopped.http.count(DetailsRig::concept_media_query), 0);
}

class ConceptDetailsWithoutALibrary : public ::testing::Test {
 protected:
  const libchiaking::catalog::ConceptId grouped =
      DetailsRig::concept_id("900002");
};

TEST_F(ConceptDetailsWithoutALibrary, AreShownToAHomeUser) {
  DetailsRig rig;
  rig.script_concept_store();
  const auto home = rig.home_user();
  EXPECT_TRUE(rig.records.names().empty());
  const auto asked = rig.http.requests();
  const auto shown = rig.catalog.details_of(*home, grouped);
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_EQ(shown.value().concept_id, grouped);
  EXPECT_EQ(shown.value().locale, StoreLocale::english_us());
  EXPECT_TRUE(rig.made_only_the_four_public_requests(asked));
  EXPECT_EQ(rig.records.names().size(), 1U);
  EXPECT_TRUE(rig.has_record("details/en-us/concept-900002"));
  const auto again = rig.catalog.details_of(*home, grouped);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked + 4);
}

TEST_F(ConceptDetailsWithoutALibrary, AreShownToAUserWithoutStoreAccess) {
  DetailsRig rig;
  rig.script_concept_store();
  libchiaking::users::SavedSignIn saved{libchiaking::AccountId{5}, "player",
                                        libchiaking::psn::auth::Npsso{"NPSSO"}};
  saved.region = DetailsRig::gb();
  const auto user = rig.signed_in.as(std::move(saved));
  const auto asked = rig.http.requests();
  const auto shown = rig.catalog.details_of(*user, grouped);
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_EQ(shown.value().locale, DetailsRig::british());
  EXPECT_TRUE(rig.made_only_the_four_public_requests(asked));
  EXPECT_TRUE(rig.has_record(DetailsRig::concept_record));
  EXPECT_EQ(rig.records.names().size(), 1U);
}

TEST_F(ConceptDetailsWithoutALibrary, AreShownWhenTheLibraryIsRefused) {
  for (const auto& refusal :
       {Reply{500, "{}"}, Reply{403, ""}, Reply{200, "", {}, true}}) {
    SCOPED_TRACE(testing::Message()
                 << refusal.status
                 << (refusal.transport_error ? " unreachable" : ""));
    DetailsRig rig;
    rig.script_concept_store();
    rig.answers_the_library_token();
    rig.http.on("GET", "entitlement_type=5", {refusal});
    const auto user = rig.user(1, DetailsRig::gb());
    EXPECT_FALSE(rig.catalog.library_of(*user));
    EXPECT_FALSE(rig.catalog.process_library_and_list(*user));
    const auto asked = rig.http.requests();
    const auto shown = rig.catalog.details_of(*user, grouped);
    ASSERT_TRUE(shown);
    EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
    EXPECT_EQ(shown.value().concept_id, grouped);
    EXPECT_TRUE(rig.made_only_the_four_public_requests(asked));
  }
}

class ConceptRequests : public ::testing::Test {
 protected:
  auto over(GatedStore& gate) -> libchiaking::catalog::Catalog {
    return rig.restarted_over(gate);
  }

  DetailsRig rig;
  const libchiaking::catalog::ConceptId grouped =
      DetailsRig::concept_id("900002");
};

TEST_F(ConceptRequests, AreInFlightTogether) {
  rig.script_concept_store();
  const auto user = rig.user();
  DetailsRig sequential;
  sequential.script_concept_store();
  const auto same_user = sequential.user();
  const auto expected = sequential.catalog.details_of(*same_user, grouped);
  ASSERT_TRUE(expected);

  const auto asked = rig.http.requests();
  GatedStore gate{rig.http, 4};
  auto catalog = over(gate);
  const auto shown = catalog.details_of(*user, grouped);
  EXPECT_FALSE(gate.waited_alone());
  EXPECT_EQ(gate.arrived(), 4);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown.value(), expected.value());
  EXPECT_EQ(rig.http.requests(), asked + 4);
  for (const auto query : DetailsRig::concept_queries)
    EXPECT_EQ(rig.http.count(query), 1) << query;
  EXPECT_TRUE(rig.has_record(DetailsRig::us_concept_record));
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(ConceptRequests, KeepNothingWhenTheRatingBesideThemFails) {
  rig.script_concept_store();
  rig.http.on("GET", std::string{DetailsRig::concept_stars_query},
              {Reply{400, ""}});
  const auto user = rig.user();
  GatedStore gate{rig.http, 4};
  auto catalog = over(gate);
  const auto shown = catalog.details_of(*user, grouped);
  EXPECT_FALSE(gate.waited_alone());
  EXPECT_EQ(gate.arrived(), 4);
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_FALSE(shown.value().star_rating);
  EXPECT_TRUE(shown.value().age_rating);
  EXPECT_EQ(shown.value().publisher, "Example Publisher");
  EXPECT_FALSE(rig.has_record(DetailsRig::us_concept_record));
}

TEST_F(ConceptRequests, AreRejectedWhenTheMediaIsRefused) {
  rig.script_concept_store();
  rig.http.on("GET", std::string{DetailsRig::concept_media_query},
              {Reply{400, ""}});
  const auto user = rig.user();
  GatedStore closed{rig.http, 4};
  auto without_media = over(closed);
  const auto nothing = without_media.details_of(*user, grouped);
  EXPECT_FALSE(closed.waited_alone());
  EXPECT_EQ(closed.arrived(), 4);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason(), Reason::rejected);
  EXPECT_FALSE(rig.has_record(DetailsRig::us_concept_record));
}

TEST_F(ConceptRequests, AreAllCancelledByAStop) {
  rig.script_concept_store();
  const auto user = rig.user();
  const auto asked = rig.http.requests();
  const auto state = std::make_shared<test_support::AtomicStop>();
  GatedStore gate{rig.http, 4};
  gate.once_together = [&] { state->stopped.store(true); };
  auto catalog = over(gate);
  const auto stopped = catalog.details_of(
      *user, grouped, libchiaking::platform::StopToken{state});
  EXPECT_FALSE(gate.waited_alone());
  EXPECT_EQ(gate.arrived(), 4);
  ASSERT_FALSE(stopped);
  EXPECT_TRUE(stopped.error().is_cancelled());
  EXPECT_EQ(rig.http.requests(), asked + 4);
  EXPECT_FALSE(rig.has_record(DetailsRig::us_concept_record));

  const auto never = catalog.details_of(
      *user, grouped, libchiaking::platform::StopToken{state});
  ASSERT_FALSE(never);
  EXPECT_TRUE(never.error().is_cancelled());
  EXPECT_EQ(gate.arrived(), 4);
}

TEST(ProductDetailsStop, AsksNothingOnceStopped) {
  const auto state = std::make_shared<test_support::AtomicStop>();
  state->stopped.store(true);
  DetailsRig sold;
  sold.script_whole_store();
  const auto buyer = sold.user();
  const auto before = sold.http.requests();
  const auto unasked =
      sold.catalog.details_of(*buyer, DetailsRig::product(DetailsRig::remaster),
                              libchiaking::platform::StopToken{state});
  ASSERT_FALSE(unasked);
  EXPECT_TRUE(unasked.error().is_cancelled());
  EXPECT_EQ(sold.http.requests(), before);
  EXPECT_FALSE(sold.has_record(DetailsRig::details_record));
}

}  // namespace
}  // namespace catalog_test
