#include "src/core/catalog/adapters/fetch_board.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace catalog_api_test {
namespace {

using FetchProgress = CatalogApiTest;

TEST_F(FetchProgress, ReportsEveryStepOfAColdFetchInOrder) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  cloud_streaming_allowed(rig);
  EXPECT_EQ(rig.catalog.fetch_state(*user).step(), FetchStep::idle);
  RecordedFetch recorded;
  const auto games =
      rig.catalog.process_library_and_list(*user, {}, recorded.observer());
  ASSERT_TRUE(games);
  const auto count = recorded.states().size();
  EXPECT_EQ(recorded.steps(), every_step);
  for (const auto& state : recorded.states()) EXPECT_EQ(state.attempt(), 1U);
  ASSERT_FALSE(recorded.states().empty());
  const auto& last = recorded.states().back();
  EXPECT_FALSE(last.is_running());
  EXPECT_FALSE(last.failure());
  EXPECT_EQ(last.passed().size(), every_step.size() - 1);
  EXPECT_EQ(recorded.outcome_of(FetchStep::checking_sign_in),
            StepOutcome::held);
  EXPECT_EQ(recorded.outcome_of(FetchStep::cloud_list), StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::owned_library),
            StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::premium_category),
            StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::streaming_access),
            StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::matching), StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::trusted_time), StepOutcome::skipped);
  EXPECT_EQ(recorded.outcome_of(FetchStep::exclusions), StepOutcome::fetched);
  EXPECT_FALSE(recorded.did(FetchActivity::reusing_held_answer));
  EXPECT_TRUE(recorded.did(FetchActivity::walking_public_store));
  EXPECT_TRUE(recorded.did(FetchActivity::walking_old_service_shelves));
  EXPECT_TRUE(recorded.did(FetchActivity::reading_entitlements));
  EXPECT_TRUE(recorded.did(FetchActivity::scanning_ps5_library));
  EXPECT_TRUE(recorded.did(FetchActivity::reading_category_pages));
  EXPECT_TRUE(recorded.did(FetchActivity::reading_exclusions));
  EXPECT_TRUE(
      std::ranges::any_of(recorded.states(), [](const FetchState& state) {
        return state.activity() == FetchActivity::walking_old_service_shelves &&
               state.pages_read() > 0 && state.pages_expected() &&
               state.status().position;
      }));
  const auto now = rig.catalog.fetch_state(*user);
  EXPECT_EQ(now.step(), FetchStep::ready);
  EXPECT_EQ(now.attempt(), 1U);
  EXPECT_EQ(recorded.states().size(), count);
}

TEST_F(FetchProgress, ReportsHeldAnswersAsHeld) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  cloud_streaming_allowed(rig);
  EXPECT_TRUE(rig.catalog.process_library_and_list(*user));
  rig.advance(std::chrono::minutes{10});
  const auto asked = rig.http.requests();
  RecordedFetch recorded;
  EXPECT_TRUE(
      rig.catalog.process_library_and_list(*user, {}, recorded.observer()));
  EXPECT_EQ(recorded.steps(), every_step);
  for (const auto& state : recorded.states()) EXPECT_EQ(state.attempt(), 2U);
  for (const auto step :
       {FetchStep::cloud_list, FetchStep::owned_library,
        FetchStep::premium_category, FetchStep::for_sale_category,
        FetchStep::streaming_access})
    EXPECT_EQ(recorded.outcome_of(step), StepOutcome::held);
  EXPECT_TRUE(recorded.did(FetchActivity::reusing_held_answer));
  EXPECT_FALSE(recorded.did(FetchActivity::walking_old_service_shelves));
  EXPECT_EQ(rig.http.requests(), asked);
}

