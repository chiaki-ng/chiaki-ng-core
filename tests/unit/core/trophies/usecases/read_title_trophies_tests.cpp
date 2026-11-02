#include "tests/unit/core/trophies/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trophies_test {
namespace {

using libchiaking::trophies::Grade;
using libchiaking::trophies::NpCommunicationId;
using libchiaking::trophies::Rarity;
using libchiaking::trophies::TrophyCounts;
using libchiaking::trophies::TrophyGroupId;
using libchiaking::trophies::TrophyPlatform;
using libchiaking::trophies::TrophySet;
using libchiaking::trophies::TrophyTitle;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = Rig::Reply;

class TitleTrophies : public ::testing::Test {
 protected:
  static constexpr std::string_view racer = "NPWR20001_00";
  static constexpr std::string_view puzzler = "NPWR01234_00";

  static constexpr std::string_view racer_trophies = R"({
  "trophySetVersion": "01.10", "hasTrophyGroups": true,
  "trophies": [
    {"trophyId": 0, "trophyHidden": false, "trophyType": "platinum",
     "trophyName": "Champion", "trophyDetail": "Earn every trophy",
     "trophyIconUrl": "https://image.example/t0.png", "trophyGroupId": "default"},
    {"trophyId": 1, "trophyHidden": false, "trophyType": "bronze",
     "trophyName": "First Lap", "trophyDetail": "Finish a lap",
     "trophyIconUrl": "https://image.example/t1.png", "trophyGroupId": "default"},
    {"trophyId": 2, "trophyHidden": true, "trophyType": "silver",
     "trophyName": "Hundred Laps", "trophyDetail": "Drive 100 laps",
     "trophyIconUrl": "https://image.example/t2.png", "trophyGroupId": "default",
     "trophyProgressTargetValue": "100"},
    {"trophyId": 3, "trophyHidden": false, "trophyType": "bronze",
     "trophyName": "Night Owl", "trophyDetail": "Race at night",
     "trophyIconUrl": "https://image.example/t3.png", "trophyGroupId": "001"},
    {"trophyId": 4, "trophyHidden": false, "trophyType": "bronze",
     "trophyName": "Moonlight", "trophyDetail": "Win at night",
     "trophyIconUrl": "https://image.example/t4.png", "trophyGroupId": "001"}
  ],
  "totalItemCount": 5
})";

  static constexpr std::string_view racer_earned = R"({
  "trophySetVersion": "01.10", "hasTrophyGroups": true,
  "lastUpdatedDateTime": "2026-03-04T18:22:01Z",
  "trophies": [
    {"trophyId": 0, "earned": false, "trophyType": "platinum",
     "trophyRare": 0, "trophyEarnedRate": "1.2"},
    {"trophyId": 1, "earned": true, "earnedDateTime": "2026-02-01T10:00:00Z",
     "trophyType": "bronze", "trophyRare": 3, "trophyEarnedRate": "88.0"},
    {"trophyId": 2, "earned": false, "trophyType": "silver",
     "trophyRare": 2, "trophyEarnedRate": "12.5", "progress": "40",
     "progressRate": 40, "progressedDateTime": "2026-03-01T12:00:00Z"},
    {"trophyId": 4, "earned": true, "earnedDateTime": "2026-03-04T18:22:01Z",
     "trophyType": "bronze", "trophyRare": 1, "trophyEarnedRate": "6.0"}
  ],
  "totalItemCount": 4
})";

  static constexpr std::string_view racer_groups = R"({"trophyGroups":[
  {"trophyGroupId":"default","trophyGroupName":"Sample Racer",
   "trophyGroupIconUrl":"https://image.example/g0.png",
   "definedTrophies":{"bronze":1,"silver":1,"gold":0,"platinum":1}},
  {"trophyGroupId":"001","trophyGroupName":"Night Tracks",
   "trophyGroupDetail":"Add-on","trophyGroupIconUrl":"https://image.example/g1.png",
   "definedTrophies":{"bronze":2,"silver":0,"gold":0,"platinum":0}}]})";

  static constexpr std::string_view racer_group_progress = R"({"trophyGroups":[
  {"trophyGroupId":"default","progress":10,
   "earnedTrophies":{"bronze":1,"silver":0,"gold":0,"platinum":0},
   "lastUpdatedDateTime":"2026-02-01T10:00:00Z"},
  {"trophyGroupId":"001","progress":50,
   "earnedTrophies":{"bronze":1,"silver":0,"gold":0,"platinum":0},
   "lastUpdatedDateTime":"2026-03-04T18:22:01Z"}]})";

  static auto racer_title() -> TrophyTitle {
    return TrophyTitle{.id = NpCommunicationId::parse(racer).value(),
                       .set = TrophySet::ps5,
                       .name = "Sample Racer",
                       .detail = {},
                       .icon_url = "https://image.example/racer.png",
                       .platforms = {TrophyPlatform::ps5},
                       .set_version = "01.10",
                       .has_groups = true,
                       .group_count = 2,
                       .defined = {3, 1, 0, 1},
                       .earned = {2, 0, 0, 0},
                       .progress = 21,
                       .hidden = false,
                       .last_updated = Rig::at("2026-03-04T18:22:01Z")};
  }

  static auto puzzler_title() -> TrophyTitle {
    return TrophyTitle{
        .id = NpCommunicationId::parse(puzzler).value(),
        .set = TrophySet::classic,
        .name = "Old Puzzler",
        .detail = {},
        .icon_url = "https://image.example/puzzler.png",
        .platforms = {TrophyPlatform::ps3, TrophyPlatform::ps_vita},
        .set_version = "01.00",
        .has_groups = false,
        .group_count = 1,
        .defined = {2, 0, 0, 0},
        .earned = {1, 0, 0, 0},
        .progress = 50,
        .hidden = false,
        .last_updated = Rig::at("2019-07-01T09:00:00Z")};
  }

  void script_racer(std::vector<Reply> group_progress) {
    rig.http.on(
        "GET",
        Rig::set_url("", racer, "/trophyGroups/all/trophies", "trophy2", 0),
        {Reply{200, std::string{racer_trophies}}});
    rig.http.on("GET",
                Rig::set_url("/users/me", racer, "/trophyGroups/all/trophies",
                             "trophy2", 0),
                {Reply{200, std::string{racer_earned}}});
    rig.http.on("GET", Rig::set_url("", racer, "/trophyGroups", "trophy2"),
                {Reply{200, std::string{racer_groups}}});
    rig.http.on("GET",
                Rig::set_url("/users/me", racer, "/trophyGroups", "trophy2"),
                std::move(group_progress));
  }

  Rig rig;
};

