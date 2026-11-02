#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include "src/core/psn/auth/credentials.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::Gamelist;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using Reply = RestRig::Reply;

class PlayedTitles : public ::testing::Test {
 protected:
  static constexpr std::string_view token = "SYNTHETIC_GAMELIST_TOKEN";
  static constexpr std::string_view first_page =
      "https://m.np.playstation.com/api/gamelist/v2/users/me/"
      "titles?limit=50&offset=0";
  static constexpr std::string_view played_page = R"({
  "titles": [
    {
      "titleId": "PPSA07028_00",
      "name": "Sample Racer",
      "localizedName": "Sample Racer",
      "imageUrl": "https://image.example/racer.png",
      "localizedImageUrl": "https://image.example/racer-en.png",
      "category": "ps5_native_game",
      "service": "none_purchased",
      "playCount": 41,
      "concept": {"id": 10001234, "titleIds": ["PPSA07028_00", "PPSA07029_00"]},
      "firstPlayedDateTime": "2025-11-02T19:04:11.000Z",
      "lastPlayedDateTime": "2026-03-04T18:22:01.140Z",
      "playDuration": "PT47H12M5S"
    },
    {
      "titleId": "CUSA01234_00",
      "name": "Old Puzzler",
      "category": "ps4_game",
      "playCount": "3",
      "lastPlayedDateTime": "2019-07-01T09:00:00Z",
      "playDuration": "PT25M"
    },
    {
      "titleId": "PPSA07030_00",
      "name": "Shy Game",
      "category": "ps5_native_game",
      "playCount": "many",
      "playDuration": 12
    }
  ],
  "nextOffset": 3,
  "previousOffset": 0,
  "totalItemCount": 3
})";

  void answers_first_page(std::string body) {
    rig.fake.on("GET", std::string{first_page}, {Reply{200, std::move(body)}});
  }

  RestRig rig;
  Gamelist gamelist{rig.http, BearerToken{token}};
};

TEST_F(PlayedTitles, ReadPlayTimeCountAndFirstPlay) {
  answers_first_page(std::string{played_page});
  const auto page = gamelist.played_titles(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->received, 3U);
  ASSERT_EQ(page->titles.size(), 3U);
  EXPECT_EQ(page->next_offset, 3U);
  EXPECT_EQ(page->total, 3U);
  const auto& racer = page->titles[0];
  EXPECT_EQ(racer.title_id, "PPSA07028_00");
  EXPECT_EQ(racer.image_url, "https://image.example/racer-en.png");
  EXPECT_EQ(racer.play_count, 41U);
  EXPECT_EQ(racer.play_duration, "PT47H12M5S");
  EXPECT_EQ(racer.first_played, "2025-11-02T19:04:11.000Z");
  EXPECT_EQ(racer.last_played, "2026-03-04T18:22:01.140Z");
  const auto& puzzler = page->titles[1];
  EXPECT_EQ(puzzler.play_count, 3U);
  EXPECT_EQ(puzzler.play_duration, "PT25M");
  EXPECT_TRUE(puzzler.first_played.empty());
  const auto& shy = page->titles[2];
  EXPECT_FALSE(shy.play_count);
  EXPECT_TRUE(shy.play_duration.empty());
  EXPECT_TRUE(shy.first_played.empty());
  EXPECT_TRUE(shy.last_played.empty());
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"),
            "Bearer " + std::string{token});
}

TEST_F(PlayedTitles, AreMalformedWithoutTitles) {
  answers_first_page(R"({"totalItemCount":0})");
  const auto page = gamelist.played_titles(0);
  ASSERT_FALSE(page);
  EXPECT_TRUE(page.error().is<bad_reply::InvalidSchema>());
}

TEST_F(PlayedTitles, NameNoNextOffsetOnTheLastPage) {
  answers_first_page(R"({"titles":[{"titleId":"PPSA07028_00"}],)"
                     R"("previousOffset":0,"totalItemCount":"1"})");
  const auto page = gamelist.played_titles(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->received, 1U);
  EXPECT_FALSE(page->next_offset);
  EXPECT_EQ(page->total, 1U);
}

}  // namespace
}  // namespace psn_test
