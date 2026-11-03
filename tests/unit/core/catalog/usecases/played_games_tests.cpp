#include "libchiaking/testing/service_builder.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/users/request_budget_core.hpp"
#include "src/core/utils/json/ps_fields.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::Platform;
using libchiaking::catalog::PlayedGame;
using PsnReply = test_support::psn::Reply;
using libchiaking::catalog::PlayedGamesReason;
using UserReason = libchiaking::users::Unanswered::Reason;

class PlayedGamesRig {
 public:
  using Records = libchiaking::testing::InMemoryRecordStore;

  static constexpr std::uint64_t wall_start_us = 1'900'000'000'000'000;

  PlayedGamesRig()
      : PlayedGamesRig(
            std::make_shared<Records>(test_support::process_threading())) {}

  explicit PlayedGamesRig(std::shared_ptr<Records> kept)
      : records(std::move(kept)) {
    wall.now_us = wall_start_us;
    catalog_ = libchiaking::testing::ServiceBuilder::build_unique<
        libchiaking::catalog::Catalog>(
        http, *records, test_support::process_threading(), clock, wall, budget);
    user = signed_in.as(saved());
  }

  static auto page_url(std::size_t offset) -> std::string {
    return "https://m.np.playstation.com/api/gamelist/v2/users/me/"
           "titles?limit=50&offset=" +
           std::to_string(offset);
  }

  static auto title_id(std::size_t number) -> std::string {
    auto digits = std::to_string(number);
    digits.insert(0, 5 - digits.size(), '0');
    return "PPSA" + digits + "_00";
  }

  static auto played_json(std::size_t number) -> std::string {
    return R"({"titleId":")" + title_id(number) + R"(","name":"Game )" +
           std::to_string(number) + R"(","imageUrl":"https://image.example/)" +
           std::to_string(number) +
           R"(.png","category":"ps5_native_game","playCount":2,)"
           R"("firstPlayedDateTime":"2025-01-01T10:00:00Z",)"
           R"("lastPlayedDateTime":"2026-01-01T10:00:00Z",)"
           R"("playDuration":"PT1H"})";
  }

  static auto page_of(std::size_t first, std::size_t count,
                      std::optional<std::size_t> next) -> std::string {
    std::string rows;
    for (std::size_t number = first; number < first + count; ++number)
      rows += (rows.empty() ? "" : ",") + played_json(number);
    std::string body = R"({"titles":[)" + rows + "]";
    if (next) body += R"(,"nextOffset":)" + std::to_string(*next);
    return body + "}";
  }

  static auto at(std::string_view text) -> libchiaking::catalog::Time {
    const auto instant = libchiaking::json::ps::Instant::from_text(text);
    EXPECT_TRUE(instant) << text;
    return instant.value().time();
  }

  auto saved() const -> libchiaking::users::SavedSignIn {
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{7}, "player",
                                          libchiaking::psn::auth::Npsso{"np"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"SYNTHETIC_APP_TOKEN"},
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{wall_start_us} + std::chrono::hours{72}});
    saved.store_refresh.emplace(
        libchiaking::psn::auth::AppRefreshToken{"refresh-7"});
    return saved;
  }

  auto games()
      -> libchiaking::Expected<std::vector<PlayedGame>,
                               libchiaking::catalog::PlayedGamesFailure> {
    return catalog_->played_games(*user);
  }

  auto library() -> libchiaking::catalog::Catalog& { return *catalog_; }

  void advance_wall(std::chrono::milliseconds by) {
    wall.now_us += static_cast<std::uint64_t>(by.count()) * 1000U;
  }

  auto requests() const -> int { return http.count("/gamelist/"); }

  auto kept() const -> bool {
    return std::ranges::any_of(records->names(), [](const auto& name) {
      return name.text() == "user/7/played_games";
    });
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp http;
  std::shared_ptr<Records> records;
  test_support::users::SignedIn signed_in{http, clock, wall};
  test_support::FakeWaiter waiter{&clock};
  libchiaking::users::RequestBudget budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, waiter.factory())};
  std::shared_ptr<libchiaking::users::PsUser> user;

 private:
  std::unique_ptr<libchiaking::catalog::Catalog> catalog_;
};

