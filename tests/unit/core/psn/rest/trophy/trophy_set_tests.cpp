#include "tests/unit/core/psn/rest/trophy/trophies_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::rest::TrophySetRef;
using libchiaking::psn::rest::TrophyTally;

class RestTrophySet : public TrophiesFixture {
 protected:
  static constexpr std::string_view set_path =
      "/npCommunicationIds/NPWR20001_00";
  static constexpr std::string_view trophies_page = R"({
  "trophySetVersion": "01.10",
  "hasTrophyGroups": true,
  "trophies": [
    {"trophyId": 0, "trophyHidden": false, "trophyType": "platinum",
     "trophyName": "Champion", "trophyDetail": "Earn every trophy",
     "trophyIconUrl": "https://image.example/t/0.png",
     "trophyGroupId": "default"},
    {"trophyId": 1, "trophyHidden": true, "trophyType": "bronze",
     "trophyName": "Hundred Laps", "trophyDetail": "Drive 100 laps",
     "trophyIconUrl": "https://image.example/t/1.png",
     "trophyGroupId": "001", "trophyProgressTargetValue": "100"}
  ],
  "totalItemCount": 2
})";
  static constexpr std::string_view earned_page = R"({
  "trophySetVersion": "01.10",
  "hasTrophyGroups": true,
  "lastUpdatedDateTime": "2026-03-04T18:22:01Z",
  "trophies": [
    {"trophyId": 0, "trophyHidden": false, "earned": false,
     "trophyType": "platinum", "trophyRare": 0, "trophyEarnedRate": "1.2"},
    {"trophyId": 1, "trophyHidden": true, "earned": true,
     "earnedDateTime": "2026-02-01T10:00:00Z", "trophyType": "bronze",
     "trophyRare": 3, "trophyEarnedRate": "64.5", "progress": "100",
     "progressRate": 100, "progressedDateTime": "2026-02-01T10:00:00Z"}
  ],
  "rarestTrophies": [],
  "totalItemCount": 2
})";
  static constexpr std::string_view groups_reply =
      R"({"trophyGroups":[{"trophyGroupId":"default",
               "trophyGroupName":"Sample Racer","trophyGroupIconUrl":"g0",
               "definedTrophies":{"bronze":30,"silver":8,"gold":2,"platinum":1}},
               {"trophyGroupId":"001","trophyGroupName":"Night Tracks",
               "trophyGroupDetail":"Add-on","trophyGroupIconUrl":"g1",
               "definedTrophies":{"bronze":10,"silver":2,"gold":1,"platinum":0}}]})";
  static constexpr std::string_view earned_groups_reply =
      R"({"trophyGroups":[{"trophyGroupId":"default","progress":40,
               "earnedTrophies":{"bronze":17,"silver":2,"gold":0,"platinum":0},
               "lastUpdatedDateTime":"2026-03-04T18:22:01Z"}]})";

  RestTrophySet() {
    const std::string path{set_path};
    rig.fake.on(
        "GET",
        url(path + "/trophyGroups/all/trophies?npServiceName=trophy2&limit="
                   "100&offset=0"),
        {Reply{200, std::string{trophies_page}}});
    rig.fake.on("GET",
                url("/users/me" + path +
                    "/trophyGroups/all/trophies?npServiceName=trophy2&limit="
                    "100&offset=0"),
                {Reply{200, std::string{earned_page}}});
    rig.fake.on("GET", url(path + "/trophyGroups?npServiceName=trophy2"),
                {Reply{200, std::string{groups_reply}}});
    rig.fake.on("GET",
                url("/users/me" + path + "/trophyGroups?npServiceName=trophy2"),
                {Reply{200, std::string{earned_groups_reply}}});
  }

  const TrophySetRef set{"NPWR20001_00", "trophy2"};
};

TEST_F(RestTrophySet, DefinedTrophiesNameTheSetAndItsService) {
  const auto defined = trophies.trophies(set, 0);
  ASSERT_TRUE(defined);
  ASSERT_EQ(defined->rows.size(), 2U);
  EXPECT_EQ(defined->total, 2U);
  EXPECT_FALSE(defined->next_offset);
  EXPECT_EQ(defined->rows[0].id, 0);
  EXPECT_EQ(defined->rows[0].type, "platinum");
  EXPECT_FALSE(defined->rows[0].progress_target);
  EXPECT_TRUE(defined->rows[1].hidden);
  EXPECT_EQ(defined->rows[1].group_id, "001");
  EXPECT_EQ(defined->rows[1].progress_target, 100);
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(RestTrophySet, EarnedTrophiesNameTheSetAndItsService) {
  const auto earned = trophies.earned_trophies(set, 0);
  ASSERT_TRUE(earned);
  ASSERT_EQ(earned->rows.size(), 2U);
  EXPECT_FALSE(earned->rows[0].earned);
  EXPECT_EQ(earned->rows[0].rarity, 0);
  EXPECT_EQ(earned->rows[0].earned_rate, 1.2);
  EXPECT_TRUE(earned->rows[1].earned);
  EXPECT_EQ(earned->rows[1].earned_at, "2026-02-01T10:00:00Z");
  EXPECT_EQ(earned->rows[1].progress, 100);
  EXPECT_EQ(earned->rows[1].progress_rate, 100);
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(RestTrophySet, GroupsNameTheSetAndItsService) {
  const auto groups = trophies.groups(set);
  ASSERT_TRUE(groups);
  ASSERT_EQ(groups->size(), 2U);
  EXPECT_EQ((*groups)[1].id, "001");
  EXPECT_EQ((*groups)[1].detail, "Add-on");
  EXPECT_EQ((*groups)[0].defined, (TrophyTally{30, 8, 2, 1}));
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(RestTrophySet, EarnedGroupsNameTheSetAndItsService) {
  const auto progress = trophies.earned_groups(set);
  ASSERT_TRUE(progress);
  ASSERT_EQ(progress->size(), 1U);
  EXPECT_EQ((*progress)[0].progress, 40);
  EXPECT_EQ((*progress)[0].earned, (TrophyTally{17, 2, 0, 0}));
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(RestTrophySet, LeavesOutAMissingServiceName) {
  rig.fake.on("GET",
              url("/npCommunicationIds/NPWR01234_00/trophyGroups/all/"
                  "trophies?limit=100&offset=200"),
              {Reply{200, R"({"trophies":[]})"}});
  const auto page = trophies.trophies(TrophySetRef{"NPWR01234_00", ""}, 200);
  ASSERT_TRUE(page);
  EXPECT_TRUE(page->rows.empty());
  EXPECT_EQ(page->received, 0U);
}

}  // namespace
}  // namespace psn_test
