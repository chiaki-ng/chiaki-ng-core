#include "tests/unit/core/trophies/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace trophies_test {
namespace {

using libchiaking::NpTitleId;
using libchiaking::trophies::NpCommunicationId;
using libchiaking::trophies::TitleSets;
using libchiaking::trophies::Trophies;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = Rig::Reply;

class TitleSetLookup {
 public:
  static auto release(std::size_t number) -> NpTitleId {
    auto digits = std::to_string(number);
    digits.insert(0, 5 - digits.size(), '0');
    return NpTitleId::parse("PPSA" + digits + "_00").value();
  }

  static auto releases(std::size_t first, std::size_t count)
      -> std::vector<NpTitleId> {
    std::vector<NpTitleId> made;
    for (std::size_t number = first; number < first + count; ++number)
      made.push_back(release(number));
    return made;
  }

  static auto set_for(const NpTitleId& title) -> std::string {
    return "NPWR" + title.text().substr(4);
  }

  static auto sets_url(const std::vector<NpTitleId>& titles) -> std::string {
    std::string ids;
    for (const auto& title : titles)
      ids += std::string{ids.empty() ? "" : ","} + title.text();
    return std::string{Rig::base} +
           "/users/me/titles/trophyTitles?npTitleIds=" + ids;
  }

  static auto trophy_title_json(std::string_view set) -> std::string {
    return R"({"npServiceName":"trophy2","npCommunicationId":")" +
           std::string{set} +
           R"(","trophySetVersion":"01.00","trophyTitleName":"Synthetic Game",)"
           R"("trophyTitlePlatform":"PS5","hasTrophyGroups":false,)"
           R"("definedTrophies":{"bronze":10,"silver":2,"gold":1,"platinum":1},)"
           R"("progress":12,"earnedTrophies":{"bronze":3,"silver":0,"gold":0,)"
           R"("platinum":0},"hiddenFlag":false,)"
           R"("lastUpdatedDateTime":"2026-03-04T18:22:01Z"})";
  }

  static auto reply_for(const std::vector<NpTitleId>& titles) -> std::string {
    std::string rows;
    for (const auto& title : titles)
      rows += std::string{rows.empty() ? "" : ","} + R"({"npTitleId":")" +
              title.text() + R"(","trophyTitles":[)" +
              trophy_title_json(set_for(title)) + "]}";
    return R"({"titles":[)" + rows + "]}";
  }

  static void answers(Rig& rig, const std::vector<NpTitleId>& titles) {
    rig.http.on("GET", sets_url(titles), {Reply{200, reply_for(titles)}});
  }

  static auto each_maps_to_its_own_set(const std::vector<TitleSets>& found,
                                       const std::vector<NpTitleId>& titles)
      -> ::testing::AssertionResult {
    if (found.size() != titles.size())
      return ::testing::AssertionFailure()
             << found.size() << " entries for " << titles.size() << " titles";
    for (std::size_t index = 0; index < titles.size(); ++index) {
      const auto& entry = found[index];
      if (entry.title != titles[index] || entry.sets.size() != 1 ||
          entry.sets[0] != set_for(titles[index]))
        return ::testing::AssertionFailure()
               << "entry " << index << " does not map " << titles[index].text()
               << " to " << set_for(titles[index]);
    }
    return ::testing::AssertionSuccess();
  }
};

class TrophyTitleSets : public ::testing::Test, protected TitleSetLookup {
 protected:
  Rig rig;
};

TEST_F(TrophyTitleSets, AreAskedForFiveReleasesAtATimeInOrder) {
  const auto titles = releases(10001, 7);
  const std::vector first(titles.begin(), titles.begin() + 5);
  const std::vector rest(titles.begin() + 5, titles.end());
  answers(rig, first);
  answers(rig, rest);
  const auto found = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(each_maps_to_its_own_set(*found, titles));
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_EQ(rig.http.seen[0].url,
            std::string{Rig::base} +
                "/users/me/titles/trophyTitles?npTitleIds=PPSA10001_00,"
                "PPSA10002_00,PPSA10003_00,PPSA10004_00,PPSA10005_00");
  EXPECT_EQ(rig.http.seen[1].url, sets_url(rest));
  EXPECT_EQ(rig.http.seen[0].header("Authorization"),
            "Bearer " + std::string{Rig::app_token});
  static_assert(Trophies::titles_per_request == 5);
}