class PlayedGames : public ::testing::Test {
 protected:
  static auto page_url(std::size_t offset) -> std::string {
    return PlayedGamesRig::page_url(offset);
  }

  static auto page_of(std::size_t first, std::size_t count,
                      std::optional<std::size_t> next) -> std::string {
    return PlayedGamesRig::page_of(first, count, next);
  }

  void answers_two_pages() {
    rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 50, 50)}});
    rig.http.on(
        "GET", page_url(50),
        {PsnReply{
            200,
            R"({"titles":[)" + PlayedGamesRig::played_json(51) +
                R"(,{"titleId":"PPSA00051_00","name":"Again"},)"
                R"({"titleId":"not an id","name":"Odd"},)"
                R"({"titleId":"CUSA01234_00","localizedName":"Old Puzzler",)"
                R"("name":"Puzzler","category":"ps4_game",)"
                R"("playDuration":"forever","playCount":"x"},)"
                R"({"titleId":"PPSA00099_00","name":"Unknown",)"
                R"("category":"unknown"}],"totalItemCount":54})"}});
  }

  PlayedGamesRig rig;
};

TEST_F(PlayedGames, FollowAndReadEveryPage) {
  answers_two_pages();
  const auto games = rig.games();
  ASSERT_TRUE(games);
  ASSERT_EQ(games->size(), 53U);
  EXPECT_EQ(rig.requests(), 2);
  EXPECT_TRUE(rig.http.unmatched.empty());
  const auto& first = games->front();
  EXPECT_EQ(first.title, "PPSA00001_00");
  EXPECT_EQ(first.name, "Game 1");
  EXPECT_EQ(first.platform, Platform::ps5);
  EXPECT_EQ(first.category, "ps5_native_game");
  EXPECT_EQ(first.image_url, "https://image.example/1.png");
  EXPECT_EQ(first.play_time, std::chrono::hours{1});
  EXPECT_EQ(first.play_count, 2U);
  EXPECT_EQ(first.first_played, PlayedGamesRig::at("2025-01-01T10:00:00Z"));
  EXPECT_EQ(first.last_played, PlayedGamesRig::at("2026-01-01T10:00:00Z"));
  EXPECT_EQ((*games)[50].title, "PPSA00051_00");
  EXPECT_EQ((*games)[50].name, "Game 51");
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            "Bearer SYNTHETIC_APP_TOKEN");
}

TEST_F(PlayedGames, KeepWhatTheyCanReadOfAnUnusualRow) {
  answers_two_pages();
  const auto games = rig.games();
  ASSERT_TRUE(games);
  ASSERT_EQ(games->size(), 53U);
  const auto& puzzler = (*games)[51];
  EXPECT_EQ(puzzler.title, "CUSA01234_00");
  EXPECT_EQ(puzzler.name, "Old Puzzler");
  EXPECT_EQ(puzzler.platform, Platform::ps4);
  EXPECT_FALSE(puzzler.play_time);
  EXPECT_FALSE(puzzler.play_count);
  EXPECT_FALSE(puzzler.first_played);
  EXPECT_FALSE(puzzler.last_played);
  EXPECT_TRUE(puzzler.image_url.empty());
  const auto& unknown = (*games)[52];
  EXPECT_FALSE(unknown.platform);
  EXPECT_EQ(unknown.category, "unknown");
}

TEST_F(PlayedGames, AskForTheNextPageAfterAFullPageWithoutANextOffset) {
  rig.http.on("GET", page_url(0),
              {PsnReply{200, page_of(1, 50, std::nullopt)}});
  rig.http.on("GET", page_url(50), {PsnReply{200, R"({"titles":[]})"}});
  const auto games = rig.games();
  ASSERT_TRUE(games);
  EXPECT_EQ(games->size(), 50U);
  EXPECT_EQ(rig.requests(), 2);
}

TEST_F(PlayedGames, EndAtAShortPageWithoutANextOffset) {
  rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 3, std::nullopt)}});
  const auto games = rig.games();
  ASSERT_TRUE(games);
  EXPECT_EQ(games->size(), 3U);
  EXPECT_EQ(rig.requests(), 1);
}

