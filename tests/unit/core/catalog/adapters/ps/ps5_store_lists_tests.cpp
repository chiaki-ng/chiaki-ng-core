#include "src/core/catalog/adapters/ps/ps5_store_lists.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_walk_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

class Ps5StoreListsReply : public ::testing::Test {
 protected:
  static constexpr std::string_view listed =
      R"([{"games":[{"productId":"EP-PPSA10001_00-GAME","name":"Fable® ™ \"II\"",)"
      R"("device":["PS5"],"streamingSupported":true,"imageUrl":"https:\/\/a.example\/x",)"
      R"("conceptId":18446744073709551615,"conceptUrl":"https:\/\/store.playstation.com\/en-gb\/concept\/1"}]}])";

  PsRig rig;
};

TEST_F(Ps5StoreListsReply, ReadsAnEntryWithTheSharedReader) {
  rig.http.on("GET", "categoryList=all-ps5-list", {{200, std::string{listed}}});
  rig.http.on("GET", "imagic/gameslist", {{200, "[]"}});
  const auto fetched =
      libchiaking::catalog::ps::Ps5StoreLists{rig.client, rig.wall}.fetch(
          PsRig::locale("en-GB"));
  ASSERT_TRUE(fetched);
  ASSERT_EQ(fetched.value().entries().size(), 1U);
  const auto& entry = fetched.value().entries().front();
  EXPECT_EQ(entry.name, "Fable® ™ \"II\"");
  EXPECT_EQ(entry.cover_url, "https://a.example/x");
  ASSERT_TRUE(entry.concept_id);
  EXPECT_EQ(entry.concept_id->text(), "9223372036854775807");
  ASSERT_TRUE(entry.concept_page);
  EXPECT_EQ(entry.concept_page->url(),
            "https://store.playstation.com/en-gb/concept/1");
}

TEST_F(Ps5StoreListsReply, TruncatesARealConceptNumber) {
  const std::string reals =
      R"([{"games":[)"
      R"({"productId":"EP-PPSA10001_00-GAME","name":"a","device":["PS5"],"conceptId":1.5},)"
      R"({"productId":"EP-PPSA10002_00-GAME","name":"b","device":["PS5"],"conceptId":2e3}]}])";
  rig.http.on("GET", "categoryList=all-ps5-list", {{200, reals}});
  rig.http.on("GET", "imagic/gameslist", {{200, "[]"}});
  const auto fetched =
      libchiaking::catalog::ps::Ps5StoreLists{rig.client, rig.wall}.fetch(
          PsRig::locale("en-GB"));
  ASSERT_TRUE(fetched);
  std::vector<std::string> concepts;
  for (const auto& entry : fetched.value().entries())
    concepts.push_back(entry.concept_id ? entry.concept_id->text() : "-");
  EXPECT_EQ(concepts, (std::vector<std::string>{"1", "2000"}));
}

TEST_F(Ps5StoreListsReply, RefusesAReplyOverItsSizeLimit) {
  rig.http.on("GET", "imagic/gameslist", {{200, std::string{listed}}});
  const auto refused =
      libchiaking::catalog::ps::Ps5StoreLists{rig.client, rig.wall, 16}.fetch(
          PsRig::locale("en-GB"));
  EXPECT_FALSE(refused);
  EXPECT_EQ(rig.http.count("imagic"), 6 * 2);
}

TEST_F(Ps5StoreListsReply, CallsAMalformedReplyUnavailable) {
  rig.http.on("GET", "imagic/gameslist",
              {{200, R"({"games":[]})"}, {200, "[1]x"}});
  const auto malformed =
      libchiaking::catalog::ps::Ps5StoreLists{rig.client, rig.wall}.fetch(
          PsRig::locale("en-GB"));
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().reason(),
            libchiaking::catalog::Unanswered::Reason::unavailable);
}

class Ps5StoreListsFetch : public ::testing::Test, public StoreWalkRig {
 protected:
  static auto asked_without_credentials(
      const std::vector<FragmentHttp::Seen>& requests)
      -> ::testing::AssertionResult {
    for (const auto& request : requests)
      for (const auto& header : request.headers)
        if (header.name == "Authorization" || header.name == "Cookie")
          return ::testing::AssertionFailure()
                 << request.url << " carries " << header.name;
    return ::testing::AssertionSuccess();
  }

