#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::GameDetails;
using libchiaking::catalog::StoreLocale;
using Reason = libchiaking::catalog::ProductDetailsReason;
using Reply = DetailsRig::Reply;
using Json = DetailsRig::Json;
using Fixtures = DetailsRig::Fixtures;

class ProductDetails : public ::testing::Test {
 protected:
  auto details_of()
      -> libchiaking::Expected<GameDetails,
                               libchiaking::catalog::ProductDetailsFailure> {
    const auto user = rig.user();
    return rig.catalog.details_of(*user,
                                  DetailsRig::product(DetailsRig::remaster));
  }

  DetailsRig rig;
};

TEST_F(ProductDetails, AreReadFromThePublicStore) {
  rig.script_whole_store();
  const auto user = rig.user();
  const auto asked = rig.http.requests();
  const auto shown = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(shown);
  const GameDetails& details = shown.value();
  EXPECT_EQ(details.product, rig.product(rig.remaster));
  EXPECT_EQ(details.locale, StoreLocale::english_us());
  EXPECT_EQ(details.fetched_at, rig.now());

  const auto layers = DetailsRig::layers_of(details);
  ASSERT_TRUE(layers);
  EXPECT_EQ(layers->background.url(), DetailsRig::concept_background);
  EXPECT_EQ(layers->hero.url(), DetailsRig::concept_hero);
  EXPECT_EQ(layers->logo.url(), DetailsRig::concept_logo);

  ASSERT_EQ(details.screenshots.size(), 7U);
  EXPECT_TRUE(details.screenshots.front().url().starts_with(
      "https://image.api.playstation.com/vulcan/ap/rnd/202409/2717/"
      "373e5f7126353772"));
  ASSERT_EQ(details.trailers.size(), 1U);
  EXPECT_EQ(details.trailers.front().url(), DetailsRig::product_trailer);

  ASSERT_TRUE(details.star_rating);
  EXPECT_GT(details.star_rating->average, 4.73);
  EXPECT_LT(details.star_rating->average, 4.75);
  EXPECT_EQ(details.star_rating->average_text, "4.74");
  EXPECT_EQ(details.star_rating->ratings, 216607);
  EXPECT_EQ(details.star_rating->percent_by_stars,
            (std::array<int, 5>{2, 1, 3, 8, 86}));

  ASSERT_TRUE(details.age_rating);
  EXPECT_EQ(details.age_rating->authority, "PEGI");
  EXPECT_EQ(details.age_rating->name, "PEGI_16");
  EXPECT_EQ(details.age_rating->description, "PEGI 16");
  ASSERT_TRUE(details.age_rating->icon);
  EXPECT_EQ(details.age_rating->icon->url(), DetailsRig::rating_icon);
  EXPECT_EQ(details.age_rating->descriptors,
            std::vector<std::string>{"Violence"});

  EXPECT_EQ(details.publisher, "Example Publisher");
  using namespace std::chrono;
  EXPECT_EQ(details.released_at,
            GameDetails::Time{sys_days{year{2024} / October / 31}});
  EXPECT_EQ(details.genres,
            (std::vector<std::string>{"Role Playing Games", "Action"}));

  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
  const auto seen = rig.http.seen();
  ASSERT_GT(seen.size(), asked);
  EXPECT_EQ(seen[asked].method, "GET");
  EXPECT_EQ(seen[asked].url,
            std::string{DetailsRig::store_products} +
                DetailsRig::details_query_of(DetailsRig::remaster, "US"));
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);
  EXPECT_TRUE(rig.sent_no_credential());
  EXPECT_TRUE(rig.has_record(DetailsRig::details_record));
}