TEST_F(PlayedGames, StopPagingAtThePageCap) {
  for (std::size_t page = 0; page < 60; ++page)
    rig.http.on("GET", page_url(page * 50),
                {PsnReply{200, page_of(page * 50 + 1, 50, (page + 1) * 50)}});
  const auto games = rig.games();
  ASSERT_FALSE(games);
  EXPECT_EQ(games.error().reason(), PlayedGamesReason::malformed);
  EXPECT_EQ(rig.requests(), 50);
  EXPECT_FALSE(rig.kept());
  EXPECT_EQ(rig.waiter.waits.size(), 45U);
}

TEST_F(PlayedGames, WaitTheirTurnOnTheRequestBudget) {
  for (std::size_t page = 0; page < 7; ++page)
    rig.http.on("GET", page_url(page * 50),
                {PsnReply{200, page_of(page * 50 + 1, 50, (page + 1) * 50)}});
  rig.http.on("GET", page_url(350), {PsnReply{200, R"({"titles":[]})"}});
  const auto games = rig.games();
  ASSERT_TRUE(games);
  EXPECT_EQ(games->size(), 350U);
  EXPECT_EQ(rig.requests(), 8);
  EXPECT_EQ(rig.waiter.waits, (std::vector{200, 200, 200}));
}

TEST_F(PlayedGames, CoolTheAccountDownAfterA429) {
  rig.http.on(
      "GET", page_url(0),
      {PsnReply{429, "{}"}, PsnReply{200, page_of(1, 1, std::nullopt)}});
  const auto throttled = rig.games();
  ASSERT_FALSE(throttled);
  EXPECT_EQ(throttled.error().reason(), PlayedGamesReason::user);
  ASSERT_TRUE(throttled.error().cause());
  EXPECT_EQ(throttled.error().cause()->reason, UserReason::rate_limited);
  const auto cooling = rig.games();
  ASSERT_FALSE(cooling);
  ASSERT_TRUE(cooling.error().cause());
  EXPECT_EQ(cooling.error().cause()->reason, UserReason::rate_limited);
  EXPECT_EQ(rig.requests(), 1);
  rig.clock.advance_ms(
      std::chrono::milliseconds{std::chrono::minutes{15}}.count());
  EXPECT_TRUE(rig.games());
  EXPECT_EQ(rig.requests(), 2);
}

TEST_F(PlayedGames, CallANextOffsetThatDoesNotAdvanceMalformed) {
  rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 50, 50)}});
  rig.http.on("GET", page_url(50), {PsnReply{200, page_of(51, 50, 20)}});
  const auto games = rig.games();
  ASSERT_FALSE(games);
  EXPECT_EQ(games.error().reason(), PlayedGamesReason::malformed);
  EXPECT_EQ(rig.requests(), 2);
  EXPECT_FALSE(rig.kept());
}

TEST(PlayedGamesAcrossRestarts, AreKeptForSixHours) {
  auto records = std::make_shared<PlayedGamesRig::Records>(
      test_support::process_threading());
  std::vector<PlayedGame> first_read;
  {
    PlayedGamesRig rig{records};
    rig.http.on("GET", PlayedGamesRig::page_url(0),
                {PsnReply{200, PlayedGamesRig::page_of(1, 4, std::nullopt)}});
    const auto games = rig.games();
    ASSERT_TRUE(games);
    EXPECT_EQ(games->size(), 4U);
    first_read = *games;
    const auto again = rig.games();
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, first_read);
    EXPECT_EQ(rig.requests(), 1);
    EXPECT_TRUE(rig.kept());
  }
  PlayedGamesRig restarted{records};
  restarted.http.on(
      "GET", PlayedGamesRig::page_url(0),
      {PsnReply{200, PlayedGamesRig::page_of(10, 2, std::nullopt)}});
  const auto kept = restarted.games();
  ASSERT_TRUE(kept);
  EXPECT_EQ(*kept, first_read);
  EXPECT_EQ(restarted.requests(), 0);
  restarted.advance_wall(std::chrono::hours{6} - std::chrono::milliseconds{1});
  EXPECT_TRUE(restarted.games());
  EXPECT_EQ(restarted.requests(), 0);
  restarted.advance_wall(std::chrono::milliseconds{1});
  const auto renewed = restarted.games();
  ASSERT_TRUE(renewed);
  ASSERT_EQ(renewed->size(), 2U);
  EXPECT_EQ(restarted.requests(), 1);
  EXPECT_EQ(renewed->front().title, "PPSA00010_00");
}