  auto lists_three_titles() -> Walked {
    http.on("GET", "categoryList=all-ps5-list",
            {{200, store_list(std::string{ps5_game} + R"(,
        {"productId":"EP-PPSA10002_00-NOSTREAM","name":"Offline","conceptId":"90000014","device":["PS5","PS4"],
         "streamingSupported":false,"imageUrl":"https://image.example/own.png",
         "images":[{"type":10,"url":"https://image.example/ten.png"}],
         "landscapeImageUrl":"https://image.example/wide.png",
         "conceptUrl":"https://store.playstation.com/en-gb/concept/90000014"},
        {"productId":"EP-PPSA10003_00-UNSAID","name":"Unsaid","device":["PS3"],
         "images":[{"type":13,"url":"https://image.example/thirteen.png"},{"type":10,"url":"https://image.example/ten.png"}]},
        {"name":"No product"}, 7)")}});
    http.on("GET", "categoryList=plus-games-list",
            {{200, store_list(ps5_game)}});
    http.on("GET", "imagic/gameslist", {{200, "[]"}});
    return ps5_lists("en-GB");
  }
};

TEST_F(Ps5StoreListsFetch, KeepsWhatPsSaysOfEachTitle) {
  using libchiaking::catalog::ListOrigin;
  using libchiaking::catalog::Shelf;
  const auto fetched = lists_three_titles();
  ASSERT_TRUE(fetched);
  const auto& listing = fetched.value();
  EXPECT_TRUE(listing.source().complete);
  EXPECT_EQ(listing.source().origin, ListOrigin::public_store);
  EXPECT_EQ(listing.source().region.text(), "en-GB");
  EXPECT_EQ(listing.source().settled_locale.text(), "en-GB");
  EXPECT_EQ(listing.source().fetched_at, now());
  ASSERT_EQ(listing.entries().size(), 4U);
  const auto& comet = listing.entries()[0];
  EXPECT_EQ(comet.product, "EP-PPSA10001_00-GAME");
  EXPECT_EQ(comet.name, "Comet");
  EXPECT_EQ(comet.shelf, Shelf::all_ps5);
  ASSERT_TRUE(comet.concept_id);
  EXPECT_EQ(comet.concept_id->text(), "1");
  EXPECT_TRUE(comet.on_ps5);
  EXPECT_FALSE(comet.on_ps4);
  EXPECT_EQ(comet.streaming_supported, true);
  EXPECT_TRUE(comet.cover_url.empty());
  EXPECT_TRUE(comet.landscape_cover_url.empty());
  EXPECT_FALSE(comet.concept_page);
  const auto& offline = listing.entries()[1];
  ASSERT_TRUE(offline.concept_id);
  EXPECT_EQ(offline.concept_id->text(), "90000014");
  EXPECT_TRUE(offline.on_ps5);
  EXPECT_TRUE(offline.on_ps4);
  EXPECT_EQ(offline.streaming_supported, false);
  EXPECT_EQ(offline.cover_url, "https://image.example/own.png");
  EXPECT_EQ(offline.landscape_cover_url, "https://image.example/wide.png");
  ASSERT_TRUE(offline.concept_page);
  EXPECT_EQ(offline.concept_page->url(),
            "https://store.playstation.com/en-gb/concept/90000014");
  const auto& unsaid = listing.entries()[2];
  EXPECT_FALSE(unsaid.on_ps5);
  EXPECT_FALSE(unsaid.on_ps4);
  EXPECT_FALSE(unsaid.streaming_supported);
  EXPECT_FALSE(unsaid.concept_id);
  EXPECT_EQ(unsaid.cover_url, "https://image.example/ten.png");
  EXPECT_EQ(listing.entries()[3].product, "EP-PPSA10001_00-GAME");
  EXPECT_EQ(listing.entries()[3].shelf, Shelf::plus_games);
}

TEST_F(Ps5StoreListsFetch, AsksEveryPublicListWithoutCredentials) {
  static_cast<void>(lists_three_titles());
  ASSERT_EQ(http.seen.size(), 6U);
  const std::string asked =
      "https://www.playstation.com/bin/imagic/"
      "gameslist?locale=en-gb&categoryList=";
  EXPECT_EQ(http.seen[0].url, asked + "all-ps5-list");
  EXPECT_EQ(http.seen[1].url, asked + "plus-games-list");
  EXPECT_EQ(http.seen[2].url, asked + "ubisoft-classics-list");
  EXPECT_EQ(http.seen[3].url, asked + "plus-classics-list");
  EXPECT_EQ(http.seen[4].url, asked + "plus-monthly-games-list");
  EXPECT_EQ(http.seen[5].url, asked + "free-to-play-list");
  EXPECT_TRUE(asked_without_credentials(http.seen));
}

TEST_F(Ps5StoreListsFetch, LeavesTheListingIncompleteAfterAFailedAllPs5List) {
  http.on("GET", "categoryList=all-ps5-list", {{500, store_list(ps5_game)}});
  http.on("GET", "categoryList=plus-games-list", {{200, store_list(ps5_game)}});
  http.on("GET", "imagic/gameslist", {{200, "[]"}});
  const auto fetched = ps5_lists("en-GB");
  ASSERT_TRUE(fetched);
  EXPECT_FALSE(fetched.value().source().complete);
  ASSERT_EQ(fetched.value().entries().size(), 1U);
  EXPECT_EQ(fetched.value().entries()[0].shelf,
            libchiaking::catalog::Shelf::plus_games);
  EXPECT_EQ(http.count("imagic/gameslist?locale=en-gb&"), 6);
}

TEST_F(Ps5StoreListsFetch, SettlesDownTheLocaleChain) {
  http.on("GET", "locale=de-de&", {{404, ""}});
  http.on("GET", "locale=en-de&categoryList=free-to-play-list",
          {{200, store_list(ps5_game)}});
  http.on("GET", "locale=en-de&", {{500, ""}});
  const auto fetched = ps5_lists("de-DE");
  ASSERT_TRUE(fetched);
  EXPECT_EQ(fetched.value().source().settled_locale.text(), "en-DE");
  EXPECT_EQ(fetched.value().source().region.text(), "de-DE");
  EXPECT_FALSE(fetched.value().source().complete);
  ASSERT_EQ(fetched.value().entries().size(), 1U);
  EXPECT_EQ(fetched.value().entries()[0].shelf,
            libchiaking::catalog::Shelf::free_to_play);
  EXPECT_EQ(http.count("locale=de-de&"), 6);
  EXPECT_EQ(http.count("locale=en-de&"), 6);
  EXPECT_EQ(http.count("locale=en-us&"), 0);
  ASSERT_FALSE(http.seen.empty());
  EXPECT_NE(
      http.seen.front().url.find("locale=de-de&categoryList=all-ps5-list"),
      std::string::npos);
}

}  // namespace
}  // namespace catalog_test
