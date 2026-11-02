#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace shared = libchiaking::progress;
using cloud_launches::Launching;
using Reason = shared::WaitReason;

class CloudLaunchWaits : public cloud_launches::LaunchTest {
 protected:
  static auto has_text(const Launching& launching, std::string_view text)
      -> bool {
    return std::ranges::find(launching.listener().texts, text) !=
           launching.listener().texts.end();
  }

  static auto first_wait(const std::vector<play::LaunchProgress>& typed) {
    return std::ranges::find_if(typed, [](const auto& progress) {
      return progress.wait().has_value();
    });
  }

  const shared::Wait again{Reason::title_not_ready,
                           std::chrono::milliseconds{0}};
};

TEST_F(CloudLaunchWaits, ReportsWaitingWithTypedCounts) {
  Launching waiting{ps5_case()};
  waiting.ps().busy_locks = 1;
  waiting.ps().waits = {
      R"({"queued":true,"pollFrequency":1,"queuePosition":7})",
      R"({"dataMigration":true,"pollFrequency":1,"dataMigrationPercentageComplete":40})",
      R"({"dataMigration":true,"pollFrequency":1,"dataMigrationPercentageComplete":250})"};
  EXPECT_TRUE(waiting.core().run());
  EXPECT_EQ(waiting.waiter().waits,
            std::vector<int>({1000, 1000, 1000, 1000}));
  std::vector<play::LaunchProgress> counted;
  for (const auto& progress : waiting.listener().typed)
    if (progress.attempt()) counted.push_back(progress);
  ASSERT_EQ(counted.size(), 4U);
  EXPECT_EQ(counted[0].activity(), Activity::reserving_server);
  EXPECT_EQ(counted[0].attempt(), 1U);
  EXPECT_EQ(counted[1].activity(), Activity::allocating_server);
  EXPECT_EQ(counted[1].queue_position(), 7U);
  EXPECT_EQ(counted[1].attempt(), 1U);
  EXPECT_FALSE(counted[1].migration_percent());
  EXPECT_EQ(counted[2].activity(), Activity::migrating_data);
  EXPECT_EQ(counted[2].migration_percent(), 40U);
  EXPECT_EQ(counted[2].attempt(), 2U);
  EXPECT_FALSE(counted[2].queue_position());
  EXPECT_EQ(counted[3].activity(), Activity::migrating_data);
  EXPECT_EQ(counted[3].migration_percent(), 100U);
  EXPECT_TRUE(has_text(waiting, "Closing old session - Attempt 1"));
  EXPECT_TRUE(has_text(
      waiting, "Allocating streaming slot - Queue position: 7 - Attempt 1"));
  EXPECT_TRUE(has_text(waiting, "Migrating data (40%) - Attempt 2"));
  EXPECT_TRUE(has_text(waiting, "Migrating data (250%) - Attempt 3"));
}