TEST_F(TitleTrophies, MergeDefinitionsWithEarnedState) {
  script_racer({Reply{200, std::string{racer_group_progress}}});
  const auto read = rig.read().trophies(*rig.user, racer_title(), {});
  ASSERT_TRUE(read);
  EXPECT_EQ(read->title, racer);
  ASSERT_EQ(read->trophies.size(), 5U);
  const auto& platinum = read->trophies[0];
  EXPECT_EQ(platinum.grade, Grade::platinum);
  EXPECT_FALSE(platinum.earned);
  EXPECT_FALSE(platinum.earned_at);
  EXPECT_EQ(platinum.rarity, Rarity::ultra_rare);
  EXPECT_EQ(platinum.earned_rate, 1.2);
  const auto& lap = read->trophies[1];
  EXPECT_TRUE(lap.earned);
  EXPECT_EQ(lap.earned_at, Rig::at("2026-02-01T10:00:00Z"));
  EXPECT_EQ(lap.rarity, Rarity::common);
  EXPECT_EQ(lap.name, "First Lap");
  const auto& counting = read->trophies[2];
  EXPECT_TRUE(counting.hidden);
  EXPECT_EQ(counting.progress_target, std::uint64_t{100});
  EXPECT_FALSE(counting.earned);
  ASSERT_TRUE(counting.progress);
  EXPECT_EQ(counting.progress->value, 40U);
  EXPECT_EQ(counting.progress->percent, 40U);
  EXPECT_EQ(counting.progress->updated_at, Rig::at("2026-03-01T12:00:00Z"));
  const auto& unreported = read->trophies[3];
  EXPECT_FALSE(unreported.earned);
  EXPECT_FALSE(unreported.rarity);
  EXPECT_FALSE(unreported.progress);
  EXPECT_EQ(unreported.group, "001");
  EXPECT_TRUE(read->trophies[4].earned);
  EXPECT_EQ(read->trophies[4].rarity, Rarity::very_rare);
  ASSERT_EQ(read->groups.size(), 2U);
  EXPECT_EQ(read->groups[0].id, TrophyGroupId::base_game());
  EXPECT_EQ(read->groups[0].defined, (TrophyCounts{1, 1, 0, 1}));
  EXPECT_EQ(read->groups[0].earned, (TrophyCounts{1, 0, 0, 0}));
  EXPECT_EQ(read->groups[0].progress, 10);
  EXPECT_EQ(read->groups[1].name, "Night Tracks");
  EXPECT_EQ(read->groups[1].detail, "Add-on");
  EXPECT_EQ(read->groups[1].progress, 50);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_EQ(rig.http.seen.size(), 4U);
}