TEST_F(ProductDetails, AreKeptForADayAcrossARestart) {
  rig.script_whole_store();
  const auto user = rig.user();
  const auto asked = rig.http.requests();
  const auto shown = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(shown);

  rig.advance(std::chrono::hours{23});
  const auto again = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked + 1);

  auto restarted = rig.restarted();
  const auto after_restart =
      restarted.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(after_restart);
  EXPECT_EQ(after_restart.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked + 1);

  rig.advance(std::chrono::hours{1});
  const auto refreshed =
      rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(refreshed);
  EXPECT_EQ(refreshed.value().fetched_at, rig.now());
  EXPECT_EQ(rig.http.requests(), asked + 2);
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(ProductDetails, AreFetchedOnceForTwoAskersAtOnce) {
  rig.script_whole_store();
  const auto user = rig.user();
  rig.http.hold(std::string{DetailsRig::details_query});
  std::optional<libchiaking::Expected<
      GameDetails, libchiaking::catalog::ProductDetailsFailure>>
      first;
  std::optional<libchiaking::Expected<
      GameDetails, libchiaking::catalog::ProductDetailsFailure>>
      second;
  auto one = DetailsRig::started(rig.threading, [&] {
    first = rig.catalog.details_of(*user, rig.product(rig.remaster));
  });
  rig.http.wait_until_held();
  auto other = DetailsRig::started(rig.threading, [&] {
    second = rig.catalog.details_of(*user, rig.product(rig.remaster));
  });
  DetailsRig::pause_for(rig.threading, std::chrono::milliseconds{50});
  rig.http.release();
  static_cast<void>(one.join());
  static_cast<void>(other.join());
  ASSERT_TRUE(first);
  ASSERT_TRUE(*first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(*second);
  EXPECT_EQ(first->value(), second->value());
  EXPECT_EQ(rig.http.count(DetailsRig::details_query), 1);
}

TEST(ProductDetailsKeyArt, FallsBackToGameHubArtWithoutABackgroundOrHero) {
  for (const auto role : {"BACKGROUND_LAYER_ART", "HERO_CHARACTER"}) {
    DetailsRig rig;
    rig.the_store_answers(DetailsRig::sold_without("concept", "images", role));
    const auto user = rig.user();
    const auto shown = rig.catalog.details_of(*user, rig.product(rig.remaster));
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

TEST_F(ProductDetails, BorrowTheProductLogoForTheLayers) {
  rig.the_store_answers(DetailsRig::sold_without("concept", "images", "LOGO"));
  const auto borrowed = details_of();
  ASSERT_TRUE(borrowed);
  const auto layers = DetailsRig::layers_of(borrowed.value());
  ASSERT_TRUE(layers);
  EXPECT_TRUE(layers->logo.url().starts_with(DetailsRig::product_logo_stem));
  EXPECT_EQ(layers->background.url(), DetailsRig::concept_background);
  EXPECT_FALSE(DetailsRig::game_hub_of(borrowed.value()));
}

TEST_F(ProductDetails, ShowAnUnbrandedCoverWithoutAnyLogo) {
  auto root =
      Fixtures::json(DetailsRig::sold_without("concept", "images", "LOGO"));
  auto& shown = root[0]["media"]["images"];
  auto kept = Json::array();
  for (const auto& entry : Json::elements(shown))
    if (Json::text_at(entry, "type") != "LOGO")
      Json::elements(kept).push_back(entry);
  shown = std::move(kept);
  rig.the_store_answers(Json::dump(root));
  const auto plain = details_of();
  ASSERT_TRUE(plain);
  const auto unbranded = DetailsRig::game_hub_of(plain.value());
  EXPECT_FALSE(DetailsRig::layers_of(plain.value()));
  ASSERT_TRUE(unbranded);
  EXPECT_EQ(unbranded->cover.url(), DetailsRig::concept_cover);
  EXPECT_FALSE(unbranded->logo);
}

TEST_F(ProductDetails, UseTheProductArtWithoutAConcept) {
  auto alone = Fixtures::json(Fixtures::text(DetailsRig::sold_fixture));
  Json::erase(alone[0], "concept");
  rig.the_store_answers(Json::dump(alone));
  const auto bare = details_of();
  ASSERT_TRUE(bare);
  const auto alone_cover = DetailsRig::game_hub_of(bare.value());
  EXPECT_FALSE(DetailsRig::layers_of(bare.value()));
  EXPECT_EQ(bare.value().screenshots.size(), 7U);
  EXPECT_EQ(bare.value().publisher, "Example Publisher");
  ASSERT_TRUE(alone_cover);
  EXPECT_EQ(alone_cover->cover.url(), DetailsRig::product_cover);
  ASSERT_TRUE(alone_cover->logo);
  EXPECT_TRUE(
      alone_cover->logo->url().starts_with(DetailsRig::product_logo_stem));
}

TEST_F(ProductDetails, TakeTheProductGameHubCoverForAMissingHero) {
  rig.the_store_answers(DetailsRig::sold_lacking(
      {{"concept", "HERO_CHARACTER"}, {"concept", "GAMEHUB_COVER_ART"}}));
  const auto shown = details_of();
  ASSERT_TRUE(shown);
  const auto cover = DetailsRig::game_hub_of(shown.value());
  ASSERT_TRUE(cover);
  EXPECT_EQ(cover->cover.url(), DetailsRig::product_cover);
  ASSERT_TRUE(cover->logo);
  EXPECT_EQ(cover->logo->url(), DetailsRig::concept_logo);
  ASSERT_TRUE(shown.value().key_art);
  EXPECT_EQ(shown.value().key_art->images(),
            (std::vector{DetailsRig::image(DetailsRig::product_cover),
                         DetailsRig::image(DetailsRig::concept_logo)}));
}

TEST_F(ProductDetails, HaveNoKeyArtWithoutLayersOrCovers) {
  rig.the_store_answers(
      DetailsRig::sold_lacking({{"concept", "BACKGROUND_LAYER_ART"},
                                {"concept", "GAMEHUB_COVER_ART"},
                                {"product", "GAMEHUB_COVER_ART"}}));
  const auto shown = details_of();
  ASSERT_TRUE(shown);
  EXPECT_FALSE(shown.value().key_art);
  EXPECT_EQ(shown.value().screenshots.size(), 7U);
  EXPECT_TRUE(rig.has_record(DetailsRig::details_record));
}

TEST_F(ProductDetails, KeepGameHubArtAcrossARestart) {
  rig.the_store_answers(
      DetailsRig::sold_lacking({{"concept", "HERO_CHARACTER"}}));
  const auto user = rig.user();
  const auto shown = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::game_hub_of(shown.value()));
  EXPECT_TRUE(rig.has_record(DetailsRig::details_record));
  auto restarted = rig.restarted();
  const auto asked = rig.http.requests();
  const auto kept = restarted.details_of(*user, rig.product(rig.remaster));
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value(), shown.value());
  EXPECT_EQ(rig.http.requests(), asked);
}

TEST_F(ProductDetails, TakeScreenshotsAndTrailersFromTheConcept) {
  auto root = Fixtures::json(
      DetailsRig::sold_without("product", "images", "SCREENSHOT"));
  root[0]["media"]["videos"] = Json::array();
  rig.the_store_answers(Json::dump(root));
  const auto shown = details_of();
  ASSERT_TRUE(shown);
  ASSERT_EQ(shown.value().screenshots.size(), 8U);
  ASSERT_EQ(shown.value().trailers.size(), 3U);
  EXPECT_EQ(shown.value().screenshots.front().url(),
            "https://image.api.playstation.com/cdn/EP0109/CUSA17003_00/"
            "FREE_c8lJEdtHFvSR6MLzt3bPtPr8N8w/1.png");
  EXPECT_EQ(shown.value().trailers.front().url(),
            "https://vulcan.dl.playstation.net/img/rnd/202011/1122/"
            "ATY9ZSMvTYifuYsuHwCnjmMM.mp4");
}

TEST_F(ProductDetails, AreShownAndKeptWithoutRatingsOrInfo) {
  auto root = Fixtures::json(Fixtures::text(DetailsRig::sold_fixture));
  for (const auto absent : {"starRating", "contentRating", "leadPublisherName",
                            "releaseDate", "localizedGenres"})
    Json::erase(root[0], absent);
  Json::erase(root[0]["concept"], "publisherName");
  rig.the_store_answers(Json::dump(root));
  const auto asked = rig.http.requests();
  const auto shown = details_of();
  ASSERT_TRUE(shown);
  EXPECT_TRUE(DetailsRig::layers_of(shown.value()));
  EXPECT_EQ(shown.value().screenshots.size(), 7U);
  EXPECT_FALSE(shown.value().star_rating);
  EXPECT_FALSE(shown.value().age_rating);
  EXPECT_TRUE(shown.value().publisher.empty());
  EXPECT_FALSE(shown.value().released_at);
  EXPECT_TRUE(shown.value().genres.empty());
  EXPECT_EQ(rig.http.requests(), asked + 1);
  EXPECT_TRUE(rig.has_record(DetailsRig::details_record));
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(ProductDetails, HaveNoStarRatingWhenNobodyRatedTheGame) {
  auto silent = Fixtures::json(Fixtures::text(DetailsRig::sold_fixture));
  silent[0]["starRating"]["count"] = Json::array();
  Json::erase(silent[0], "leadPublisherName");
  rig.the_store_answers(Json::dump(silent));
  const auto quiet = details_of();
  ASSERT_TRUE(quiet);
  EXPECT_FALSE(quiet.value().star_rating);
  EXPECT_TRUE(quiet.value().age_rating);
  EXPECT_EQ(quiet.value().publisher, "Example Publisher");
}

TEST_F(ProductDetails, AreNotListedForAGameTheStoreDoesNotShow) {
  rig.the_store_answers(DetailsRig::not_sold(DetailsRig::remaster));
  const auto user = rig.user();
  const auto missing = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason(), Reason::not_listed);
  EXPECT_FALSE(rig.has_record(DetailsRig::details_record));

  rig.the_store_answers(DetailsRig::sold_as(DetailsRig::complete_edition));
  const auto another = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(another);
  EXPECT_EQ(another.error().reason(), Reason::not_listed);
  rig.the_store_sells_nothing();
  const auto nothing = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason(), Reason::not_listed);
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);
}

