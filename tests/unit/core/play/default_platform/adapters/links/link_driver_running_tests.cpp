#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using Rig = LinkDriverRig;
using RealStream = Rig::RealStream;
using StreamError = Rig::StreamError;
using StreamState = Rig::StreamState;
using api_test::LoopbackListener;
using remote_play::QuitReason;
using namespace std::chrono_literals;

class LinkDriverWake : public ::testing::Test {
 protected:
  static void interrupts_a_wait(RealStream& opened) {
    auto waker = LiveProviders::spawn("test", [&opened] {
      LiveProviders::nap(50ms);
      opened->wake();
    });
    const auto started = LiveProviders::steady_us();
    EXPECT_TRUE(opened->wait(5000ms));
    const auto elapsed = Rig::since(started);
    static_cast<void>(waker.join());
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LT(elapsed, 2s);
    const auto again = LiveProviders::steady_us();
    EXPECT_TRUE(opened->wait(60ms));
    EXPECT_GE(Rig::since(again), 50ms);
    EXPECT_EQ(opened->state(), StreamState::idle);
  }
};

TEST_F(LinkDriverWake, InterruptsACloudSessionsWait) {
  RealStream cloud{RealStream::cloud()};
  interrupts_a_wait(cloud);
}

TEST_F(LinkDriverWake, InterruptsALocalSessionsWait) {
  RealStream local{RealStream::local()};
  interrupts_a_wait(local);
}

TEST_F(LinkDriverWake, InterruptsARemoteSessionsWait) {
  RealStream remote{RealStream::remote()};
  interrupts_a_wait(remote);
}

class LinkDriverRunningLocal : public ::testing::Test {
 protected:
  auto drained() -> std::vector<std::string> {
    std::vector<std::string> lines;
    for (const auto& record : opened_.drain_log())
      lines.emplace_back(record.message());
    return lines;
  }

  static auto mentions(const std::vector<std::string>& lines,
                       std::string_view text) -> bool {
    return std::ranges::any_of(lines, [text](const std::string& line) {
      return line.find(text) != std::string::npos;
    });
  }

  LoopbackListener listener_;
  RealStream opened_{RealStream::local(listener_.port())};
};

TEST_F(LinkDriverRunningLocal, ReportsItsStateAndStops) {
  ASSERT_TRUE(opened_->start());
  opened_.pump_for(100ms);
  EXPECT_EQ(opened_->state(), StreamState::establishing_route);
  EXPECT_GE(opened_.events.state_events, 1U);
  ASSERT_FALSE(opened_.events.states.empty());
  EXPECT_EQ(opened_.events.states.front().state,
            StreamState::establishing_route);
  EXPECT_FALSE(opened_->failure());
  EXPECT_FALSE(opened_->diagnostics().available);
  EXPECT_TRUE(Rig::refused(opened_->start(), StreamError::invalid_state));
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(opened_->state(), StreamState::stopped);
  EXPECT_EQ(opened_.events.states.back().state, StreamState::stopped);
  EXPECT_EQ(opened_.events.states.back().quit_reason, QuitReason::stopped);
  EXPECT_EQ(opened_->quit().reason, QuitReason::stopped);
  const auto events = opened_.events.state_events;
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(opened_.events.state_events, events);
  EXPECT_TRUE(Rig::refused(opened_->pump_once(), StreamError::invalid_state));
  const auto lines = drained();
  EXPECT_TRUE(mentions(lines, "Starting local connection"));
  EXPECT_TRUE(mentions(lines, "session start succeeded"));
  EXPECT_TRUE(mentions(lines, "session stop succeeded"));
}