TEST_F(PlayedGames, AreDroppedOnStartingOverAndForgetting) {
  rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 2, std::nullopt)}});
  EXPECT_TRUE(rig.games());
  EXPECT_TRUE(rig.kept());
  rig.library().start_over(*rig.user);
  EXPECT_FALSE(rig.kept());
  EXPECT_TRUE(rig.games());
  EXPECT_EQ(rig.requests(), 2);
  EXPECT_TRUE(rig.kept());
  rig.library().forget(rig.user->account_id());
  EXPECT_FALSE(rig.kept());
}

TEST_F(PlayedGames, AreListedAndClearedAsTheirOwnCache) {
  rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 2, std::nullopt)}});
  EXPECT_TRUE(rig.games());
  const auto held =
      rig.library().cache_entries(libchiaking::catalog::CacheId::played_games);
  ASSERT_TRUE(held);
  ASSERT_EQ(held->size(), 1U);
  EXPECT_EQ(held->front().account, libchiaking::AccountId{7});
  EXPECT_EQ(held->front().expires_at,
            held->front().stored_at + std::chrono::hours{6});
  const auto summaries = rig.library().caches();
  ASSERT_TRUE(summaries);
  EXPECT_EQ(summaries->size(), libchiaking::catalog::every_cache.size());
  EXPECT_TRUE(
      rig.library().clear_cache(libchiaking::catalog::CacheId::played_games));
  EXPECT_FALSE(rig.kept());
  EXPECT_TRUE(rig.games());
  EXPECT_EQ(rig.requests(), 2);
}

TEST_F(PlayedGames, ReportAFailureWithoutKeepingIt) {
  rig.http.on(
      "GET", page_url(0),
      {PsnReply{500, "{}"}, PsnReply{200, page_of(1, 1, std::nullopt)}});
  const auto failed = rig.games();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason(), PlayedGamesReason::user);
  ASSERT_TRUE(failed.error().cause());
  EXPECT_EQ(failed.error().cause()->reason, UserReason::unavailable);
  EXPECT_FALSE(rig.kept());
  const auto retried = rig.games();
  ASSERT_TRUE(retried);
  EXPECT_EQ(retried->size(), 1U);
}

TEST_F(PlayedGames, ReportAGarbledReplyAsMalformed) {
  rig.http.on("GET", page_url(0), {PsnReply{200, R"({"total":0})"}});
  const auto malformed = rig.games();
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().reason(), PlayedGamesReason::user);
  ASSERT_TRUE(malformed.error().cause());
  EXPECT_EQ(malformed.error().cause()->reason, UserReason::malformed);
}

TEST_F(PlayedGames, AreNotAskedForByAHomeUserOrAStoppedCaller) {
  rig.http.on("GET", page_url(0), {PsnReply{200, page_of(1, 1, std::nullopt)}});
  auto added = rig.signed_in.users.add_home_user(libchiaking::AccountId{9},
                                                 "Living Room");
  ASSERT_TRUE(added);
  const auto home = rig.library().played_games(*added.value());
  ASSERT_FALSE(home);
  EXPECT_EQ(home.error().reason(), PlayedGamesReason::user);
  auto stopped = std::make_shared<test_support::StopState>();
  stopped->stopped = true;
  const auto cancelled = rig.library().played_games(
      *rig.user, libchiaking::platform::StopToken{stopped});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().reason(), PlayedGamesReason::cancelled);
  EXPECT_LE(rig.requests(), 1);
  EXPECT_FALSE(rig.kept());
}

}  // namespace
}  // namespace catalog_test
