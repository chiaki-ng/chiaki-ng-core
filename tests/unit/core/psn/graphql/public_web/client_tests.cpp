#include "src/core/psn/graphql/public_web/client.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using Kind = GraphqlRig::Kind;
using Reply = ClientRig::Reply;
using test_support::psn::Fixtures;
namespace public_web = libchiaking::psn::graphql::public_web;

class PublicWebGraphql : public ::testing::Test {
 protected:
  static constexpr std::string_view grid_id =
      "4cbf39e2-5749-4970-ba81-93a489e4570c";
  static constexpr std::string_view grid_variables =
      R"({"id":"4cbf39e2-5749-4970-ba81-93a489e4570c","pageArgs":{"size":200,"offset":400},"sortBy":null,)"
      R"("filterBy":["storeDisplayClassification:FULL_GAME"],"facetOptions":[]})";
  static constexpr std::string_view frozen_grid_url =
      "https://web.np.playstation.com/api/graphql/v1/"
      "op?operationName=categoryGridRetrieve&variables="
      "%7B%22id%22%3A%224cbf39e2-5749-4970-ba81-93a489e4570c%22%2C%22pageArgs%"
      "22%"
      "3A%"
      "7B%22size%22%3A200%2C"
      "%22offset%22%3A400%7D%2C%22sortBy%22%3Anull%2C%22filterBy%22%3A%5B%"
      "22storeDisplayClassification%3AFULL_GAME"
      "%22%5D%2C%22facetOptions%22%3A%5B%5D%7D&extensions=%7B%22persistedQuery%"
      "22%3A%"
      "7B%22version%22%3A1%2C"
      "%22sha256Hash%22%3A%"
      "2288c0b9a1273c6d320c51cd73e390924e21ae28bf09f01cde8b84b1034b16cd03%22%"
      "7D%"
      "7D";

  static auto role_count(
      const std::vector<libchiaking::psn::graphql::StoreMedia>& all,
      std::string_view wanted) -> std::ptrdiff_t {
    return std::ranges::count(all, wanted,
                              &libchiaking::psn::graphql::StoreMedia::role);
  }

  void answers(const libchiaking::psn::graphql::Operation& operation,
               std::string_view variables, std::string body) {
    rig.http.on("GET", GraphqlRig::url_of(operation, variables),
                {Reply{200, std::move(body)}});
  }

  GraphqlRig rig;
  libchiaking::psn::graphql::PublicWebClient web{rig.client};
};

