#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace catalog_api_test {
namespace {

class FetchJoining : public CatalogApiTest {
 protected:
  void SetUp() override {
    ps_as_usual(rig);
    cloud_streaming_allowed(rig);
    rig.http.hold(std::string{old_service_stores});
  }

  auto fetch(Games& into, RecordedFetch& recorded,
             libchiaking::platform::StopToken stop = {})
      -> libchiaking::platform::Thread {
    return started([this, &into, &recorded, stop] {
      into = rig.catalog.process_library_and_list(*user, stop,
                                                  recorded.observer());
    });
  }

  Rig rig;
  std::shared_ptr<users::PsUser> user = rig.user();
};

TEST_F(FetchJoining, ASecondCallerJoinsTheRunningFetch) {
  RecordedFetch leading;
  RecordedFetch joining;
  Games led{libchiaking::unexpected(unanswered())};
  auto joined = led;
  auto leader = fetch(led, leading);
  rig.http.wait_until_held();
  auto joiner = fetch(joined, joining);
  pause_a_moment();
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(joiner.join());
  ASSERT_TRUE(led);
  ASSERT_TRUE(joined);
  EXPECT_EQ(led.value(), joined.value());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  ASSERT_FALSE(joining.states().empty());
  const auto& first = joining.states().front();
  EXPECT_EQ(first.step(), FetchStep::cloud_list);
  EXPECT_EQ(first.activity(), FetchActivity::walking_old_service_shelves);
  for (const auto& state : joining.states()) EXPECT_EQ(state.attempt(), 1U);
  EXPECT_EQ(joining.states().back().step(), FetchStep::ready);
  ASSERT_LE(joining.states().size(), leading.states().size());
  const auto offset = leading.states().size() - joining.states().size();
  for (std::size_t at = 0; at < joining.states().size(); ++at) {
    const auto& joined_state = joining.states()[at];
    const auto& led_state = leading.states()[offset + at];
    EXPECT_EQ(joined_state.step(), led_state.step()) << at;
    EXPECT_EQ(joined_state.activity(), led_state.activity()) << at;
    EXPECT_EQ(joined_state.pages_read(), led_state.pages_read()) << at;
  }
}

TEST_F(FetchJoining, AJoinerThatLeavesIsToldNothingMoreAndTheLeaderGoesOn) {
  libchiaking::platform::StopSource impatient;
  RecordedFetch leading;
  RecordedFetch leaving;
  Games led{libchiaking::unexpected(unanswered())};
  auto left = led;
  auto leader = fetch(led, leading);
  rig.http.wait_until_held();
  auto quitter = fetch(left, leaving, impatient.token());
  pause_a_moment();
  impatient.request_stop();
  EXPECT_TRUE(quitter.join());
  ASSERT_FALSE(left);
  EXPECT_TRUE(left.error().is_cancelled());
  const auto told = leaving.states().size();
  EXPECT_EQ(told, 1U);
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(led);
  EXPECT_EQ(leaving.states().size(), told);
  ASSERT_FALSE(leading.states().empty());
  EXPECT_EQ(leading.states().back().step(), FetchStep::ready);
}

TEST_F(FetchJoining, AJoinerCarriesOnWhenTheLeaderIsCancelled) {
  libchiaking::platform::StopSource leaving;
  RecordedFetch leading;
  RecordedFetch joining;
  Games led{libchiaking::unexpected(unanswered())};
  auto joined = led;
  auto leader = fetch(led, leading, leaving.token());
  rig.http.wait_until_held();
  auto joiner = fetch(joined, joining);
  pause_a_moment();
  leaving.request_stop();
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(joiner.join());
  ASSERT_FALSE(led);
  EXPECT_TRUE(led.error().is_cancelled());
  ASSERT_FALSE(leading.states().empty());
  EXPECT_EQ(leading.states().back().step(), FetchStep::cancelled);
  EXPECT_TRUE(joined);
  EXPECT_TRUE(
      std::ranges::none_of(joining.states(), [](const FetchState& state) {
        return state.step() == FetchStep::cancelled;
      }));
  ASSERT_FALSE(joining.states().empty());
  EXPECT_EQ(joining.states().front().attempt(), 1U);
  EXPECT_EQ(joining.states().back().attempt(), 2U);
  EXPECT_EQ(joining.states().back().step(), FetchStep::ready);
  EXPECT_EQ(rig.catalog.fetch_state(*user).attempt(), 2U);
}

TEST_F(FetchJoining, AStartOverDuringAFetchMakesTheNextCallFetchAfresh) {
  RecordedFetch leading;
  RecordedFetch joining;
  RecordedFetch refreshing;
  Games led{libchiaking::unexpected(unanswered())};
  Games joined = led;
  Games refreshed = led;
  auto leader = fetch(led, leading);
  rig.http.wait_until_held();
  auto joiner = fetch(joined, joining);
  pause_a_moment();
  rig.catalog.start_over(*user);
  auto refresher = fetch(refreshed, refreshing);
  pause_a_moment();
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(joiner.join());
  EXPECT_TRUE(refresher.join());
  ASSERT_TRUE(led);
  ASSERT_TRUE(joined);
  ASSERT_TRUE(refreshed);
  EXPECT_EQ(led.value(), joined.value());
  EXPECT_NE(refreshed.value(), led.value());
  for (const auto& state : leading.states()) EXPECT_EQ(state.attempt(), 1U);
  for (const auto& state : joining.states()) EXPECT_EQ(state.attempt(), 1U);
  ASSERT_FALSE(leading.states().empty());
  ASSERT_FALSE(joining.states().empty());
  ASSERT_FALSE(refreshing.states().empty());
  EXPECT_EQ(leading.states().back().step(), FetchStep::ready);
  EXPECT_EQ(joining.states().back().step(), FetchStep::ready);
  EXPECT_EQ(refreshing.states().front().attempt(), 1U);
  EXPECT_EQ(refreshing.states().back().attempt(), 2U);
  EXPECT_EQ(refreshing.states().back().step(), FetchStep::ready);
  const auto second = std::ranges::find_if(
      refreshing.states(),
      [](const FetchState& state) { return state.attempt() == 2; });
  ASSERT_NE(second, refreshing.states().end());
  EXPECT_EQ(second->step(), FetchStep::checking_sign_in);
  EXPECT_TRUE(std::all_of(
      refreshing.states().begin(), second,
      [](const FetchState& state) { return state.attempt() == 1; }));
  EXPECT_TRUE(std::ranges::none_of(
      second, refreshing.states().end(), [](const FetchState& state) {
        return state.passed().size() > 1 &&
               state.passed()[1].outcome == StepOutcome::held;
      }));
  EXPECT_EQ(rig.http.count(old_service_stores), 2);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_EQ(rig.catalog.fetch_state(*user).attempt(), 2U);
}

TEST_F(FetchJoining, AStartOverCallerThatLeavesWhileWaitingEndsOnlyItsOwnCall) {
  libchiaking::platform::StopSource impatient;
  RecordedFetch leading;
  RecordedFetch leaving;
  Games led{libchiaking::unexpected(unanswered())};
  Games left = led;
  auto leader = fetch(led, leading);
  rig.http.wait_until_held();
  rig.catalog.start_over(*user);
  auto quitter = fetch(left, leaving, impatient.token());
  pause_a_moment();
  impatient.request_stop();
  EXPECT_TRUE(quitter.join());
  ASSERT_FALSE(left);
  EXPECT_TRUE(left.error().is_cancelled());
  const auto told = leaving.states().size();
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(led);
  ASSERT_FALSE(leading.states().empty());
  EXPECT_EQ(leading.states().back().step(), FetchStep::ready);
  EXPECT_EQ(leaving.states().size(), told);
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_TRUE(rig.catalog.process_library_and_list(*user));
  EXPECT_EQ(rig.http.count(old_service_stores), 2);
  EXPECT_EQ(rig.catalog.fetch_state(*user).attempt(), 2U);
}

TEST_F(FetchJoining, AStartOverCallerCancelledWhileItLeadsEndsOnlyItsOwnCall) {
  libchiaking::platform::StopSource impatient;
  RecordedFetch leading;
  std::vector<FetchState> leaving;
  const catalog::FetchObserver stop_on_its_own_attempt =
      [&](const FetchState& state) {
        leaving.push_back(state);
        if (state.attempt() == 2) impatient.request_stop();
      };
  Games led{libchiaking::unexpected(unanswered())};
  Games left = led;
  auto leader = fetch(led, leading);
  rig.http.wait_until_held();
  rig.catalog.start_over(*user);
  auto quitter = started([&] {
    left = rig.catalog.process_library_and_list(*user, impatient.token(),
                                                stop_on_its_own_attempt);
  });
  pause_a_moment();
  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(quitter.join());
  EXPECT_TRUE(led);
  ASSERT_FALSE(leading.states().empty());
  EXPECT_EQ(leading.states().back().attempt(), 1U);
  EXPECT_EQ(leading.states().back().step(), FetchStep::ready);
  ASSERT_FALSE(left);
  EXPECT_TRUE(left.error().is_cancelled());
  ASSERT_FALSE(leaving.empty());
  EXPECT_EQ(leaving.back().attempt(), 2U);
  EXPECT_EQ(leaving.back().step(), FetchStep::cancelled);
  EXPECT_EQ(rig.catalog.fetch_state(*user).step(), FetchStep::cancelled);
  const auto again = rig.catalog.process_library_and_list(*user);
  EXPECT_TRUE(again);
  EXPECT_EQ(rig.catalog.fetch_state(*user).attempt(), 3U);
}

}  // namespace
}  // namespace catalog_api_test
