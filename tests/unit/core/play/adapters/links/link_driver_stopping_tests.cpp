#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::Expected;
using libchiaking::unexpected;
using link_drivers::FakeSession;
using link_drivers::RecordingSink;
using link_drivers::Rig;
using remote_play::StreamError;
using remote_play::StreamState;

TEST(LinkDriverStopping, DefersAStopFromAStartCallbackAndReportsItFromStart) {
  Rig rig;
  Expected<void, StreamError> nested_stop =
      unexpected(StreamError::pipeline_failed);
  Expected<void, StreamError> nested_start{};
  Expected<void, StreamError> nested_pump{};
  Expected<void, StreamError> nested_wait{};
  unsigned stops_seen_in_callback = 99;
  rig.sink().on_state = [&](StreamState state) {
    if (state != StreamState::establishing_route) return;
    nested_start = rig.driver().start();
    nested_pump = rig.driver().pump_once();
    nested_wait = rig.driver().wait(std::chrono::milliseconds{1});
    nested_stop = rig.driver().stop();
    stops_seen_in_callback = rig.session().stops;
  };
  const auto started = rig.driver().start();
  EXPECT_TRUE(nested_stop);
  EXPECT_EQ(stops_seen_in_callback, 0U);
  ASSERT_FALSE(nested_start);
  EXPECT_EQ(nested_start.error(), StreamError::invalid_state);
  ASSERT_FALSE(nested_pump);
  EXPECT_EQ(nested_pump.error(), StreamError::invalid_state);
  ASSERT_FALSE(nested_wait);
  EXPECT_EQ(nested_wait.error(), StreamError::invalid_state);
  EXPECT_EQ(rig.driver().state(), StreamState::stopped);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_state);
  EXPECT_EQ(rig.session().stops, 1U);
  EXPECT_EQ(rig.session().sleep_stops, 0U);
  EXPECT_EQ(rig.sink().count(StreamState::stopped), 1);
  EXPECT_TRUE(rig.stop().stop_requested());
  const auto pumped = rig.driver().pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), StreamError::invalid_state);
  EXPECT_TRUE(rig.driver().stop());
  EXPECT_EQ(rig.sink().count(StreamState::stopped), 1);
  EXPECT_TRUE(rig.logged("Starting local connection"));
  EXPECT_TRUE(rig.logged("session start begin"));
  EXPECT_TRUE(rig.logged("session start succeeded"));
  EXPECT_TRUE(
      rig.logged("session stop requested from a callback; deferred "
                 "until the callback returns"));
  EXPECT_TRUE(rig.logged("session stop begin"));
  EXPECT_TRUE(rig.logged("session stop succeeded"));
}

TEST(LinkDriverStopping, DefersASleepStopFromAPumpCallbackUntilThePumpReturns) {
  Rig rig{libchiaking::play::Venue::cloud(
      libchiaking::catalog::ServiceGeneration::old_service)};
  ASSERT_TRUE(rig.driver().start());
  EXPECT_TRUE(rig.logged("Starting LegacyCloudStream cloud connection"));
  rig.session().pending_events.emplace_back(
      remote_play::event::StateChanged{StreamState::streaming});
  Expected<void, StreamError> nested = unexpected(StreamError::pipeline_failed);
  rig.sink().on_state = [&](StreamState state) {
    if (state == StreamState::streaming) nested = rig.driver().stop_for_sleep();
  };
  EXPECT_TRUE(rig.driver().pump_once());
  EXPECT_TRUE(nested);
  EXPECT_EQ(rig.session().sleep_stops, 1U);
  EXPECT_EQ(rig.session().stops, 1U);
  EXPECT_EQ(rig.driver().state(), StreamState::stopped);
  EXPECT_TRUE(rig.logged("session sleep stop begin"));
  EXPECT_TRUE(rig.logged("session sleep stop succeeded"));
}

TEST(LinkDriverStopping, ReportsAndLogsAFailedStop) {
  Rig rig{std::nullopt};
  ASSERT_TRUE(rig.driver().start());
  EXPECT_TRUE(rig.logged("Starting stream connection"));
  rig.session().stop_result = unexpected(StreamError::shutdown_failed);
  const auto stopped = rig.driver().stop();
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), StreamError::shutdown_failed);
  EXPECT_TRUE(rig.logged("session stop failed"));
}

TEST(LinkDriverStopping, ReportsAndLogsAFailedStart) {
  Rig failing{libchiaking::play::Venue::internet()};
  failing.session().start_result = unexpected(StreamError::route_failed);
  const auto started = failing.driver().start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_TRUE(failing.logged("Starting remote connection"));
  EXPECT_TRUE(failing.logged("session start failed"));
}

TEST(LinkDriverStopping, NamesTheCloudStreamConnectionItStarts) {
  Rig cloud{libchiaking::play::Venue::cloud(
      libchiaking::catalog::ServiceGeneration::ps5_cloud)};
  EXPECT_TRUE(cloud.driver().start());
  EXPECT_TRUE(cloud.logged("Starting CloudStream connection"));
}

TEST(LinkDriverStopping, RequestsAStopWhenDestroyed) {
  libchiaking::platform::StopSource stop;
  const auto token = stop.token();
  RecordingSink sink;
  libchiaking::diagnostics::LogQueue log{LiveProviders::steady()};
  {
    libchiaking::play::LinkDriver driver{std::make_unique<FakeSession>(), stop,
                                         LiveProviders::threading(), sink, log};
    EXPECT_FALSE(token.stop_requested());
  }
  EXPECT_TRUE(token.stop_requested());
}

}  // namespace
}  // namespace play_test
