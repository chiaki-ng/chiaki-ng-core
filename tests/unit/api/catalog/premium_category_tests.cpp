#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace catalog_api_test {
namespace {

using PremiumCategory = CatalogApiTest;

TEST_F(PremiumCategory, IsAskedForByAgeAndKeptApartForAnUnderAgeAccount) {
  Rig rig;
  rig.account_claims =
      R"({"age":12,"legal_country":"GB","locale":"en-GB","is_child":true})";
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed}), store_list({comet_listed}));
  library_holds(rig, owned_page({}));
  rig.http.on("GET", std::string{tag_grid},
              {{200, tagged_grid(85, "PPSA10001")}});
  (void)loaded(rig, *user);
  EXPECT_EQ(user->account_context().age(), 12);
  EXPECT_EQ(user->account_context().is_child(), true);
  EXPECT_EQ(rig.http.count("?country=GB&language=en&age=12&"), 2);
  (void)loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), 1);

  rig.account_claims = R"({"age":34,"legal_country":"GB","locale":"en-GB"})";
  const auto adult = rig.user(2);
  (void)loaded(rig, *adult);
  EXPECT_EQ(rig.http.count("?country=GB&language=en&age=34&"), 2);
  EXPECT_EQ(rig.http.count(tag_grid), 2);
}

TEST_F(PremiumCategory, IsNotAskedForAsAnAdultWhenTheAgeIsUnknown) {
  Rig rig;
  rig.account_claims = R"({"legal_country":"GB","locale":"en-GB"})";
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.on("GET", std::string{tag_grid},
              {{200, tagged_grid(85, "PPSA10001")}});
  const auto games = loaded(rig, *user);
  EXPECT_FALSE(user->account_context().age());
  EXPECT_EQ(rig.http.count(tag_grid), 0);
  EXPECT_FALSE(games->inputs().included_fetched_at);
}

TEST_F(PremiumCategory, DecidesByStoreTagsWhatTheSubscriptionIncludes) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed, listed_only_listed}),
                  store_list({listed_only_listed}));
  library_holds(rig, owned_page({}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  const auto games = loaded(rig, *user);
  EXPECT_EQ(rig.http.count("m.np.playstation.com/api/catalog/v2/categories/"),
            2);
  EXPECT_EQ(rig.http.count(tag_grid), 1);
  EXPECT_EQ(rig.http.count(store_grid), 1);
  for (const auto& request : rig.http.seen()) {
    if (request.url.find(tag_grid) == std::string::npos) continue;
    EXPECT_NE(request.url.find("a23e4f41-6e78-4dab-95e2-af1c10e4ab96?"
                               "country=GB&language=en&age=34"),
              std::string::npos)
        << request.url;
    for (const auto& header : request.headers) {
      EXPECT_NE(header.name, "Authorization");
      EXPECT_NE(header.name, "Cookie");
    }
  }
  const auto report = rig.catalog.how_fetched(*user);
  ASSERT_TRUE(report);
  ASSERT_TRUE(report->cloud_stream.premium_category);
  EXPECT_EQ(report->cloud_stream.premium_category->titles, 86U);
  EXPECT_EQ(report->cloud_stream.premium_category->fetched_at, rig.now());
  ASSERT_TRUE(report->legacy_cloud_stream.copies_checked);
  EXPECT_EQ(report->legacy_cloud_stream.copies_checked->answered, 0U);
  EXPECT_EQ(game_of(*games, comet).play_path(), PlayPath::included);
  EXPECT_EQ(game_of(*games, comet).inclusion().source(),
            catalog::PlusInclusion::Source::store_tags);
  EXPECT_EQ(game_of(*games, comet).inclusion().lowest_tier(), Tier::premium);
  EXPECT_EQ(games->find(product(listed_only)), nullptr);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(listed_only))),
            LaunchRefusal::Kind::not_listed);
  EXPECT_TRUE(rig.has_record("included/en-gb"));

  rig.advance(std::chrono::hours{2});
  const auto asked = rig.http.count(tag_grid);
  const auto again = loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), asked);
  EXPECT_EQ(game_of(*again, comet).play_path(), PlayPath::included);
}

TEST_F(PremiumCategory, ThatFailsListsNoPremiumGameAndKeepsTheRest) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed, listed_only_listed}),
                  store_list({listed_only_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  rig.http.on("GET", std::string{tag_grid}, {{500, ""}});
  const auto games = loaded(rig, *user);
  EXPECT_GE(rig.http.count(tag_grid), 1);
  EXPECT_FALSE(games->inputs().included_fetched_at);
  EXPECT_EQ(games->find(product(listed_only)), nullptr);
  EXPECT_EQ(games->find(product(comet)), nullptr);
  EXPECT_TRUE(game_of(*games, rivet).is_owned());
  EXPECT_TRUE(game_of(*games, classic).is_visible());

  const auto failed = rig.http.count(tag_grid);
  rig.advance(std::chrono::minutes{10});
  rig.catalog.start_over(*user);
  (void)loaded(rig, *user);
  EXPECT_EQ(rig.http.count(tag_grid), failed);
  rig.advance(std::chrono::minutes{21});
  (void)loaded(rig, *user);
  EXPECT_GT(rig.http.count(tag_grid), failed);
}

}  // namespace
}  // namespace catalog_api_test