TEST_F(CloudLaunchWaits, ReportsAHeldSessionLockAsThreeTypedReportsInOrder) {
  Launching held{ps5_case()};
  held.ps().busy_locks = 2;
  held.ps().lock_poll_seconds = 10;
  EXPECT_TRUE(held.core().run());
  EXPECT_EQ(held.waiter().waits, std::vector<int>({10'000, 10'000}));

  const auto& typed = held.listener().typed;
  const auto locking = std::ranges::find_if(typed, [](const auto& progress) {
    return step_of(progress) == Step{7U, 10U};
  });
  ASSERT_NE(locking, typed.end());
  ASSERT_GT(typed.end() - locking, 3);
  EXPECT_EQ(locking->activity(), Activity::reserving_server);
  EXPECT_FALSE(locking->wait());
  EXPECT_FALSE(locking->attempt());
  EXPECT_EQ(locking->status(),
            (shared::Status{shared::Position{7, 10}, 1, locking->elapsed(),
                            std::nullopt}));

  const auto& first = locking[1];
  const auto& second = locking[2];
  EXPECT_EQ(first.activity(), Activity::reserving_server);
  EXPECT_EQ(second.activity(), Activity::reserving_server);
  EXPECT_EQ(first.phase(), play::LaunchPhase::preparing);
  EXPECT_EQ(second.phase(), play::LaunchPhase::preparing);
  EXPECT_EQ(first.wait(), (shared::Wait{Reason::previous_session_closing,
                                        std::chrono::seconds{10}}));
  EXPECT_EQ(second.wait(), first.wait());
  EXPECT_EQ(first.attempt(), 1U);
  EXPECT_EQ(second.attempt(), 2U);
  EXPECT_EQ(first.status(),
            (shared::Status{std::nullopt, 1, first.elapsed(), first.wait()}));
  EXPECT_EQ(second.status(),
            (shared::Status{std::nullopt, 2, second.elapsed(), second.wait()}));
  EXPECT_TRUE(first.status().is_waiting());
  EXPECT_TRUE(second.status().is_waiting());

  EXPECT_EQ(locking[3].activity(), Activity::measuring_datacenters);
  EXPECT_FALSE(locking[3].wait());
  EXPECT_EQ(step_of(locking[3]), (Step{8U, 10U}));
  EXPECT_EQ(waits_of(typed).size(), 2U);
  for (const auto& progress : typed)
    EXPECT_EQ(progress.elapsed(), std::chrono::microseconds{0});
  const auto text = std::ranges::find(held.listener().texts,
                                      "Locking Session - Step 7 of 10");
  ASSERT_NE(text, held.listener().texts.end());
  ASSERT_GT(held.listener().texts.end() - text, 2);
  EXPECT_EQ(text[1], "Closing old session - Attempt 1");
  EXPECT_EQ(text[2], "Closing old session - Attempt 2");
  EXPECT_EQ(held.core().progress(), typed.back());
  EXPECT_FALSE(held.core().progress().wait());
}

TEST_F(CloudLaunchWaits, NamesTheReasonAndDelayOfEveryAllocationWait) {
  Launching waiting{ps5_case()};
  waiting.ps().waits = {
      R"({"queued":true,"pollFrequency":4,"queuePosition":7})",
      R"({"queued":true,"pollFrequency":3})",
      R"({"dataMigration":true,"pollFrequency":2,"dataMigrationPercentageComplete":40})"};
  EXPECT_TRUE(waiting.core().run());
  EXPECT_EQ(waiting.waiter().waits, std::vector<int>({4000, 3000, 2000}));
  const auto waits = waits_of(waiting.listener().typed);
  ASSERT_EQ(waits.size(), 3U);
  EXPECT_EQ(waits[0].activity(), Activity::allocating_server);
  EXPECT_EQ(waits[0].queue_position(), 7U);
  EXPECT_EQ(waits[0].wait(),
            (shared::Wait{Reason::server_queue, std::chrono::seconds{4}}));
  EXPECT_EQ(waits[0].attempt(), 1U);
  EXPECT_EQ(waits[1].activity(), Activity::allocating_server);
  EXPECT_FALSE(waits[1].queue_position());
  EXPECT_EQ(waits[1].wait(),
            (shared::Wait{Reason::server_queue, std::chrono::seconds{3}}));
  EXPECT_EQ(waits[1].attempt(), 2U);
  EXPECT_EQ(waits[2].activity(), Activity::migrating_data);
  EXPECT_EQ(waits[2].migration_percent(), 40U);
  EXPECT_EQ(waits[2].wait(),
            (shared::Wait{Reason::data_migration, std::chrono::seconds{2}}));
  EXPECT_EQ(waits[2].attempt(), 3U);
  EXPECT_EQ(waits[2].status().attempt, 3);
  for (const auto& progress : waits) {
    EXPECT_EQ(progress.phase(), play::LaunchPhase::finding_server);
    EXPECT_FALSE(progress.step());
    EXPECT_FALSE(progress.measured());
  }
}

