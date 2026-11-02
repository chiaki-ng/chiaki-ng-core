#include "tests/unit/core/trophies/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trophies_test {
namespace {

using libchiaking::trophies::TrophyCounts;
using libchiaking::trophies::TrophyPlatform;
using libchiaking::trophies::TrophySet;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = Rig::Reply;

class TrophyTitles : public ::testing::Test {
 protected:
  static constexpr std::string_view racer = "NPWR20001_00";
  static constexpr std::string_view puzzler = "NPWR01234_00";

  static auto title_json(std::string_view id, std::string_view service,
                         std::string_view platform, bool groups,
                         std::string_view updated) -> std::string {
    return R"({"npServiceName":")" + std::string{service} +
           R"(","npCommunicationId":")" + std::string{id} +
           R"(","trophySetVersion":"01.00","trophyTitleName":"Title )" +
           std::string{id} +
           R"(","trophyTitleIconUrl":"https://image.example/)" +
           std::string{id} + R"(.png","trophyTitlePlatform":")" +
           std::string{platform} + R"(","hasTrophyGroups":)" +
           (groups ? "true" : "false") + R"(,"trophyGroupCount":)" +
           (groups ? "2" : "1") +
           R"(,"definedTrophies":{"bronze":2,"silver":1,"gold":0,"platinum":1},)"
           R"("progress":35,"earnedTrophies":{"bronze":1,"silver":1,"gold":0,)"
           R"("platinum":0},"hiddenFlag":false,"lastUpdatedDateTime":")" +
           std::string{updated} + R"("})";
  }

  static auto titles_page(const std::vector<std::string>& rows,
                          std::optional<std::size_t> next, std::size_t total)
      -> std::string {
    std::string list;
    for (const auto& row : rows) list += (list.empty() ? "" : ",") + row;
    std::string body = R"({"trophyTitles":[)" + list + "]";
    if (next) body += R"(,"nextOffset":)" + std::to_string(*next);
    return body + R"(,"totalItemCount":)" + std::to_string(total) + "}";
  }

  Rig rig;
};

TEST_F(TrophyTitles, ArePagedUntilThereIsNoNextPage) {
  rig.http.on("GET", Rig::titles_url(0),
              {Reply{200, titles_page({title_json(racer, "trophy2", "PS5", true,
                                                  "2026-03-04T18:22:01Z"),
                                       R"({"trophyTitleName":"No id"})"},
                                      100, 3)}});
  rig.http.on(
      "GET", Rig::titles_url(100),
      {Reply{200, titles_page({title_json(puzzler, "trophy", "PS3,PSVITA",
                                          false, "2019-07-01T09:00:00Z")},
                              std::nullopt, 3)}});
  const auto titles = rig.read().titles(*rig.user, {});
  ASSERT_TRUE(titles);
  ASSERT_EQ(titles->size(), 2U);
  const auto& first = (*titles)[0];
  EXPECT_EQ(first.id, racer);
  EXPECT_EQ(first.set, TrophySet::ps5);
  EXPECT_EQ(first.platforms, std::vector{TrophyPlatform::ps5});
  EXPECT_TRUE(first.has_groups);
  EXPECT_EQ(first.group_count, 2);
  EXPECT_EQ(first.defined, (TrophyCounts{2, 1, 0, 1}));
  EXPECT_EQ(first.earned, (TrophyCounts{1, 1, 0, 0}));
  EXPECT_EQ(first.progress, 35);
  EXPECT_EQ(first.last_updated, Rig::at("2026-03-04T18:22:01Z"));
  const auto& second = (*titles)[1];
  EXPECT_EQ(second.id, puzzler);
  EXPECT_EQ(second.set, TrophySet::classic);
  EXPECT_EQ(second.platforms,
            (std::vector{TrophyPlatform::ps3, TrophyPlatform::ps_vita}));
  EXPECT_EQ(rig.requests_to(Rig::titles_url(0)), 1);
  EXPECT_EQ(rig.requests_to(Rig::titles_url(100)), 1);
  const auto again = rig.read().titles(*rig.user, {});
  ASSERT_TRUE(again);
  EXPECT_EQ(again->size(), 2U);
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

TEST_F(TrophyTitles, AreMalformedWhenPagingDoesNotAdvance) {
  rig.http.on("GET", Rig::titles_url(0),
              {Reply{200, titles_page({title_json(racer, "trophy2", "PS5", true,
                                                  "2026-03-04T18:22:01Z")},
                                      0, 5)}});
  const auto titles = rig.read().titles(*rig.user, {});
  ASSERT_FALSE(titles);
  EXPECT_EQ(titles.error().reason, Reason::malformed);
  EXPECT_TRUE(rig.records->names().empty());
}

}  // namespace
}  // namespace trophies_test