TEST_F(TitleTrophies, TallyGroupProgressFromTrophiesWhenItsReadFails) {
  script_racer({Reply{503, "{}"}});
  const auto read = rig.read().trophies(*rig.user, racer_title(), {});
  ASSERT_TRUE(read);
  ASSERT_EQ(read->groups.size(), 2U);
  EXPECT_EQ(read->groups[0].earned, (TrophyCounts{1, 0, 0, 0}));
  EXPECT_EQ(read->groups[1].earned, (TrophyCounts{1, 0, 0, 0}));
  EXPECT_EQ(read->groups[0].progress, 0);
  EXPECT_EQ(rig.requests_to(
                Rig::set_url("/users/me", racer, "/trophyGroups", "trophy2")),
            3);
  EXPECT_EQ(rig.waiter.waits, (std::vector{2000, 2000}));
}

TEST_F(TitleTrophies, GiveATitleWithoutGroupsItsBaseGameGroup) {
  rig.http.on(
      "GET",
      Rig::set_url("", puzzler, "/trophyGroups/all/trophies", "trophy", 0),
      {Reply{
          200,
          R"({"trophies":[{"trophyId":0,"trophyType":"bronze","trophyName":"A",)"
          R"("trophyGroupId":"default"},{"trophyId":1,"trophyType":"bronze",)"
          R"("trophyName":"B","trophyGroupId":"default"}]})"}});
  rig.http.on("GET",
              Rig::set_url("/users/me", puzzler, "/trophyGroups/all/trophies",
                           "trophy", 0),
              {Reply{200, R"({"trophies":[{"trophyId":1,"earned":true,)"
                          R"("earnedDateTime":"2019-07-01T09:00:00Z"}]})"}});
  const auto title = puzzler_title();
  const auto read = rig.read().trophies(*rig.user, title, {});
  ASSERT_TRUE(read);
  ASSERT_EQ(read->groups.size(), 1U);
  const auto& group = read->groups[0];
  EXPECT_EQ(group.id, TrophyGroupId::base_game());
  EXPECT_EQ(group.name, title.name);
  EXPECT_EQ(group.defined, title.defined);
  EXPECT_EQ(group.earned, title.earned);
  EXPECT_EQ(group.progress, 50);
  EXPECT_EQ(group.last_updated, title.last_updated);
  ASSERT_EQ(read->trophies.size(), 2U);
  EXPECT_FALSE(read->trophies[0].earned);
  EXPECT_TRUE(read->trophies[1].earned);
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

TEST_F(TitleTrophies, AreMalformedWhenTheSetHasNoTrophies) {
  rig.http.on(
      "GET",
      Rig::set_url("", puzzler, "/trophyGroups/all/trophies", "trophy", 0),
      {Reply{200, R"({"trophies":[]})"}});
  rig.http.on("GET",
              Rig::set_url("/users/me", puzzler, "/trophyGroups/all/trophies",
                           "trophy", 0),
              {Reply{200, R"({"trophies":[]})"}});
  const auto read = rig.read().trophies(*rig.user, puzzler_title(), {});
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().reason, Reason::malformed);
}

TEST_F(TitleTrophies, StayKeptUntilTheTitleReportsAnUpdate) {
  script_racer({Reply{200, std::string{racer_group_progress}}});
  auto title = racer_title();
  EXPECT_TRUE(rig.read().trophies(*rig.user, title, {}));
  EXPECT_EQ(rig.http.seen.size(), 4U);
  rig.advance_wall(std::chrono::hours{48});
  EXPECT_TRUE(rig.read().trophies(*rig.user, title, {}));
  EXPECT_EQ(rig.http.seen.size(), 4U);
  title.last_updated = Rig::at("2026-03-05T08:00:00Z");
  EXPECT_TRUE(rig.read().trophies(*rig.user, title, {}));
  EXPECT_EQ(rig.http.seen.size(), 8U);
  title.last_updated.reset();
  EXPECT_TRUE(rig.read().trophies(*rig.user, title, {}));
  EXPECT_EQ(rig.http.seen.size(), 8U);
  rig.advance_wall(std::chrono::hours{6});
  EXPECT_TRUE(rig.read().trophies(*rig.user, title, {}));
  EXPECT_EQ(rig.http.seen.size(), 12U);
}

}  // namespace
}  // namespace trophies_test
