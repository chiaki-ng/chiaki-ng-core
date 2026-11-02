#include "tests/unit/core/psn/rest/trophy/trophies_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::Trophies;

class RestTrophyTitleSets : public TrophiesFixture {
 protected:
  static constexpr std::string_view reply = R"({
  "titles": [
    {
      "npTitleId": "PPSA07028_00",
      "trophyTitles": [
        {
          "npServiceName": "trophy2",
          "npCommunicationId": "NPWR20001_00",
          "trophySetVersion": "01.10",
          "trophyTitleName": "Sample Racer",
          "trophyTitleIconUrl": "https://image.example/trophy/racer.png",
          "trophyTitlePlatform": "PS5",
          "hasTrophyGroups": true,
          "trophyGroupCount": 2,
          "definedTrophies": {"bronze": 40, "silver": 10, "gold": 3, "platinum": 1},
          "progress": 37,
          "earnedTrophies": {"bronze": 18, "silver": 2, "gold": 0, "platinum": 0},
          "hiddenFlag": false,
          "lastUpdatedDateTime": "2026-03-04T18:22:01Z"
        }
      ]
    },
    {"npTitleId": "CUSA01234_00", "trophyTitles": []},
    {"npTitleId": "CUSA05555_00"},
    "not a title",
    {
      "npTitleId": "CUSA07777_00",
      "trophyTitles": [
        {"npServiceName": "trophy", "npCommunicationId": "NPWR07777_00",
         "trophyTitleName": "Twin Release", "trophyTitlePlatform": "PS4"},
        {"npServiceName": "trophy", "npCommunicationId": "NPWR07778_00",
         "trophyTitleName": "Twin Release", "trophyTitlePlatform": "PS3"}
      ]
    }
  ]
})";
};

TEST_F(RestTrophyTitleSets, NameEveryIdInOneRequest) {
  rig.fake.on("GET",
              url("/users/me/titles/trophyTitles?npTitleIds=PPSA07028_00,"
                  "CUSA01234_00,CUSA05555_00,CUSA07777_00"),
              {Reply{200, std::string{reply}}});
  const std::vector<std::string> ids{"PPSA07028_00", "CUSA01234_00",
                                     "CUSA05555_00", "CUSA07777_00"};
  const auto rows = trophies.title_sets(ids);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 4U);
  const auto& racer = (*rows)[0];
  EXPECT_EQ(racer.np_title_id, "PPSA07028_00");
  ASSERT_EQ(racer.trophy_titles.size(), 1U);
  EXPECT_EQ(racer.trophy_titles[0].np_communication_id, "NPWR20001_00");
  EXPECT_EQ(racer.trophy_titles[0].np_service_name, "trophy2");
  EXPECT_TRUE(racer.trophy_titles[0].has_groups);
  EXPECT_EQ(racer.trophy_titles[0].progress, 37);
  EXPECT_TRUE((*rows)[1].trophy_titles.empty());
  EXPECT_TRUE((*rows)[2].trophy_titles.empty());
  ASSERT_EQ((*rows)[3].trophy_titles.size(), 2U);
  EXPECT_EQ((*rows)[3].trophy_titles[1].np_communication_id, "NPWR07778_00");
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(authorization(), "Bearer " + std::string{token});
}

TEST_F(RestTrophyTitleSets, RefuseNoIds) {
  const auto none = trophies.title_sets({});
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().cause(), Cause::request);
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(RestTrophyTitleSets, RefuseTooManyIds) {
  const std::vector<std::string> six(Trophies::title_sets_limit + 1,
                                     "PPSA07028_00");
  const auto many = trophies.title_sets(six);
  ASSERT_FALSE(many);
  EXPECT_EQ(many.error().cause(), Cause::request);
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(RestTrophyTitleSets, AreMalformedWithoutTitles) {
  rig.fake.on("GET",
              url("/users/me/titles/trophyTitles?npTitleIds=PPSA07028_00"),
              {Reply{200, R"({"trophyTitles":[]})"}});
  const std::vector<std::string> ids{"PPSA07028_00"};
  const auto rows = trophies.title_sets(ids);
  ASSERT_FALSE(rows);
  EXPECT_TRUE(rows.error().is<bad_reply::InvalidSchema>());
}

}  // namespace
}  // namespace psn_test