TEST_F(LinkDriverRunningLocal, EndsWhenCancelledFromAnotherThread) {
  ASSERT_TRUE(opened_->start());
  opened_.pump_for(30ms);
  EXPECT_EQ(opened_->state(), StreamState::establishing_route);
  auto canceller = LiveProviders::spawn("test", [this] {
    LiveProviders::nap(40ms);
    opened_->cancel();
  });
  const auto began = LiveProviders::steady_us();
  libchiaking::Expected<void, StreamError> pumped{};
  while (pumped && opened_->state() == StreamState::establishing_route &&
         Rig::since(began) < 10s) {
    pumped = opened_->pump_once();
    if (pumped) EXPECT_TRUE(opened_->wait(20ms));
  }
  static_cast<void>(canceller.join());
  EXPECT_LT(Rig::since(began), 10s);
  EXPECT_TRUE(pumped);
  EXPECT_TRUE(opened_.stop.stop_requested());
  EXPECT_EQ(opened_->state(), StreamState::stopped);
  EXPECT_EQ(opened_.events.failed_events, 0U);
  EXPECT_FALSE(opened_->failure());
  EXPECT_EQ(opened_.events.state_events, 2U);
  ASSERT_FALSE(opened_.events.states.empty());
  EXPECT_EQ(opened_.events.states.front().state,
            StreamState::establishing_route);
  EXPECT_EQ(opened_.events.states.back().state, StreamState::stopped);
  EXPECT_EQ(opened_.events.states.back().quit_reason, QuitReason::stopped);
  EXPECT_EQ(opened_->quit().reason, QuitReason::stopped);
  EXPECT_TRUE(Rig::refused(opened_->pump_once(), StreamError::invalid_state));
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(opened_.events.state_events, 2U);
  EXPECT_EQ(opened_.events.failed_events, 0U);
  EXPECT_EQ(opened_->state(), StreamState::stopped);
  const auto lines = drained();
  EXPECT_TRUE(mentions(lines, "session cancellation requested"));
  EXPECT_TRUE(std::ranges::any_of(lines, [](const std::string& line) {
    return line.find("local") != std::string::npos &&
           line.find("operation=stop_requested") != std::string::npos;
  }));
}

TEST_F(LinkDriverRunningLocal, ReportsAStopFromAStartCallbackThroughStart) {
  std::optional<libchiaking::Expected<void, StreamError>> nested_stop;
  std::optional<libchiaking::Expected<void, StreamError>> nested_start;
  std::optional<libchiaking::Expected<void, StreamError>> nested_pump;
  unsigned stopped_events = 0;
  opened_.events.hook = [&](const remote_play::StreamEvent& event) {
    const auto* changed = std::get_if<remote_play::event::StateChanged>(&event);
    if (changed == nullptr) return;
    if (changed->state == StreamState::establishing_route) {
      nested_start = opened_->start();
      nested_pump = opened_->pump_once();
      nested_stop = opened_->stop();
    }
    if (changed->state == StreamState::stopped) ++stopped_events;
  };
  const auto started = opened_->start();
  ASSERT_TRUE(nested_stop);
  EXPECT_TRUE(*nested_stop);
  ASSERT_TRUE(nested_start);
  EXPECT_TRUE(Rig::refused(*nested_start, StreamError::invalid_state));
  ASSERT_TRUE(nested_pump);
  EXPECT_TRUE(Rig::refused(*nested_pump, StreamError::invalid_state));
  EXPECT_EQ(opened_->state(), StreamState::stopped);
  EXPECT_TRUE(Rig::refused(started, StreamError::invalid_state));
  EXPECT_EQ(stopped_events, 1U);
  EXPECT_TRUE(Rig::refused(opened_->pump_once(), StreamError::invalid_state));
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(stopped_events, 1U);
  EXPECT_TRUE(mentions(drained(), "deferred until the callback returns"));
}

TEST_F(LinkDriverRunningLocal, DefersASleepStopFromAStartCallback) {
  std::optional<libchiaking::Expected<void, StreamError>> nested;
  opened_.events.hook = [&](const remote_play::StreamEvent& event) {
    const auto* changed = std::get_if<remote_play::event::StateChanged>(&event);
    if (changed != nullptr && changed->state == StreamState::establishing_route)
      nested = opened_->stop_for_sleep();
  };
  EXPECT_TRUE(Rig::refused(opened_->start(), StreamError::invalid_state));
  ASSERT_TRUE(nested);
  EXPECT_TRUE(*nested);
  EXPECT_EQ(opened_->state(), StreamState::stopped);
  EXPECT_TRUE(mentions(drained(), "session sleep stop succeeded"));
}

}  // namespace
}  // namespace play_test