TEST_F(CloudLaunchWaits, RetriesAPs5TitleThatIsNotReadyWithATypedReason) {
  Launching ps5{ps5_case()};
  ps5.ps().no_game_refusals = 1;
  EXPECT_TRUE(ps5.core().run());
  EXPECT_EQ(ps5.ps().starts, 2U);
  const auto& typed = ps5.listener().typed;
  const auto retried = first_wait(typed);
  ASSERT_NE(retried, typed.end());
  ASSERT_NE(retried, typed.begin());
  ASSERT_NE(retried + 1, typed.end());
  EXPECT_EQ(waits_of(typed).size(), 1U);
  EXPECT_EQ(retried->activity(), Activity::reserving_server);
  EXPECT_EQ(retried->wait(), again);
  EXPECT_EQ(retried->attempt(), 2U);
  EXPECT_FALSE(retried->step());
  EXPECT_EQ(retried->status(),
            (shared::Status{std::nullopt, 2, retried->elapsed(), again}));
  EXPECT_EQ(step_of(retried[-1]), (Step{3U, 10U}));
  EXPECT_EQ(step_of(retried[1]), (Step{1U, 10U}));
  EXPECT_EQ(std::ranges::count(
                ps5.listener().texts,
                "Starting Session again because the title was not ready"),
            1);
  EXPECT_TRUE(ps5.waiter().waits.empty());
}

TEST_F(CloudLaunchWaits, RetriesAnOldServiceTitleThatIsNotReadyAndResolvesIt) {
  Launching old_service{old_service_case("HELD")};
  old_service.ps().no_game_refusals = 1;
  EXPECT_TRUE(old_service.core().run());
  EXPECT_EQ(old_service.ps().starts, 2U);
  const auto& typed = old_service.listener().typed;
  const auto waits = waits_of(typed);
  ASSERT_EQ(waits.size(), 1U);
  EXPECT_EQ(waits.front().service(), catalog::ServiceGeneration::old_service);
  EXPECT_EQ(waits.front().activity(), Activity::reserving_server);
  EXPECT_EQ(waits.front().wait(), again);
  EXPECT_EQ(waits.front().attempt(), 2U);
  const auto retried = first_wait(typed);
  EXPECT_TRUE(
      std::ranges::any_of(retried, typed.end(), [](const auto& progress) {
        return progress.activity() == Activity::resolving_title;
      }));
}

TEST_F(CloudLaunchWaits, GivesUpOnATitleThatIsStillNotReady) {
  Launching refused{ps5_case()};
  refused.ps().no_game_refusals = 2;
  EXPECT_FALSE(refused.core().run());
  EXPECT_EQ(refused.ps().starts, 2U);
  const auto waits = waits_of(refused.listener().typed);
  ASSERT_EQ(waits.size(), 1U);
  EXPECT_EQ(waits.front().wait(), again);
}

TEST_F(CloudLaunchWaits, DoesNotRetryAnyOtherRefusal) {
  Launching other{ps5_case()};
  other.ps().other_refusals = 1;
  EXPECT_FALSE(other.core().run());
  EXPECT_EQ(other.ps().starts, 1U);
  EXPECT_TRUE(waits_of(other.listener().typed).empty());
}

TEST_F(CloudLaunchWaits, StartsAFreshSessionWhenTheLockIsRefused) {
  Launching relaunch{ps5_case()};
  relaunch.ps().refused_locks = 2;
  EXPECT_TRUE(relaunch.core().run());
  EXPECT_EQ(relaunch.ps().starts, 3U);
  EXPECT_EQ(relaunch.ps().locks, 3U);
  EXPECT_EQ(relaunch.waiter().waits, std::vector<int>({5'000, 5'000}));
  const auto waits = waits_of(relaunch.listener().typed);
  ASSERT_EQ(waits.size(), 2U);
  for (const auto& progress : waits) {
    EXPECT_EQ(progress.activity(), Activity::reserving_server);
    EXPECT_EQ(progress.wait(), (shared::Wait{Reason::connection_retry,
                                             std::chrono::seconds{5}}));
  }
  EXPECT_EQ(waits[0].attempt(), 1U);
  EXPECT_EQ(waits[1].attempt(), 2U);
  EXPECT_TRUE(
      has_text(relaunch, "Lock refused, starting a new session - Attempt 1"));
  EXPECT_TRUE(
      has_text(relaunch, "Lock refused, starting a new session - Attempt 2"));
}

TEST_F(CloudLaunchWaits, GivesUpWhenEveryFreshSessionIsRefused) {
  Launching refused{ps5_case()};
  refused.ps().refused_locks = 100;
  EXPECT_FALSE(refused.core().run());
  EXPECT_EQ(refused.ps().starts, 5U);
  EXPECT_EQ(refused.ps().locks, 5U);
  EXPECT_EQ(refused.waiter().waits, std::vector<int>(4, 5'000));
}

}  // namespace
}  // namespace play_test
