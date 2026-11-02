#include "src/core/users/cloud_relaunch.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace users_test {
namespace {

using libchiaking::users::CloudRelaunch;
using std::chrono::milliseconds;

class CloudRelaunchSettling : public ::testing::Test {
 protected:
  test_support::FakeClock clock;
  CloudRelaunch relaunch{test_support::process_threading(), clock};
};

TEST_F(CloudRelaunchSettling, LeavesNothingToWaitWithoutAStream) {
  EXPECT_EQ(relaunch.remaining(), milliseconds{0});
}

TEST_F(CloudRelaunchSettling, CountsDownAfterAStreamEnds) {
  relaunch.ended();
  EXPECT_EQ(relaunch.remaining(), CloudRelaunch::settle_time);
  clock.advance_ms(12'500);
  EXPECT_EQ(relaunch.remaining(), milliseconds{7'500});
  clock.advance_ms(7'500);
  EXPECT_EQ(relaunch.remaining(), milliseconds{0});
  clock.advance_ms(60'000);
  EXPECT_EQ(relaunch.remaining(), milliseconds{0});
}

TEST_F(CloudRelaunchSettling, RestartsWhenALaterStreamEnds) {
  relaunch.ended();
  clock.advance_ms(15'000);
  relaunch.ended();
  EXPECT_EQ(relaunch.remaining(), CloudRelaunch::settle_time);
}

}  // namespace
}  // namespace users_test
