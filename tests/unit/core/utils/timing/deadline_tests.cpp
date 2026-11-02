#include "src/core/utils/timing/deadline.hpp"
#include "src/core/utils/timing/saturating.hpp"

#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace timing_test {
namespace {

using libchiaking::timing::Deadline;
using namespace std::chrono_literals;

class Deadlines : public ::testing::Test {
 protected:
  static constexpr auto latest = std::numeric_limits<std::uint64_t>::max();
};

TEST_F(Deadlines, SaturateDurationsAtTheirBounds) {
  using libchiaking::timing::saturating_add;
  static_assert(
      saturating_add(Deadline::Duration{latest - 1}, Deadline::Duration{5}) ==
      Deadline::Duration::max());
  static_assert(
      saturating_add(Deadline::Duration{2}, Deadline::Duration{3}).count() ==
      5);
  static_assert(saturating_add(std::chrono::microseconds::max(), 1us) ==
                std::chrono::microseconds::max());
  static_assert(saturating_add(std::chrono::microseconds::min(), -1us) ==
                std::chrono::microseconds::min());
  static_assert(saturating_add(10us, -4us) == 6us);
}

TEST_F(Deadlines, NeverMeasureElapsedTimeBackwards) {
  using libchiaking::timing::elapsed;
  static_assert(elapsed(3ms, 5ms) == 2ms);
  static_assert(elapsed(5ms, 3ms) == 0ms);
  static_assert(
      elapsed(Deadline::instant_of(10), Deadline::instant_of(25)).count() ==
      15);
  static_assert(Deadline::elapsed_us(40, 30) == 0);
  static_assert(Deadline::elapsed_us(30, 40) == 10);
}

TEST_F(Deadlines, ExpireOnceTheIntervalIsReached) {
  libchiaking::testing::ManualClock clock;
  const auto deadline = Deadline::after(clock, Deadline::Duration{1'000});
  EXPECT_FALSE(deadline.expired(clock));
  EXPECT_EQ(deadline.remaining(Deadline::now(clock)),
            Deadline::Duration{1'000});
  clock.advance(1ms);
  EXPECT_TRUE(deadline.expired(clock));
  EXPECT_EQ(deadline.remaining(Deadline::now(clock)),
            Deadline::Duration::zero());
  EXPECT_EQ(deadline.at_us(), 5'001'000U);
}

TEST_F(Deadlines, SaturateNearTheEndOfTime) {
  static_assert(Deadline::after_us(latest - 2, 10) == latest);
  static_assert(Deadline::after_us(7, 0) == 7);
  static_assert(
      Deadline::after(Deadline::instant_of(latest), Deadline::Duration{1})
          .at_us() == latest);
}

TEST_F(Deadlines, NeverExpireWhenUnset) {
  constexpr Deadline never;
  static_assert(!never.expired(Deadline::instant_of(latest)));
  static_assert(!never.at_us() && !never.remaining(Deadline::instant_of(0)));
  static_assert(!Deadline::expired(std::nullopt, latest));
  static_assert(Deadline::expired(std::optional<std::uint64_t>{4}, 4));
  static_assert(!Deadline::expired(std::optional<std::uint64_t>{5}, 4));
}

TEST_F(Deadlines, LetTheEarliestWin) {
  static_assert(Deadline::earliest(std::nullopt, 9) == 9U);
  static_assert(Deadline::earliest(3, std::nullopt) == 3U);
  static_assert(Deadline::earliest(8, 3) == 3U);
  static_assert(!Deadline::earliest(std::nullopt, std::nullopt));

  Deadline soonest;
  soonest.consider(std::optional<std::uint64_t>{});
  EXPECT_FALSE(soonest.at_us());
  soonest.consider(std::uint64_t{40});
  soonest.consider(Deadline{Deadline::instant_of(70)});
  soonest.consider(std::optional<std::uint64_t>{20});
  soonest.consider(Deadline{});
  EXPECT_EQ(soonest.at_us(), 20U);
  EXPECT_EQ(soonest, Deadline{Deadline::instant_of(20)});
}

}  // namespace
}  // namespace timing_test