TEST_F(TrophyTitleSets, AreAskedOnceForARepeatedRelease) {
  const auto racer = release(20001);
  const auto puzzler = release(20002);
  answers(rig, {racer, puzzler});
  const std::vector asked{racer, puzzler, racer, puzzler, racer};
  const auto found = rig.read().title_sets(*rig.user, asked, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(each_maps_to_its_own_set(*found, {racer, puzzler}));
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(TrophyTitleSets, AreNotAskedForNoReleases) {
  const auto found = rig.read().title_sets(*rig.user, {}, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->empty());
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(TrophyTitleSets, AreEmptyForReleasesPsnKnowsNoSetFor) {
  const auto twin = release(30001);
  const auto bare = release(30002);
  const auto missing = release(30003);
  const auto odd = release(30004);
  rig.http.on(
      "GET", sets_url({twin, bare, missing, odd}),
      {Reply{200, R"({"titles":[)"
                  R"({"npTitleId":"PPSA30001_00","trophyTitles":[)" +
                      trophy_title_json("NPWR30001_00") + "," +
                      trophy_title_json("NPWR30009_00") + "," +
                      trophy_title_json("NPWR30001_00") + "]}," +
                      R"({"npTitleId":"PPSA30002_00","trophyTitles":[]},)"
                      R"({"npTitleId":"PPSA30004_00","trophyTitles":[)" +
                      trophy_title_json("not a set") + "]}," +
                      R"({"npTitleId":"PPSA99999_00","trophyTitles":[)" +
                      trophy_title_json("NPWR99999_00") + "]}]}"}});
  const auto found = rig.read().title_sets(
      *rig.user, std::vector{twin, bare, missing, odd}, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 4U);
  EXPECT_EQ((*found)[0].title, twin);
  EXPECT_EQ((*found)[0].sets,
            (std::vector{NpCommunicationId::parse("NPWR30001_00").value(),
                         NpCommunicationId::parse("NPWR30009_00").value()}));
  EXPECT_EQ((*found)[1].title, bare);
  EXPECT_TRUE((*found)[1].sets.empty());
  EXPECT_EQ((*found)[2].title, missing);
  EXPECT_TRUE((*found)[2].sets.empty());
  EXPECT_EQ((*found)[3].title, odd);
  EXPECT_TRUE((*found)[3].sets.empty());
  const auto again =
      rig.read().title_sets(*rig.user, std::vector{missing, bare}, {});
  ASSERT_TRUE(again);
  ASSERT_EQ(again->size(), 2U);
  EXPECT_TRUE((*again)[0].sets.empty());
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST(TrophyTitleSetsAcrossRestarts, AreKeptPerRelease) {
  const auto records = Rig::fresh_records();
  const auto known = TitleSetLookup::releases(40001, 3);
  {
    Rig rig{records};
    TitleSetLookup::answers(rig, known);
    EXPECT_TRUE(rig.read().title_sets(*rig.user, known, {}));
    EXPECT_EQ(rig.http.seen.size(), 1U);
  }
  EXPECT_TRUE(Rig::keeps(*records, "trophies/7/played-ppsa40002_00"));
  Rig restarted{records};
  const auto fresh = TitleSetLookup::release(40009);
  TitleSetLookup::answers(restarted, {fresh});
  const std::vector mixed{known[2], fresh, known[0]};
  const auto found = restarted.read().title_sets(*restarted.user, mixed, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(TitleSetLookup::each_maps_to_its_own_set(*found, mixed));
  ASSERT_EQ(restarted.http.seen.size(), 1U);
  EXPECT_EQ(restarted.http.seen[0].url, TitleSetLookup::sets_url({fresh}));
}

TEST_F(TrophyTitleSets, ExpireSixHoursAfterTheyWereKept) {
  const auto titles = releases(50001, 2);
  rig.http.on("GET", sets_url(titles),
              {Reply{200, reply_for(titles)},
               Reply{200, R"({"titles":[{"npTitleId":"PPSA50001_00",)"
                          R"("trophyTitles":[)" +
                              trophy_title_json("NPWR50005_00") + "]}]}"}});
  EXPECT_TRUE(rig.read().title_sets(*rig.user, titles, {}));
  rig.advance_wall(std::chrono::hours{6} - std::chrono::milliseconds{1});
  const auto kept = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(kept);
  EXPECT_TRUE(each_maps_to_its_own_set(*kept, titles));
  EXPECT_EQ(rig.http.seen.size(), 1U);
  rig.advance_wall(std::chrono::milliseconds{1});
  const auto renewed = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(renewed);
  ASSERT_EQ(renewed->size(), 2U);
  ASSERT_FALSE((*renewed)[0].sets.empty());
  EXPECT_EQ((*renewed)[0].sets[0], "NPWR50005_00");
  EXPECT_TRUE((*renewed)[1].sets.empty());
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

TEST_F(TrophyTitleSets, WaitTheirTurnAcrossManyBatches) {
  const auto titles = releases(60001, 30);
  for (std::size_t from = 0; from < titles.size(); from += 5)
    answers(rig, std::vector(
                     std::next(titles.begin(), static_cast<long>(from)),
                     std::next(titles.begin(), static_cast<long>(from + 5))));
  const auto found = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(each_maps_to_its_own_set(*found, titles));
  EXPECT_EQ(rig.http.seen.size(), 6U);
  EXPECT_EQ(rig.waiter.waits, (std::vector{200}));
}

TEST_F(TrophyTitleSets, FailTheCallOnAFailedBatchAndKeepTheBatchesBeforeIt) {
  const auto titles = releases(70001, 7);
  const std::vector first(titles.begin(), titles.begin() + 5);
  const std::vector rest(titles.begin() + 5, titles.end());
  answers(rig, first);
  rig.http.on(
      "GET", sets_url(rest),
      {Reply{200, R"({"trophyTitles":[]})"}, Reply{200, reply_for(rest)}});
  const auto failed = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason, Reason::malformed);
  const auto retried = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(retried);
  EXPECT_TRUE(each_maps_to_its_own_set(*retried, titles));
  EXPECT_EQ(rig.requests_to(sets_url(first)), 1);
  EXPECT_EQ(rig.requests_to(sets_url(rest)), 2);
}

TEST_F(TrophyTitleSets, CoolTheAccountDownAfterA429) {
  const auto titles = releases(80001, 2);
  rig.http.on("GET", sets_url(titles),
              {Reply{429, "{}"}, Reply{200, reply_for(titles)}});
  const auto throttled = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_FALSE(throttled);
  EXPECT_EQ(throttled.error().reason, Reason::rate_limited);
  const auto cooling = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error().reason, Reason::rate_limited);
  EXPECT_EQ(rig.requests_to(sets_url(titles)), 1);
  rig.clock.advance_ms(
      std::chrono::milliseconds{std::chrono::minutes{15}}.count());
  EXPECT_TRUE(rig.read().title_sets(*rig.user, titles, {}));
  EXPECT_EQ(rig.requests_to(sets_url(titles)), 2);
  EXPECT_EQ(rig.records->names().size(), 2U);
}

TEST_F(TrophyTitleSets, RenewARefusedTokenOnce) {
  rig.renews_store_access();
  const auto titles = releases(85001, 1);
  rig.http.on("GET", sets_url(titles),
              {Reply{401, R"({"error":"invalid token"})"},
               Reply{200, reply_for(titles)}});
  const auto found = rig.read().title_sets(*rig.user, titles, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(each_maps_to_its_own_set(*found, titles));
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            "Bearer " + std::string{Rig::renewed_token});
}

TEST_F(TrophyTitleSets, AreNotAskedForSignedOutOrStoppedCallers) {
  const auto titles = releases(90001, 2);
  answers(rig, titles);
  auto added = rig.signed_in.users.add_home_user(libchiaking::AccountId{9},
                                                 "Living Room");
  ASSERT_TRUE(added);
  const auto home = rig.read().title_sets(*added.value(), titles, {});
  ASSERT_FALSE(home);
  EXPECT_EQ(home.error().reason, Reason::signed_out);
  auto stopped = std::make_shared<test_support::StopState>();
  stopped->stopped = true;
  const auto cancelled = rig.read().title_sets(
      *rig.user, titles, libchiaking::platform::StopToken{stopped});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().reason, Reason::cancelled);
  EXPECT_EQ(rig.requests_to(sets_url(titles)), 0);
  EXPECT_TRUE(rig.records->names().empty());
}

TEST_F(TrophyTitleSets, AreDroppedByStartingOver) {
  const auto titles = releases(95001, 1);
  rig.http.on("GET", sets_url(titles),
              {Reply{200, reply_for(titles)}, Reply{200, reply_for(titles)}});
  EXPECT_TRUE(rig.read().title_sets(*rig.user, titles, {}));
  rig.read().start_over(rig.user->account_id());
  EXPECT_TRUE(rig.records->names().empty());
  EXPECT_TRUE(rig.read().title_sets(*rig.user, titles, {}));
  EXPECT_EQ(rig.requests_to(sets_url(titles)), 2);
}

}  // namespace
}  // namespace trophies_test