TEST_F(FetchProgress, FollowsAFailedShelfWalkWithThePublicStoreWalk) {
  Rig rig;
  const auto user = rig.user();
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}),
                  store_list({comet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  premium_includes(rig, {premium_offer(comet, "Comet")});
  cloud_streaming_allowed(rig);
  RecordedFetch recorded;
  EXPECT_TRUE(
      rig.catalog.process_library_and_list(*user, {}, recorded.observer()));
  const auto activities = recorded.activities();
  const auto at = [&](FetchActivity activity) {
    return std::ranges::find(activities, activity) - activities.begin();
  };
  EXPECT_LT(at(FetchActivity::walking_public_store),
            at(FetchActivity::walking_old_service_shelves));
  EXPECT_LT(at(FetchActivity::walking_old_service_shelves),
            at(FetchActivity::walking_public_store_fallback));
  EXPECT_LT(at(FetchActivity::walking_public_store_fallback),
            at(FetchActivity::reading_ps5_store_lists));
  ASSERT_FALSE(recorded.states().empty());
  EXPECT_EQ(recorded.states().back().step(), FetchStep::ready);
}

TEST_F(FetchProgress, DegradesTheStepOfAFailingLibraryAndGoesOn) {
  Rig rig;
  const auto user = rig.user();
  old_service_lists(rig);
  ps5_store_lists(rig, store_list({comet_listed, rivet_listed}),
                  store_list({comet_listed}));
  library_holds(rig, owned_page({rivet_owned}));
  rig.http.on("GET", std::string{owned_list}, {{500, "{}"}});
  rig.http.on("GET", std::string{ps5_owned_list}, {{500, "{}"}});
  premium_includes(rig, {premium_offer(comet, "Comet")});
  cloud_streaming_allowed(rig);
  RecordedFetch recorded;
  EXPECT_TRUE(
      rig.catalog.process_library_and_list(*user, {}, recorded.observer()));
  EXPECT_EQ(recorded.outcome_of(FetchStep::cloud_list), StepOutcome::fetched);
  EXPECT_EQ(recorded.outcome_of(FetchStep::owned_library),
            StepOutcome::degraded);
  EXPECT_EQ(recorded.steps(), every_step);
}

TEST_F(FetchProgress, EndsUnansweredForAUserWhoIsNotSignedIn) {
  Rig rig;
  ps_as_usual(rig);
  const auto home = rig.home_user();
  RecordedFetch recorded;
  const auto refused =
      rig.catalog.process_library_and_list(*home, {}, recorded.observer());
  ASSERT_FALSE(refused);
  EXPECT_TRUE(refused.error().needs_sign_in());
  EXPECT_EQ(recorded.steps(),
            (std::vector{FetchStep::checking_sign_in, FetchStep::unanswered}));
  ASSERT_FALSE(recorded.states().empty());
  const auto& last = recorded.states().back();
  EXPECT_TRUE(last.passed().empty());
  ASSERT_TRUE(last.failure());
  EXPECT_TRUE(last.failure()->needs_sign_in());
  EXPECT_EQ(rig.catalog.fetch_state(*home).step(), FetchStep::unanswered);
}

TEST_F(FetchProgress, EndsCancelledAndStartingOverReturnsItToIdle) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  cloud_streaming_allowed(rig);
  rig.http.hold(std::string{old_service_stores});
  libchiaking::platform::StopSource stop;
  RecordedFetch recorded;
  Games ended{libchiaking::unexpected(unanswered())};
  auto fetching = started([&] {
    ended = rig.catalog.process_library_and_list(*user, stop.token(),
                                                 recorded.observer());
  });
  rig.http.wait_until_held();
  const auto running = rig.catalog.fetch_state(*user);
  EXPECT_TRUE(running.is_running());
  EXPECT_EQ(running.step(), FetchStep::cloud_list);
  EXPECT_EQ(running.activity(), FetchActivity::walking_old_service_shelves);
  stop.request_stop();
  rig.http.release();
  EXPECT_TRUE(fetching.join());
  ASSERT_FALSE(ended);
  EXPECT_TRUE(ended.error().is_cancelled());
  ASSERT_FALSE(recorded.states().empty());
  EXPECT_EQ(recorded.states().back().step(), FetchStep::cancelled);
  EXPECT_EQ(rig.catalog.fetch_state(*user).step(), FetchStep::cancelled);
  rig.catalog.start_over(*user);
  const auto reset = rig.catalog.fetch_state(*user);
  EXPECT_EQ(reset.step(), FetchStep::idle);
  EXPECT_TRUE(reset.passed().empty());
  EXPECT_TRUE(rig.catalog.process_library_and_list(*user));
  EXPECT_EQ(rig.catalog.fetch_state(*user).attempt(), 2U);
}

