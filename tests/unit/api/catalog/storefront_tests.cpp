#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace catalog_api_test {
namespace {

class Storefront : public CatalogApiTest {
 protected:
  static void premium_member_loaded(Rig& rig, users::PsUser& user) {
    ps_as_usual(rig);
    membership_is(rig, "TIER_30");
    cloud_streaming_allowed(rig);
    (void)loaded(rig, user);
  }
};

TEST_F(Storefront, SaysAnOwnedTitleStreamsAndTheLaunchIsCleared) {
  Rig streams;
  const auto user = streams.user();
  premium_member_loaded(streams, *user);
  storefront_says(streams, rivet, streams_notice);
  const auto answer =
      streams.catalog.check_title_against_storefront(*user, product(rivet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(), StorefrontAnswer::Streaming::streams);
  EXPECT_EQ(answer.value().answered_product(), product(rivet));
  EXPECT_EQ(answer.value().checked_at(), streams.now());
  EXPECT_TRUE(
      streams.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_EQ(streams.http.count(store_app), 1);
}

TEST_F(Storefront, IsAskedForTheBaseProductOfAnEdition) {
  Rig suffixed;
  const auto buyer = suffixed.user();
  const std::string edition = "UP0112-PPSA01234_00-GAME000000000000-E001";
  const std::string base = "UP0112-PPSA01234_00-GAME000000000000";
  storefront_says(suffixed, base, streams_notice);
  const auto answer =
      suffixed.catalog.check_title_against_storefront(*buyer, product(edition));
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer.value().says_it_streams());
  EXPECT_EQ(answer.value().answered_product(), product(base));
  const auto seen = suffixed.http.seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back().url.find("GAME000000000000-E001"), std::string::npos);
}

TEST_F(Storefront, AnsweringForAnotherProductIsMalformed) {
  Rig another;
  const auto asker = another.user();
  storefront_says(another, "EP0100-PPSA01234_00-FAKE0086TITLE000",
                  streams_notice);
  const auto answer = another.catalog.check_title_against_storefront(
      *asker, product("UP0112-PPSA01234_00-GAME000000000000"));
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), catalog::StorefrontReason::malformed);
}

TEST_F(Storefront, SayingATitleDoesNotStreamNeitherRefusesNorRemembersIt) {
  Rig refusing;
  const auto owner = refusing.user();
  premium_member_loaded(refusing, *owner);
  storefront_says(refusing, rivet, no_stream_notice);
  const auto answer =
      refusing.catalog.check_title_against_storefront(*owner, product(rivet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(),
            StorefrontAnswer::Streaming::not_streamable);
  EXPECT_EQ(answer.value().notice(), "Not offered for cloud streaming");
  EXPECT_TRUE(
      refusing.catalog.clear_for_launch(*owner, product(rivet)).is_allowed());
  EXPECT_EQ(refusing.http.count(store_app), 1);
  EXPECT_FALSE(game_of(*loaded(refusing, *owner), rivet).remembered_refusal());
  EXPECT_FALSE(refusing.has_record("user/1/refusals"));
}

TEST_F(Storefront, WithoutANoticeLeavesTheLaunchCleared) {
  Rig silent;
  const auto quiet = silent.user();
  premium_member_loaded(silent, *quiet);
  storefront_says(silent, rivet, "");
  const auto answer =
      silent.catalog.check_title_against_storefront(*quiet, product(rivet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().streaming(), StorefrontAnswer::Streaming::no_notice);
  EXPECT_TRUE(
      silent.catalog.clear_for_launch(*quiet, product(rivet)).is_allowed());
  EXPECT_EQ(silent.http.count(store_app), 1);
}

TEST_F(Storefront, WhoseAccessLapsedAsksForSignInAndLeavesTheLaunchUnchecked) {
  Rig lapsed;
  const auto user = lapsed.user();
  premium_member_loaded(lapsed, *user);
  EXPECT_TRUE(user->get_my_membership());
  lapsed.http.on("GET", std::string{store_app},
                 {{401, R"({"status":"auth_refused"})"}});
  lapsed.http.before_everything(
      "GET", "com.scee.psxandroid",
      {{302,
        "",
        {{"Location",
          "com.scee.psxandroid.scecompcall://redirect?code=CODE"}}}});
  lapsed.http.on(
      "POST", "/oauth/token",
      {{200,
        R"({"access_token":"renewed","refresh_token":"next","expires_in":3600})"}});
  const auto answer =
      lapsed.catalog.check_title_against_storefront(*user, product(rivet));
  ASSERT_FALSE(answer);
  EXPECT_TRUE(answer.error().needs_sign_in());
  ASSERT_TRUE(answer.error().cause());
  EXPECT_EQ(answer.error().cause()->reason,
            users::Unanswered::Reason::store_access_lapsed);
  const auto verdict = lapsed.catalog.clear_for_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(verdict), LaunchRefusal::Kind::check_unavailable);
  ASSERT_TRUE(verdict.refusal()->cause());
  EXPECT_TRUE(verdict.refusal()->cause()->needs_sign_in());
  EXPECT_FALSE(lapsed.has_record("user/1/refusals"));
}

TEST_F(Storefront, ThatIsDownIsUnavailableAndAskedAgain) {
  Rig down;
  const auto waiting = down.user();
  down.http.on("GET", std::string{store_app}, {{500, "{}"}});
  const auto answer =
      down.catalog.check_title_against_storefront(*waiting, product(rivet));
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), catalog::StorefrontReason::unavailable);
  EXPECT_EQ(answer.error().http_status(), 500);
  const auto asked = down.http.count(store_app);
  EXPECT_FALSE(
      down.catalog.check_title_against_storefront(*waiting, product(rivet)));
  EXPECT_GT(down.http.count(store_app), asked);
}

TEST_F(Storefront, ThatAnswersGarbageIsMalformed) {
  Rig garbled;
  const auto reader = garbled.user();
  garbled.http.on("GET", std::string{store_app}, {{200, "not json"}});
  const auto answer =
      garbled.catalog.check_title_against_storefront(*reader, product(rivet));
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().reason(), catalog::StorefrontReason::malformed);
}

TEST_F(Storefront, PageIsTheProductsOrTheConcepts) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  (void)loaded(rig, *user);
  rig.http.on("HEAD", "store.playstation.com/en-gb/product/",
              {{200, "<html>"}});
  const auto page = rig.catalog.store_page_of(*user, product(rivet));
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().source(), catalog::StorePage::Source::product);
  EXPECT_EQ(page.value().url(),
            "https://store.playstation.com/en-gb/product/"
            "EP0109-PPSA07028_00-FAKE0110TITLE000");
  rig.http.on("HEAD", "store.playstation.com/en-gb/product/", {{404, ""}});
  const auto missing = rig.catalog.store_page_of(*user, product(nowhere));
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason(), catalog::StorePageReason::not_listed);
}

}  // namespace
}  // namespace catalog_api_test
