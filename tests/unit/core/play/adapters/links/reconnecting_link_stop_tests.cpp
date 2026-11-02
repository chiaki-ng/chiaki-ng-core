#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/reconnecting_link_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace play_test {
namespace {

namespace rp = libchiaking::net::protocol::remote_play;
using reconnecting_links::Rig;

class ReconnectingLinkResting : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.link().start());
    rig_.current().fail(rp::StreamError::ctrl_timeout);
    ASSERT_TRUE(rig_.link().pump_once());
  }

  auto rig() -> Rig& { return rig_; }

 private:
  Rig rig_;
};

TEST_F(ReconnectingLinkResting, HaltsOnceWhenStopped) {
  const auto before = rig().sink().states().size();
  EXPECT_TRUE(rig().link().stop());
  EXPECT_EQ(rig().sink().states().size(), before + 1);
  ASSERT_FALSE(rig().sink().states().empty());
  EXPECT_EQ(rig().sink().states().back(), rp::StreamState::stopped);
  EXPECT_EQ(rig().link().state(), rp::StreamState::stopped);
  EXPECT_TRUE(rig().link().stop_for_sleep());
  EXPECT_EQ(rig().sink().states().size(), before + 1);
  const auto pumped = rig().link().pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), rp::StreamError::invalid_state);
  EXPECT_FALSE(rig().link().report_video_loss(1, 2));
}

TEST_F(ReconnectingLinkResting, HaltsOnTheNextPumpWhenAStopIsRequested) {
  rig().stop().request_stop();
  rig().clock().advance(std::chrono::seconds{5});
  EXPECT_TRUE(rig().link().pump_once());
  EXPECT_EQ(rig().script().opened(), 1U);
  EXPECT_EQ(rig().link().state(), rp::StreamState::stopped);
  ASSERT_FALSE(rig().sink().states().empty());
  EXPECT_EQ(rig().sink().states().back(), rp::StreamState::stopped);
}

TEST(ReconnectingLinkStop, HaltsBeforeTheStart) {
  Rig rig;
  EXPECT_TRUE(rig.link().stop());
  EXPECT_EQ(rig.sink().states(), std::vector{rp::StreamState::stopped});
  EXPECT_EQ(rig.link().state(), rp::StreamState::stopped);
  const auto started = rig.link().start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), rp::StreamError::invalid_state);
}

TEST(ReconnectingLinkStop, KeepsTheLastStateOfTheAttemptItEnds) {
  Rig rig;
  ASSERT_TRUE(rig.link().start());
  rig.current().stream();
  EXPECT_TRUE(rig.link().stop());
  EXPECT_EQ(rig.script().stopped(), 1U);
  EXPECT_EQ(rig.script().destroyed(), 1U);
  EXPECT_EQ(rig.link().state(), rp::StreamState::stopped);
  EXPECT_EQ(rig.link().diagnostics().video_units, 42U);
  EXPECT_EQ(rig.link().takion_version(), 20U);
  const auto events = rig.sink().events();
  EXPECT_TRUE(rig.link().stop());
  EXPECT_EQ(rig.sink().events(), events);
  const auto pumped = rig.link().pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), rp::StreamError::invalid_state);
}

}  // namespace
}  // namespace play_test