TEST_F(PublicWebGraphql, AsksForACategoryGridByteForByteAsTheStoreDoes) {
  rig.http.on(
      "GET", std::string{frozen_grid_url},
      {Reply{
          200,
          R"({"data":{"categoryGridRetrieve":{"pageInfo":{"totalCount":3},"products":[)"
          R"({"id":"EP1-PPSA00001_00-A","price":{"serviceBranding":["PS_PLUS"]}},)"
          R"({"id":"EP1-PPSA00002_00-B","price":{"serviceBranding":["NONE"]}},"odd"]}}})"}});
  const auto page = web.store_category_grid(grid_id, 400, 200, "en-GB");
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().total_count, 3);
  ASSERT_EQ(page.value().products.size(), 2U);
  EXPECT_EQ(page.value().products[0].id, "EP1-PPSA00001_00-A");
  EXPECT_TRUE(page.value().products[0].ps_plus);
  EXPECT_EQ(page.value().products[1].id, "EP1-PPSA00002_00-B");
  EXPECT_FALSE(page.value().products[1].ps_plus);
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& seen = rig.http.seen[0];
  EXPECT_EQ(seen.url, frozen_grid_url);
  EXPECT_NE(seen.url.find(
                libchiaking::net::http::PercentEncoding::form(grid_variables)),
            std::string::npos);
  EXPECT_EQ(seen.method, "GET");
  EXPECT_TRUE(seen.body.empty());
  EXPECT_EQ(seen.total_timeout, std::chrono::milliseconds{45'000});
  EXPECT_EQ(seen.connect_timeout, std::chrono::milliseconds{45'000});
  ASSERT_EQ(seen.headers.size(), 4U);
  EXPECT_EQ(seen.headers[0].name, "User-Agent");
  EXPECT_NE(seen.headers[0].value.find("gkApollo"), std::string::npos)
      << seen.headers[0].value;
  EXPECT_EQ(seen.headers[1].name, "x-psn-store-locale-override");
  EXPECT_EQ(seen.headers[1].value, "en-GB");
  EXPECT_EQ(seen.headers[2].name, "Content-Type");
  EXPECT_EQ(seen.headers[2].value, "application/json");
  EXPECT_EQ(seen.headers[3].name, "Accept");
  EXPECT_EQ(seen.headers[3].value, "application/json");
}

TEST_F(PublicWebGraphql, FindsACategoryGridWithoutGridDataMalformed) {
  rig.http.on("GET", std::string{frozen_grid_url},
              {Reply{200, R"({"data":{}})"}});
  const auto page = web.store_category_grid(grid_id, 400, 200, "en-GB");
  ASSERT_FALSE(page);
  EXPECT_EQ(page.error().kind(), Kind::malformed);
  EXPECT_EQ(page.error().http_status(), 200);
}

TEST_F(PublicWebGraphql, ReadsConceptDetailsWithoutACredential) {
  constexpr std::string_view asked = R"({"conceptId":"900002"})";
  rig.supply("APP-SECRET");
  answers(public_web::concept_media, asked,
          Fixtures::text("concept_media_900002.json"));
  answers(public_web::concept_star_rating, asked,
          Fixtures::text("concept_star_rating_900002.derived.json"));
  answers(public_web::concept_content_rating, asked,
          Fixtures::text("concept_content_rating_900002.derived.json"));
  answers(public_web::concept_game_info, asked,
          Fixtures::text("concept_game_info_900002.derived.json"));

  const auto media = web.concept_media("900002", "en-GB");
  ASSERT_TRUE(media);
  EXPECT_EQ(media.value().concept_id, "900002");
  EXPECT_EQ(media.value().product_id, "EP0109-PPSA17001_00-FAKE0056TITLE000");
  const auto& concept_wide = media.value().of_concept;
  const auto& edition = media.value().of_product;
  EXPECT_EQ(concept_wide.size(), 18U);
  EXPECT_EQ(edition.size(), 14U);
  EXPECT_EQ(role_count(concept_wide, "BACKGROUND_LAYER_ART"), 1);
  EXPECT_EQ(role_count(concept_wide, "HERO_CHARACTER"), 1);
  EXPECT_EQ(role_count(concept_wide, "LOGO"), 1);
  EXPECT_EQ(role_count(concept_wide, "SCREENSHOT"), 8);
  EXPECT_EQ(role_count(concept_wide, "PREVIEW"), 3);
  EXPECT_EQ(role_count(edition, "BACKGROUND_LAYER_ART"), 0);
  EXPECT_EQ(role_count(edition, "SCREENSHOT"), 7);
  EXPECT_EQ(role_count(edition, "PREVIEW"), 1);

  const auto stars = web.concept_star_rating("900002", "en-GB");
  ASSERT_TRUE(stars);
  ASSERT_TRUE(stars.value().has_value());
  EXPECT_EQ(stars.value()->average_text, "4.74");
  EXPECT_EQ(stars.value()->ratings, 216539);
  EXPECT_EQ(stars.value()->percent_by_stars,
            (std::array<int, 5>{2, 1, 3, 8, 86}));

  const auto age = web.concept_content_rating("900002", "en-GB");
  ASSERT_TRUE(age);
  ASSERT_TRUE(age.value().has_value());
  EXPECT_EQ(age.value()->authority, "PEGI");
  EXPECT_EQ(age.value()->name, "PEGI_16");
  EXPECT_EQ(age.value()->descriptors, (std::vector<std::string>{"Violence"}));

  const auto info = web.concept_info("900002", "en-GB");
  ASSERT_TRUE(info);
  EXPECT_EQ(info.value().publisher, "Example Publisher");
  EXPECT_EQ(info.value().release_date, "2017-03-01T00:00:00Z");
  EXPECT_EQ(info.value().genres,
            (std::vector<std::string>{"Role Playing Games", "Action"}));

  EXPECT_EQ(rig.http.seen.size(), 4U);
  EXPECT_TRUE(rig.http.unmatched.empty());
  for (const auto& seen : rig.http.seen) {
    SCOPED_TRACE(seen.url);
    EXPECT_TRUE(seen.url.starts_with(
        "https://web.np.playstation.com/api/graphql/v1/op?"));
    EXPECT_TRUE(seen.header("Authorization").empty());
    EXPECT_EQ(seen.header("x-psn-store-locale-override"), "en-GB");
  }
  EXPECT_EQ(rig.app_token_uses(), 0U);
}

TEST_F(PublicWebGraphql, EscapesControlCharactersInVariables) {
  constexpr std::string_view asked =
      "{\"conceptId\":\"a\\u0001\\u001F\x7f\\\"\xC3\xA9\"}";
  answers(public_web::concept_media, asked,
          R"({"data":{"conceptRetrieve":null}})");
  const auto media = web.concept_media("a\x01\x1f\x7f\"\xC3\xA9", "en-GB");
  EXPECT_EQ(GraphqlRig::kind_of(media), Kind::not_found);
  ASSERT_EQ(rig.http.seen.size(), 1U);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_NE(rig.http.seen[0].url.find(
                "&variables=%7B%22conceptId%22%3A%22a%5Cu0001%5Cu001F%7F%5C%"
                "22%C3%"
                "A9%22%7D&"),
            std::string::npos)
      << rig.http.seen[0].url;
}

TEST_F(PublicWebGraphql, NeverSendsVariablesWithBrokenText) {
  const auto media = web.concept_media("a\xFF", "en-GB");
  EXPECT_FALSE(media);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(PublicWebGraphql, ReadsWhatItCanOfAConceptTheStoreDoesNotKnow) {
  constexpr std::string_view asked = R"({"conceptId":"9"})";
  answers(public_web::concept_media, asked,
          R"({"data":{"conceptRetrieve":null}})");
  answers(public_web::concept_star_rating, asked,
          R"({"data":{"conceptRetrieve":{"id":"8","defaultProduct":null}}})");
  answers(
      public_web::concept_content_rating, asked,
      R"({"data":{"conceptRetrieve":{"id":"9","contentRating":null,"defaultProduct":null}}})");
  answers(
      public_web::concept_game_info, asked,
      R"({"data":{"conceptRetrieve":{"id":"9","defaultProduct":{"publisherName":"Studio","releaseDate":"2020-01-02T00:00:00Z","localizedGenres":[{"value":"Puzzle"}]},"publisherName":null,"releaseDate":null,"localizedGenres":[]}}})");
  EXPECT_EQ(GraphqlRig::kind_of(web.concept_media("9", "en-GB")),
            Kind::not_found);
  EXPECT_EQ(GraphqlRig::kind_of(web.concept_star_rating("9", "en-GB")),
            Kind::malformed);
  const auto age = web.concept_content_rating("9", "en-GB");
  ASSERT_TRUE(age);
  EXPECT_FALSE(age.value().has_value());
  const auto info = web.concept_info("9", "en-GB");
  ASSERT_TRUE(info);
  EXPECT_EQ(info.value().publisher, "Studio");
  EXPECT_EQ(info.value().release_date, "2020-01-02T00:00:00Z");
  EXPECT_EQ(info.value().genres, std::vector<std::string>{"Puzzle"});
}

}  // namespace
}  // namespace psn_test
