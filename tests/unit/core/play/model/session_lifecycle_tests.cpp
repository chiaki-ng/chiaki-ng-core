#include "src/core/play/model/session_lifecycle.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using Lifecycle = libchiaking::play::SessionLifecycle;
using Phase = Lifecycle::Phase;

class SessionLifecycle : public ::testing::Test {
 protected:
  static auto fresh() -> Lifecycle { return Lifecycle{}; }
};

TEST_F(SessionLifecycle, AcceptsNothingUntilItRuns) {
  auto lifecycle = fresh();
  EXPECT_EQ(lifecycle.phase(), Phase::created);
  EXPECT_FALSE(lifecycle.accepting());
  EXPECT_FALSE(lifecycle.cancelled());
  EXPECT_FALSE(lifecycle.sleeping());
  EXPECT_FALSE(lifecycle.sleep());
  EXPECT_TRUE(lifecycle.begin());
  EXPECT_EQ(lifecycle.phase(), Phase::running);
  EXPECT_TRUE(lifecycle.accepting());
  EXPECT_FALSE(lifecycle.begin());
  lifecycle.finish();
  EXPECT_EQ(lifecycle.phase(), Phase::finished);
  EXPECT_FALSE(lifecycle.accepting());
  EXPECT_FALSE(lifecycle.begin());
  lifecycle.finish();
  EXPECT_EQ(lifecycle.phase(), Phase::finished);
}

TEST_F(SessionLifecycle, FinishedBeforeItRanNeverRuns) {
  auto lifecycle = fresh();
  lifecycle.finish();
  EXPECT_EQ(lifecycle.phase(), Phase::finished);
  EXPECT_FALSE(lifecycle.begin());
}

TEST_F(SessionLifecycle, StopsAcceptingAndRulesOutSleepWhenLeft) {
  auto lifecycle = fresh();
  lifecycle.cancel();
  EXPECT_TRUE(lifecycle.cancelled());
  EXPECT_FALSE(lifecycle.sleeping());
  EXPECT_TRUE(lifecycle.begin());
  EXPECT_FALSE(lifecycle.accepting());
  EXPECT_FALSE(lifecycle.sleep());
  EXPECT_FALSE(lifecycle.sleeping());
  lifecycle.cancel();
  EXPECT_TRUE(lifecycle.cancelled());
  EXPECT_FALSE(lifecycle.sleeping());
}

TEST_F(SessionLifecycle, SleepsOnlyWhileRunning) {
  auto lifecycle = fresh();
  ASSERT_TRUE(lifecycle.begin());
  EXPECT_TRUE(lifecycle.sleep());
  EXPECT_TRUE(lifecycle.sleeping());
  EXPECT_TRUE(lifecycle.cancelled());
  EXPECT_FALSE(lifecycle.accepting());
  EXPECT_FALSE(lifecycle.sleep());
  lifecycle.cancel();
  EXPECT_TRUE(lifecycle.sleeping());
  lifecycle.conclude(false);
  EXPECT_TRUE(lifecycle.sleeping());
  lifecycle.finish();
  EXPECT_FALSE(lifecycle.sleep());
}

TEST_F(SessionLifecycle, CannotSleepOnceFinished) {
  auto lifecycle = fresh();
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.finish();
  EXPECT_FALSE(lifecycle.sleep());
  EXPECT_FALSE(lifecycle.sleeping());
  EXPECT_FALSE(lifecycle.cancelled());
}

TEST_F(SessionLifecycle, ConcludesARestByLeavingOrSleeping) {
  auto staying = fresh();
  staying.conclude(false);
  EXPECT_TRUE(staying.cancelled());
  EXPECT_FALSE(staying.sleeping());
  staying.conclude(true);
  EXPECT_TRUE(staying.sleeping());

  auto resting = fresh();
  ASSERT_TRUE(resting.begin());
  resting.conclude(true);
  EXPECT_TRUE(resting.cancelled());
  EXPECT_TRUE(resting.sleeping());
  EXPECT_FALSE(resting.accepting());
  resting.conclude(false);
  EXPECT_TRUE(resting.sleeping());

  auto left = fresh();
  left.cancel();
  left.conclude(false);
  EXPECT_TRUE(left.cancelled());
  EXPECT_FALSE(left.sleeping());
}

}  // namespace
}  // namespace play_test
