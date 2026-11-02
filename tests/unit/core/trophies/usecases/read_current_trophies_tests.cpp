#include "tests/unit/core/trophies/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace trophies_test {
namespace {

using libchiaking::NpTitleId;
using Reply = Rig::Reply;

class CurrentTrophies : public ::testing::Test {
 protected:
  static constexpr std::string_view set = "NPWR07028_00";

  static auto release() -> NpTitleId {
    return NpTitleId::parse("CUSA00341_00").value();
  }

  static auto sets_url() -> std::string {
    return std::string{Rig::base} +
           "/users/me/titles/trophyTitles?npTitleIds=CUSA00341_00";
  }

  static auto sets_reply(std::string_view updated, int bronze) -> Reply {
    return Reply{
        200,
        R"({"titles":[{"npTitleId":"CUSA00341_00","trophyTitles":[)"
        R"({"npServiceName":"trophy","npCommunicationId":")" +
            std::string{set} +
            R"(","trophySetVersion":"01.00","trophyTitleName":"UNCHARTED 4",)"
            R"("trophyTitlePlatform":"PS4","hasTrophyGroups":false,)"
            R"("definedTrophies":{"bronze":2,"silver":0,"gold":0,"platinum":0},)"
            R"("progress":)" +
            std::to_string(bronze * 50) + R"(,"earnedTrophies":{"bronze":)" +
            std::to_string(bronze) +
            R"(,"silver":0,"gold":0,"platinum":0},"hiddenFlag":false,)"
            R"("lastUpdatedDateTime":")" +
            std::string{updated} + R"("}]}]})"};
  }

  static auto definitions() -> Reply {
    return Reply{
        200,
        R"({"trophies":[{"trophyId":0,"trophyType":"bronze","trophyName":"A",)"
        R"("trophyGroupId":"default"},{"trophyId":7,"trophyType":"bronze",)"
        R"("trophyName":"First Treasure","trophyGroupId":"default"}]})"};
  }

  static auto earned(bool treasure) -> Reply {
    std::string rows = R"({"trophyId":0,"earned":true,)"
                       R"("earnedDateTime":"2026-10-23T00:58:53Z"})";
    if (treasure)
      rows += R"(,{"trophyId":7,"earned":true,)"
              R"("earnedDateTime":"2026-10-26T06:43:07Z"})";
    return Reply{200, R"({"trophies":[)" + rows + "]}"};
  }

  static auto definitions_url() -> std::string {
    return Rig::set_url("", set, "/trophyGroups/all/trophies", "trophy", 0);
  }

  static auto earned_url() -> std::string {
    return Rig::set_url("/users/me", set, "/trophyGroups/all/trophies",
                        "trophy", 0);
  }

  Rig rig;
};

TEST_F(CurrentTrophies,
       AlwaysAskForTheProgressAndReadTrophiesAgainOnlyWhenItMoved) {
  rig.http.on("GET", sets_url(),
              {sets_reply("2026-10-23T00:58:53Z", 1),
               sets_reply("2026-10-23T00:58:53Z", 1),
               sets_reply("2026-10-26T06:43:07Z", 2)});
  rig.http.on("GET", definitions_url(), {definitions(), definitions()});
  rig.http.on("GET", earned_url(), {earned(false), earned(true)});

  const auto first = rig.read().current(*rig.user, release(), {});
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->has_value());
  EXPECT_EQ((*first)->title.id, set);
  EXPECT_EQ((*first)->title.name, "UNCHARTED 4");
  ASSERT_EQ((*first)->trophies.trophies.size(), 2U);
  EXPECT_FALSE((*first)->trophies.trophies[1].earned);

  const auto unchanged = rig.read().current(*rig.user, release(), {});
  ASSERT_TRUE(unchanged && unchanged->has_value());
  EXPECT_EQ(rig.requests_to(sets_url()), 2);
  EXPECT_EQ(rig.requests_to(earned_url()), 1);

  const auto moved = rig.read().current(*rig.user, release(), {});
  ASSERT_TRUE(moved && moved->has_value());
  EXPECT_EQ(rig.requests_to(sets_url()), 3);
  EXPECT_EQ(rig.requests_to(earned_url()), 2);
  EXPECT_EQ((*moved)->title.earned.bronze, 2U);
  EXPECT_TRUE((*moved)->trophies.trophies[1].earned);
}

TEST_F(CurrentTrophies, AreNothingForAReleaseWithoutATrophySet) {
  rig.http.on("GET", sets_url(),
              {Reply{200, R"({"titles":[{"npTitleId":"CUSA00341_00",)"
                          R"("trophyTitles":[]}]})"}});
  const auto found = rig.read().current(*rig.user, release(), {});
  ASSERT_TRUE(found);
  EXPECT_FALSE(found->has_value());
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

}  // namespace
}  // namespace trophies_test
