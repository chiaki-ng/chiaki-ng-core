#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace shared = libchiaking::progress;
using libchiaking::catalog::ServiceGeneration;
using play::ConnectStep;
using play::EndReason;
using play::SessionState;
using play::Where;
using FailureStage = remote_play::FailureStage;
using Route = shared::InternetRoute;

using PlaySessionProgress = SessionCase;

TEST_F(PlaySessionProgress, ConnectStepsReachTheApp) {
  SessionRig rig;
  rig.link.progress(ConnectStep::finding_console);
  rig.link.progress(ConnectStep::opening_account_session);
  rig.link.progress(ConnectStep::pairing);
  rig.link.progress(ConnectStep::reaching_console);
  rig.link.progress(ConnectStep::opening_stream_route);
  rig.link.progress(ConnectStep::requesting_session);
  rig.link.progress(ConnectStep::opening_control);
  rig.link.progress(ConnectStep::measuring_connection);
  rig.link.progress(ConnectStep::starting_stream);
  rig.link.progress(ConnectStep::waking_console);
  rig.link.finish();
  rig.begin();
  rig.session->wait_until_ended();
  const auto progress = rig.events.progress();
  const std::array expected{ConnectStep::finding_console,
                            ConnectStep::opening_account_session,
                            ConnectStep::pairing,
                            ConnectStep::reaching_console,
                            ConnectStep::opening_stream_route,
                            ConnectStep::requesting_session,
                            ConnectStep::opening_control,
                            ConnectStep::measuring_connection,
                            ConnectStep::starting_stream};
  ASSERT_EQ(progress.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(progress[index].step, expected[index]);
    EXPECT_EQ(progress[index].attempt, 1U);
  }
}

TEST_F(PlaySessionProgress, MapsFailureStagesToConnectSteps) {
  EXPECT_EQ(play::Session::Core::connect_step(FailureStage::route_switch),
            ConnectStep::opening_stream_route);
  EXPECT_EQ(play::Session::Core::connect_step(FailureStage::psn_start),
            ConnectStep::opening_account_session);
  EXPECT_EQ(play::Session::Core::connect_step(FailureStage::ctrl_receive),
            ConnectStep::opening_control);
  EXPECT_EQ(play::Session::Core::connect_step(FailureStage::stream_info),
            ConnectStep::starting_stream);
  EXPECT_FALSE(play::Session::Core::connect_step(FailureStage::media));
  EXPECT_FALSE(play::Session::Core::connect_step(FailureStage::shutdown));
  EXPECT_FALSE(play::Session::Core::connect_step(FailureStage::cloud_allocate));
}

TEST_F(PlaySessionProgress, TellsLaunchStagesApart) {
  EXPECT_TRUE(
      play::Session::Core::is_launch_stage(FailureStage::cloud_allocate));
  EXPECT_TRUE(
      play::Session::Core::is_launch_stage(FailureStage::cloud_launch_check));
  EXPECT_FALSE(
      play::Session::Core::is_launch_stage(FailureStage::session_init));
}

TEST_F(PlaySessionProgress, ScriptedRouteSubStepsReachTheApp) {
  const std::array scripted{
      Route{Route::Step::opening_account_session},
      Route{Route::Step::gathering_candidates},
      Route{Route::Step::waiting_for_console},
      Route{Route::Step::exchanging_offers, Route::Purpose::control,
            Route::Candidates{1, 2, 0}},
      Route{Route::Step::testing_candidates, Route::Purpose::control,
            Route::Candidates{2, 1, 75}},
      Route{Route::Step::route_chosen, Route::Purpose::control, std::nullopt,
            Route::Kind::port_guessed},
      Route{Route::Step::confirming_route, Route::Purpose::control,
            std::nullopt, Route::Kind::port_guessed},
      Route{Route::Step::gathering_candidates, Route::Purpose::stream},
      Route{Route::Step::exchanging_offers, Route::Purpose::stream,
            Route::Candidates{1, 2, 0}},
      Route{Route::Step::testing_candidates, Route::Purpose::stream,
            Route::Candidates{1, 1, 0}},
      Route{Route::Step::route_chosen, Route::Purpose::stream, std::nullopt,
            Route::Kind::local},
      Route{Route::Step::confirming_route, Route::Purpose::stream, std::nullopt,
            Route::Kind::local},
      Route{Route::Step::switching_control, Route::Purpose::stream}};
  const std::array steps{
      ConnectStep::opening_account_session, ConnectStep::reaching_console,
      ConnectStep::opening_account_session, ConnectStep::reaching_console,
      ConnectStep::reaching_console,        ConnectStep::reaching_console,
      ConnectStep::reaching_console,        ConnectStep::opening_stream_route,
      ConnectStep::opening_stream_route,    ConnectStep::opening_stream_route,
      ConnectStep::opening_stream_route,    ConnectStep::opening_stream_route,
      ConnectStep::opening_stream_route};
  SessionRig rig;
  for (const auto& route : scripted) rig.link.route(route);
  rig.link.progress(ConnectStep::requesting_session);
  rig.link.finish();
  rig.begin();
  rig.session->wait_until_ended();
  const auto progress = rig.events.progress();
  ASSERT_EQ(progress.size(), scripted.size() + 1);
  for (std::size_t index = 0; index < scripted.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& told = progress[index];
    EXPECT_EQ(told.step, steps[index]);
    EXPECT_EQ(told.route, scripted[index]);
    EXPECT_EQ(told.attempt, 1U);
    EXPECT_FALSE(told.reconnecting);
    EXPECT_FALSE(told.previous_failure);
    if (scripted[index].step == Route::Step::waiting_for_console)
      EXPECT_EQ(told.wait, (shared::Wait{shared::WaitReason::console_joining,
                                         std::nullopt}));
    else
      EXPECT_FALSE(told.wait);
    EXPECT_EQ(told.status(),
              (shared::Status{std::nullopt, 1, told.elapsed, told.wait}));
  }
  EXPECT_EQ(progress.back().step, ConnectStep::requesting_session);
  EXPECT_FALSE(progress.back().route);
  EXPECT_FALSE(progress.back().wait);
}

