#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace progress = libchiaking::progress;
using progress::Status;
using progress::Wait;
using progress::WaitReason;
using Route = progress::InternetRoute;

class ConnectProgress : public ::testing::Test {
 protected:
  static auto reconnecting() -> play::ConnectProgress {
    return play::ConnectProgress{play::ConnectStep::waking_console,
                                 std::chrono::microseconds{5}, 2, true};
  }
};

TEST_F(ConnectProgress, KeepsItsOldFields) {
  const auto old = reconnecting();
  EXPECT_EQ(old.step, play::ConnectStep::waking_console);
  EXPECT_EQ(old.elapsed, std::chrono::microseconds{5});
  EXPECT_EQ(old.attempt, 2u);
  EXPECT_TRUE(old.reconnecting);
  EXPECT_FALSE(old.wait);
  EXPECT_FALSE(old.previous_failure);
  EXPECT_FALSE(old.route);
  EXPECT_EQ(old.status(), (Status{std::nullopt, 2, std::chrono::microseconds{5},
                                  std::nullopt}));
}

TEST_F(ConnectProgress, StartsByFindingTheConsole) {
  const play::ConnectProgress fresh;
  EXPECT_EQ(fresh.step, play::ConnectStep::finding_console);
  EXPECT_EQ(fresh.attempt, 1u);
  EXPECT_FALSE(fresh.reconnecting);
  EXPECT_EQ(fresh.status(), Status{.elapsed = std::chrono::microseconds{0}});
}

TEST_F(ConnectProgress, CarriesTheWaitAndThePreviousFailure) {
  const auto old = reconnecting();
  auto waking = old;
  waking.wait = Wait{WaitReason::console_waking, std::chrono::seconds{2}};
  waking.previous_failure = play::AttemptFailure{
      play::EndReason::Cause::control_timeout, std::nullopt, false};
  EXPECT_NE(waking, old);
  EXPECT_TRUE(waking.status().is_waiting());
  ASSERT_TRUE(waking.status().wait);
  EXPECT_EQ(waking.status().wait->next_try_in, std::chrono::milliseconds{2000});
  ASSERT_TRUE(waking.previous_failure);
  EXPECT_EQ(waking.previous_failure->cause,
            play::EndReason::Cause::control_timeout);
  EXPECT_FALSE(waking.previous_failure->console_quit);
  const play::AttemptFailure quit{
      std::nullopt,
      play::ConsoleQuit::Reason::session_request_connection_refused};
  EXPECT_NE(quit, *waking.previous_failure);
  EXPECT_FALSE(quit.cause);
}

TEST_F(ConnectProgress, CarriesTheRouteWithoutChangingTheStatus) {
  const play::ConnectProgress fresh;
  auto routed = fresh;
  routed.step = play::ConnectStep::opening_stream_route;
  routed.route = Route{Route::Step::testing_candidates, Route::Purpose::stream,
                       Route::Candidates{2, 1, 75}};
  ASSERT_TRUE(routed.route);
  ASSERT_TRUE(routed.route->candidates);
  EXPECT_EQ(routed.route->candidates->total(), 78u);
  EXPECT_FALSE(routed.route->chosen);
  EXPECT_NE(routed, fresh);
  EXPECT_EQ(routed.status(), fresh.status());
}

}  // namespace
}  // namespace play_test
