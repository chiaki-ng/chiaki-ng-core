#include "src/core/net/deadline_stop_state.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace net_deadline_test {
namespace {

namespace pf = libchiaking::platform;
using libchiaking::net::DeadlineStopState;

class DeadlineStop : public ::testing::Test {
 protected:
  auto deadline_at(pf::StopToken source, std::uint64_t deadline_us)
      -> std::shared_ptr<DeadlineStopState> {
    return std::make_shared<DeadlineStopState>(std::move(source), clock,
                                               deadline_us);
  }

  test_support::FakeClock clock{0};
  std::shared_ptr<test_support::StopState> parent_state =
      std::make_shared<test_support::StopState>();
  pf::StopToken parent{parent_state};
};

TEST_F(DeadlineStop, RunsUntilTheDeadline) {
  auto deadline = deadline_at(parent, 100);
  const pf::StopToken wrapped{deadline};
  clock.now_us = 99;
  EXPECT_FALSE(wrapped.stop_requested());
  EXPECT_FALSE(deadline->timed_out());
}

TEST_F(DeadlineStop, TimesOutAtAndAfterTheDeadline) {
  auto deadline = deadline_at(parent, 100);
  const pf::StopToken wrapped{deadline};
  clock.now_us = 100;
  EXPECT_TRUE(wrapped.stop_requested());
  EXPECT_TRUE(deadline->timed_out());
  clock.now_us = 101;
  EXPECT_TRUE(wrapped.stop_requested());
}

TEST_F(DeadlineStop, StopsWithItsParentWithoutTimingOut) {
  auto deadline = deadline_at(parent, 100);
  const pf::StopToken wrapped{deadline};
  clock.now_us = 101;
  parent_state->stopped = true;
  EXPECT_TRUE(wrapped.stop_requested());
  EXPECT_FALSE(deadline->timed_out());
  clock.now_us = 0;
  EXPECT_TRUE(wrapped.stop_requested());
  parent_state->stopped = false;
  EXPECT_FALSE(wrapped.stop_requested());
}

TEST_F(DeadlineStop, TimesOutWithAnOuterDeadline) {
  auto outer = deadline_at(parent, 50);
  auto inner = deadline_at(pf::StopToken{outer}, 100);
  clock.now_us = 50;
  EXPECT_TRUE(inner->stop_requested());
  EXPECT_TRUE(inner->timed_out());
  EXPECT_TRUE(pf::StopToken{inner}.deadline_expired());
}

TEST_F(DeadlineStop, DoesNotTimeOutWhenTheOuterParentStops) {
  auto outer = deadline_at(parent, 50);
  auto inner = deadline_at(pf::StopToken{outer}, 100);
  clock.now_us = 50;
  parent_state->stopped = true;
  EXPECT_TRUE(inner->stop_requested());
  EXPECT_FALSE(inner->timed_out());
}

TEST_F(DeadlineStop, TimesOutAtOnceWithAnEmptyParentAndZeroDeadline) {
  const pf::StopToken empty_parent;
  const DeadlineStopState empty_state(empty_parent, clock, 0);
  EXPECT_TRUE(empty_state.stop_requested());
  EXPECT_TRUE(empty_state.timed_out());
}

}  // namespace
}  // namespace net_deadline_test