TEST_F(ProductDetails, ReportWhyTheStoreGaveNoAnswer) {
  const auto user = rig.user();
  rig.http.on("GET", std::string{DetailsRig::details_query}, {Reply{400, ""}});
  const auto refused = rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), Reason::rejected);
  rig.the_store_answers("{}");
  const auto unreadable =
      rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(unreadable);
  EXPECT_EQ(unreadable.error().reason(), Reason::malformed);
  rig.http.on("GET", std::string{DetailsRig::details_query},
              {Reply{200, "", {}, true}});
  const auto unreachable =
      rig.catalog.details_of(*user, rig.product(rig.remaster));
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error().reason(), Reason::unavailable);
  EXPECT_FALSE(rig.has_record(DetailsRig::details_record));
  EXPECT_EQ(rig.http.count(DetailsRig::graphql), 0);
}

TEST_F(ProductDetails, AskWithTheAgeOfTheAccount) {
  rig.script_whole_store();
  EXPECT_TRUE(details_of());
  EXPECT_EQ(rig.http.count("&age=34&"), 1);
  EXPECT_EQ(rig.http.count("&age=99&"), 0);
}

TEST_F(ProductDetails, AreNotAskedForAnAccountOfUnknownAge) {
  rig.account_claims = R"({"legal_country":"GB","locale":"en-GB"})";
  rig.script_whole_store();
  const auto unknown = rig.user();
  const auto asked = rig.http.requests();
  const auto shown =
      rig.catalog.details_of(*unknown, rig.product(rig.remaster));
  ASSERT_FALSE(shown);
  EXPECT_EQ(shown.error().reason(), Reason::not_listed);
  EXPECT_EQ(rig.http.requests(), asked);
  EXPECT_FALSE(rig.has_record(DetailsRig::details_record));
}

TEST_F(ProductDetails, AreGatedByTheAgeOfAChild) {
  rig.account_claims =
      R"({"age":12,"legal_country":"GB","locale":"en-GB","is_child":true})";
  rig.http.on("GET", "age=12&includeConcept=true",
              {Reply{200, DetailsRig::not_sold(DetailsRig::remaster)}});
  const auto gated = details_of();
  ASSERT_FALSE(gated);
  EXPECT_EQ(gated.error().reason(), Reason::not_listed);
  EXPECT_EQ(rig.http.count("&age=12&"), 1);
  EXPECT_FALSE(rig.has_record(DetailsRig::details_record));
}

}  // namespace
}  // namespace catalog_test
