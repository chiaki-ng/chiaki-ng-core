#include "tests/unit/core/psn/rest/trophy/trophies_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::TrophyTally;

class RestTrophyTitles : public TrophiesFixture {
 protected:
  static constexpr std::string_view page = R"({
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
    },
    {"npServiceName": "trophy", "trophyTitleName": "No id"},
    {
      "npServiceName": "trophy",
      "npCommunicationId": "NPWR01234_00",
      "trophySetVersion": "01.00",
      "trophyTitleName": "Old Puzzler",
      "trophyTitleDetail": "Trophies for Old Puzzler",
      "trophyTitleIconUrl": "https://image.example/trophy/puzzler.png",
      "trophyTitlePlatform": "PS3,PSVITA",
      "hasTrophyGroups": false,
      "trophyGroupCount": 1,
      "definedTrophies": {"bronze": 12, "silver": 4, "gold": 2, "platinum": 0},
      "progress": 100,
      "earnedTrophies": {"bronze": 12, "silver": 4, "gold": 2, "platinum": 0},
      "hiddenFlag": true,
      "lastUpdatedDateTime": "2019-07-01T09:00:00Z"
    }
  ],
  "nextOffset": 3,
  "previousOffset": 0,
  "totalItemCount": 4
})";

  void answers(std::string body) {
    rig.fake.on("GET", url("/users/me/trophyTitles?limit=100&offset=0"),
                {Reply{200, std::move(body)}});
  }
};

TEST_F(RestTrophyTitles, ReadRowsAndPaging) {
  answers(std::string{page});
  const auto titles = trophies.titles(0);
  ASSERT_TRUE(titles);
  EXPECT_EQ(titles->received, 3U);
  ASSERT_EQ(titles->rows.size(), 3U);
  EXPECT_EQ(titles->next_offset, 3U);
  EXPECT_EQ(titles->total, 4U);
  const auto& racer = titles->rows[0];
  EXPECT_EQ(racer.np_communication_id, "NPWR20001_00");
  EXPECT_EQ(racer.np_service_name, "trophy2");
  EXPECT_EQ(racer.platform, "PS5");
  EXPECT_TRUE(racer.has_groups);
  EXPECT_EQ(racer.group_count, 2);
  EXPECT_EQ(racer.defined, (TrophyTally{40, 10, 3, 1}));
  EXPECT_EQ(racer.earned, (TrophyTally{18, 2, 0, 0}));
  EXPECT_EQ(racer.progress, 37);
  EXPECT_FALSE(racer.hidden);
  EXPECT_EQ(racer.last_updated, "2026-03-04T18:22:01Z");
  EXPECT_TRUE(titles->rows[1].np_communication_id.empty());
  EXPECT_EQ(titles->rows[2].platform, "PS3,PSVITA");
  EXPECT_TRUE(titles->rows[2].hidden);
}

TEST_F(RestTrophyTitles, AreMalformedWithoutTitles) {
  answers(R"({"totalItemCount":0})");
  const auto titles = trophies.titles(0);
  ASSERT_FALSE(titles);
  EXPECT_TRUE(titles.error().is<bad_reply::InvalidSchema>());
}

}  // namespace
}  // namespace psn_test