TEST_F(FetchProgress, AnObserverThatStartsOverDoesNotStallTheFetch) {
  auto rig = std::make_unique<Rig>();
  auto user = rig->user();
  ps_as_usual(*rig);
  cloud_streaming_allowed(*rig);
  bool started_over = false;
  const catalog::FetchObserver observer = [&](const FetchState&) {
    if (std::exchange(started_over, true)) return;
    rig->catalog.start_over(*user);
  };
  std::optional<Games> answer;
  std::exception_ptr thrown;
  auto fetching = started([&] {
    try {
      answer.emplace(
          rig->catalog.process_library_and_list(*user, {}, observer));
    } catch (...) {
      thrown = std::current_exception();
    }
  });
  if (!fetching.join_for(std::chrono::seconds{2}).value_or(false)) {
    static_cast<void>(
        std::make_unique<libchiaking::platform::Thread>(std::move(fetching))
            .release());
    static_cast<void>(rig.release());
    FAIL() << "the fetch is still blocked two seconds after its observer "
              "called Catalog::start_over";
  }
  EXPECT_TRUE(started_over);
  EXPECT_FALSE(thrown);
  EXPECT_TRUE(answer.has_value());
}

TEST_F(FetchProgress, AThrowingObserverDoesNotSilenceTheBoard) {
  catalog::FetchBoard board{shared_threading, steady_clock};
  bool threw = false;
  const catalog::FetchObserver throwing = [&](const FetchState&) {
    if (!std::exchange(threw, true)) throw std::runtime_error{"observer"};
  };
  RecordedFetch recorded;
  const auto leader = board.watch(throwing);
  const auto follower = board.watch(recorded.observer());
  EXPECT_THROW(static_cast<void>(board.begin(leader)), std::runtime_error);
  board.ended(1, StepOutcome::held);
  board.began(1, FetchStep::cloud_list, std::nullopt);
  ASSERT_FALSE(recorded.states().empty());
  EXPECT_EQ(recorded.states().back().step(), FetchStep::cloud_list);
  EXPECT_EQ(board.state().step(), FetchStep::cloud_list);
}

TEST_F(FetchProgress, AnObserverReadsTheStateItIsTold) {
  catalog::FetchBoard board{shared_threading, steady_clock};
  const catalog::FetchObserver starting_over = [&](const FetchState& state) {
    if (state.step() == FetchStep::unanswered) board.start_over();
  };
  std::vector<std::pair<FetchStep, FetchStep>> seen;
  const catalog::FetchObserver reading = [&](const FetchState& state) {
    seen.emplace_back(state.step(), board.state().step());
  };
  const auto leader = board.watch(starting_over);
  const auto follower = board.watch(reading);
  ASSERT_TRUE(board.begin(leader));
  board.failed(1,
               catalog::Unanswered{catalog::Unanswered::Reason::unavailable});
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back().first, FetchStep::unanswered);
  for (const auto& [told, read] : seen) EXPECT_EQ(read, told);
  EXPECT_EQ(board.state().step(), FetchStep::idle);
}

TEST_F(FetchProgress, AWatchDroppedDuringATurnWaitsAndHearsNothingAfter) {
  catalog::FetchBoard board{shared_threading, steady_clock};
  const auto pause = [this] {
    libchiaking::platform::Sleeper{shared_threading, steady_clock}.sleep_for(
        std::chrono::milliseconds{1});
  };
  std::atomic<bool> telling{false};
  std::atomic<bool> released{false};
  const catalog::FetchObserver holding = [&](const FetchState&) {
    if (telling.exchange(true)) return;
    while (!released.load()) pause();
  };
  std::atomic<int> heard{0};
  const catalog::FetchObserver counting = [&](const FetchState&) {
    heard.fetch_add(1);
  };
  const auto leader = board.watch(holding);
  std::atomic<bool> watching{false};
  std::atomic<bool> dropping{false};
  std::atomic<int> heard_when_dropped{-1};
  auto follower = started([&] {
    {
      const auto watch = board.watch(counting);
      watching = true;
      while (!dropping.load()) pause();
    }
    heard_when_dropped = heard.load();
  });
  while (!watching.load()) pause();
  auto fetching = started([&] { static_cast<void>(board.begin(leader)); });
  while (!telling.load()) pause();
  dropping = true;
  EXPECT_FALSE(
      follower.join_for(std::chrono::milliseconds{100}).value_or(true));
  released = true;
  ASSERT_TRUE(fetching.join_for(std::chrono::seconds{2}).value_or(false));
  ASSERT_TRUE(follower.join_for(std::chrono::seconds{2}).value_or(false));
  EXPECT_EQ(heard.load(), heard_when_dropped.load());
  const auto before = heard.load();
  board.ended(1, StepOutcome::held);
  board.began(1, FetchStep::cloud_list, std::nullopt);
  EXPECT_EQ(heard.load(), before);
  EXPECT_EQ(board.state().step(), FetchStep::cloud_list);
}