class LaunchReports final : public play::Session::Core::LinkReports {
 public:
  explicit LaunchReports(play::SessionEvents& events) : events_(events) {}

  void launch_progress(
      const remote_play::event::SetupProgress& progress) override {
    stage = progress.stage;
    events_.on_launch_progress(
        play::LaunchProgress{ServiceGeneration::ps5_cloud});
  }

  void datacenters_measured(
      const remote_play::event::CloudDatacenters& datacenters) override {
    measured = datacenters.datacenters.size();
    events_.on_datacenters_offered(play::DatacenterOffer::from({}));
  }

  void datacenter_selected(
      const remote_play::event::CloudDatacenterSelected& selected) override {
    chosen = selected.name;
    events_.on_datacenter_chosen(play::DatacenterChoice::automatic(
        play::DatacenterName::parse(selected.name).value(), std::nullopt));
  }

  void console_named(std::string_view name) override { named = name; }

  auto launch_refused() -> std::optional<play::LaunchRefused> override {
    ++refusals;
    return play::LaunchRefused::unavailable(503);
  }

  FailureStage stage{};
  std::size_t measured{99};
  std::string chosen;
  std::string named;
  unsigned refusals{};

 private:
  play::SessionEvents& events_;
};

class PlaySessionLaunchReports : public SessionCase {
 protected:
  void SetUp() override {
    EXPECT_TRUE(rig_.core->attach(reports_));
    rig_.begin();
    EXPECT_FALSE(rig_.core->attach(reports_));
    rig_.events.until_state(SessionState::connecting);
  }

  auto rig() -> SessionRig& { return rig_; }
  auto reports() -> LaunchReports& { return reports_; }

 private:
  SessionRig rig_{Where::cloud, ServiceGeneration::ps5_cloud};
  LaunchReports reports_{rig_.events};
};

TEST_F(PlaySessionLaunchReports, GoToWhoeverAttached) {
  rig().link.core().report(remote_play::event::SetupProgress{
      FailureStage::cloud_allocate, 5, "queued"});
  rig().events.until("launch_progress");
  EXPECT_EQ(reports().stage, FailureStage::cloud_allocate);
  EXPECT_EQ(rig().events.count("connect_progress"), 0U);
  ASSERT_TRUE(rig().events.launch_progress());
  EXPECT_EQ(rig().events.launch_progress()->service(),
            ServiceGeneration::ps5_cloud);

  rig().link.core().report(remote_play::event::CloudDatacenters{});
  rig().events.until("datacenters_offered");
  EXPECT_EQ(reports().measured, 0U);
  EXPECT_EQ(rig().events.offered(), 0U);

  rig().link.core().report(
      remote_play::event::CloudDatacenterSelected{"frankfurt", 12, false});
  rig().events.until("datacenter_chosen");
  EXPECT_EQ(reports().chosen, "frankfurt");
}

TEST_F(PlaySessionLaunchReports, ExplainAFailedLaunch) {
  rig().link.core().report(remote_play::event::NicknameReceived{"Living room"});
  rig().link.core().report(remote_play::event::Holepunch{true});
  rig().link.fail(
      EndReason{EndReason::Cause::route_failed,
                {.step = EndReason::Step::cloud_allocate, .http_status = 503}});
  const auto& outcome = rig().session->wait_until_ended();
  EXPECT_EQ(reports().named, "Living room");
  EXPECT_EQ(reports().refusals, 1U);
  ASSERT_NE(outcome.launch_refused(), nullptr);
  EXPECT_EQ(outcome.launch_refused()->kind(),
            play::LaunchRefused::Kind::unavailable);
  ASSERT_NE(outcome.reason(), nullptr);
  EXPECT_EQ(outcome.reason()->step(), EndReason::Step::cloud_allocate);
  EXPECT_EQ(outcome.reason()->http_status(), 503);
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::session_failed,
                       EndReason::Cause::route_failed));
  EXPECT_EQ(rig().events.threads().size(), 1U);
}

TEST_F(PlaySessionProgress, KeepsLaunchReportsFromAnUnattachedApp) {
  SessionRig unattached{Where::cloud, ServiceGeneration::old_service};
  unattached.link.core().report(
      remote_play::event::SetupProgress{FailureStage::cloud_lock, 0, {}});
  unattached.link.core().report(remote_play::event::CloudDatacenters{});
  unattached.link.core().report(
      remote_play::event::CloudDatacenterSelected{"tokyo", 30, true});
  unattached.link.core().report(remote_play::event::NicknameReceived{"PS5"});
  unattached.link.finish();
  unattached.begin();
  EXPECT_TRUE(unattached.session->wait_until_ended().succeeded());
  EXPECT_EQ(unattached.events.count("launch_progress"), 0U);
  EXPECT_EQ(unattached.events.count("connect_progress"), 0U);
  EXPECT_EQ(unattached.events.count("datacenters_offered"), 0U);
  EXPECT_EQ(unattached.events.count("datacenter_chosen"), 0U);
}

}  // namespace
}  // namespace play_test
