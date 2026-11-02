#include "src/core/users/cloud_relaunch.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/core/cloud/links/scripted_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace cloud_test {
namespace {

using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;

class ReservationSettle : public ::testing::Test {
 protected:
  auto settle() -> libchiaking::Expected<void, gk::Error> {
    return play::ServerReservation::settle(
        client, {waiter, nullptr, observed},
        libchiaking::psn::StreamingService::cloud_stream, relaunch);
  }

  void TearDown() override { EXPECT_EQ(http.calls, 0U); }

  test_support::FakeClock clock{5'000'000'000};
  libchiaking::users::CloudRelaunch relaunch{test_support::process_threading(),
                                             clock};
  RecordingHttp http;
  Gk client{http};
  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  StageProgress observed{FailureStage::cloud_lock};
};

TEST_F(ReservationSettle, GoesStraightOnWithoutARecentStream) {
  EXPECT_TRUE(settle());
  EXPECT_TRUE(waiter.waits.empty());
  EXPECT_TRUE(observed.texts.empty());
}

TEST_F(ReservationSettle, WaitsOutTheRestOfTheGapAfterARecentStream) {
  relaunch.ended();
  clock.now_us += 2'400'000;
  EXPECT_TRUE(settle());
  EXPECT_EQ(waiter.waits, std::vector<int>{17'600});
  EXPECT_EQ(observed.texts, std::vector<std::string>{
                                "Waiting 18 seconds since last cloud stream"});
  ASSERT_FALSE(observed.typed.empty());
  const auto& waiting = observed.typed.front();
  EXPECT_EQ(waiting.activity(), play::LaunchActivity::reserving_server);
  ASSERT_TRUE(waiting.wait());
  EXPECT_EQ(waiting.wait()->reason,
            libchiaking::progress::WaitReason::recent_cloud_session);
  EXPECT_EQ(waiting.wait()->next_try_in, std::chrono::milliseconds{18'000});
  EXPECT_FALSE(waiting.attempt());
}

TEST_F(ReservationSettle, IsCancelledWhenTheWaitIsRefused) {
  relaunch.ended();
  clock.now_us += 2'400'000;
  waiter.answer = false;
  const auto settled = settle();
  ASSERT_FALSE(settled);
  EXPECT_TRUE(settled.error().is_cancelled());
}

}  // namespace
}  // namespace cloud_test