TEST_F(FetchProgress, AWatchWhoseFirstTellThrowsIsNotKept) {
  catalog::FetchBoard board{shared_threading, steady_clock};
  const catalog::FetchObserver quiet;
  const auto leader = board.watch(quiet);
  ASSERT_TRUE(board.begin(leader));
  int told = 0;
  const catalog::FetchObserver throwing = [&](const FetchState&) {
    ++told;
    throw std::runtime_error{"observer"};
  };
  EXPECT_THROW(static_cast<void>(board.watch(throwing)), std::runtime_error);
  EXPECT_EQ(told, 1);
  board.ended(1, StepOutcome::held);
  board.began(1, FetchStep::cloud_list, std::nullopt);
  EXPECT_EQ(told, 1);
  EXPECT_EQ(board.state().step(), FetchStep::cloud_list);
}

class FetchProgressDeathTest : public CatalogApiTest {
 protected:
  void SetUp() override {
    CatalogApiTest::SetUp();
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
};

TEST_F(FetchProgressDeathTest, AnObserverThatWatchesEndsTheProcess) {
  const auto reenter = [this] {
    std::set_terminate([] { std::_Exit(EXIT_SUCCESS); });
    catalog::FetchBoard board{shared_threading, steady_clock};
    const catalog::FetchObserver quiet;
    const catalog::FetchObserver watching = [&](const FetchState&) {
      static_cast<void>(board.watch(quiet));
    };
    const auto leader = board.watch(watching);
    static_cast<void>(board.begin(leader));
    std::_Exit(EXIT_FAILURE);
  };
  EXPECT_EXIT(reenter(), ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST_F(FetchProgressDeathTest, AnObserverThatBeginsEndsTheProcess) {
  const auto reenter = [this] {
    std::set_terminate([] { std::_Exit(EXIT_SUCCESS); });
    catalog::FetchBoard board{shared_threading, steady_clock};
    const catalog::FetchBoard::Watch* leading = nullptr;
    const catalog::FetchObserver reentering = [&](const FetchState&) {
      static_cast<void>(board.begin(*leading));
    };
    const auto leader = board.watch(reentering);
    leading = &leader;
    static_cast<void>(board.begin(leader));
    std::_Exit(EXIT_FAILURE);
  };
  EXPECT_EXIT(reenter(), ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST_F(FetchProgressDeathTest, AnObserverThatFollowsAgainEndsTheProcess) {
  const auto reenter = [this] {
    std::set_terminate([] { std::_Exit(EXIT_SUCCESS); });
    catalog::FetchBoard board{shared_threading, steady_clock};
    const catalog::FetchBoard::Watch* leading = nullptr;
    const catalog::FetchObserver reentering = [&](const FetchState&) {
      board.follow_again(*leading);
    };
    const auto leader = board.watch(reentering);
    leading = &leader;
    static_cast<void>(board.begin(leader));
    std::_Exit(EXIT_FAILURE);
  };
  EXPECT_EXIT(reenter(), ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST_F(FetchProgressDeathTest, AnObserverThatCatchesUpEndsTheProcess) {
  const auto reenter = [this] {
    std::set_terminate([] { std::_Exit(EXIT_SUCCESS); });
    catalog::FetchBoard board{shared_threading, steady_clock};
    const catalog::FetchBoard::Watch* leading = nullptr;
    const catalog::FetchObserver reentering = [&](const FetchState&) {
      board.catch_up(*leading, 1);
    };
    const auto leader = board.watch(reentering);
    leading = &leader;
    static_cast<void>(board.begin(leader));
    std::_Exit(EXIT_FAILURE);
  };
  EXPECT_EXIT(reenter(), ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

}  // namespace
}  // namespace catalog_api_test
