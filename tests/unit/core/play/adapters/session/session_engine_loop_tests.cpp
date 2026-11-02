#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using namespace std::chrono_literals;
using libchiaking::media::VideoVerdict;
using libchiaking::testing::ScriptedSession;
using Cause = play::EndReason::Cause;
using Ending = play::Outcome::Ending;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

class SessionEngineOrder : public ::testing::Test {
 protected:
  void SetUp() override {
    rig_.then([this](ScriptedSession& s) {
      s.set_state(StreamState::streaming);
      s.emit(api_test::stream_info());
      s.emit(remote_play::event::KeyboardOpen{""});
      s.emit(remote_play::event::PinRequired{});
      rig_.media.verdict = {VideoVerdict::recover, -9};
      s.emit(api_test::video_media(5));
      s.emit(api_test::video_media(6));
      rig_.media.verdict = {};
      auto& commands = rig_.runner->commands();
      libchiaking::controllers::ControllerState input;
      input.buttons = 2;
      EXPECT_TRUE(rig_.runner->controller_rules().send(
          libchiaking::controllers::SeatNumber::first(), input));
      EXPECT_TRUE(commands.pin().submit(*play::AccessCode::parse("1234")));
      auto& seats = rig_.runner->seat_rules();
      EXPECT_TRUE(
          seats.enter_passcode(libchiaking::controllers::SeatNumber::masked(1),
                               *play::AccessCode::parse("4321")));
      EXPECT_TRUE(seats.add_player(
          libchiaking::controllers::SeatNumber::masked(1),
          libchiaking::controllers::ControllerKind::dualsense, std::nullopt));
      EXPECT_TRUE(commands.go_home());
      EXPECT_TRUE(commands.microphone().connect());
      EXPECT_TRUE(commands.keyboard().set_text("a"));
      EXPECT_TRUE(
          rig_.runner->stop_with_rest(play::RestRequest::Reason::leaving));
    });
    rig_.then([this](ScriptedSession&) { rig_.clock.advance(395ms); });
    rig_.then([this](ScriptedSession&) { rig_.clock.advance(5ms); });

    EXPECT_EQ(rig_.runner->lifecycle().phase(),
              play::SessionLifecycle::Phase::created);
    outcome_.emplace(rig_.run());
  }

  auto rig() -> SessionEngineRig& { return rig_; }
  auto outcome() const -> const play::Outcome& { return *outcome_; }

 private:
  SessionEngineRig rig_;
  std::optional<play::Outcome> outcome_;
};

TEST_F(SessionEngineOrder, FinishesItsLifecycle) {
  EXPECT_EQ(rig().runner->lifecycle().phase(),
            play::SessionLifecycle::Phase::finished);
}

TEST_F(SessionEngineOrder, RunsItsStepsInOneFixedOrder) {
  const auto& s = *rig().session;
  ASSERT_GE(s.calls.size(), 2U);
  EXPECT_EQ(s.calls[0], "start");
  EXPECT_EQ(s.calls[1], "pump");
  EXPECT_EQ(s.calls_between_pumps(0).size(), 1U);
  EXPECT_EQ(
      s.calls_between_pumps(1),
      (std::vector<std::string>{
          "loss", "idr", "controller", "pin", "couch_passcode", "couch_join",
          "go_home", "microphone_connect", "keyboard_text", "goto_bed"}));
  EXPECT_EQ(s.calls_between_pumps(2), (std::vector<std::string>{"wait"}));
  EXPECT_EQ(s.calls_between_pumps(3),
            (std::vector<std::string>{"wait", "stop"}));
  EXPECT_EQ(s.waits, (std::vector<std::chrono::milliseconds>{8ms, 5ms}));
  EXPECT_EQ(s.pumps, 3);
  EXPECT_EQ(s.stops, 1);
  EXPECT_EQ(s.sleep_stops, 0);
}

TEST_F(SessionEngineOrder, HandsTheLinkWhatThePlayerAskedFor) {
  const auto& s = *rig().session;
  ASSERT_EQ(s.losses.size(), 1U);
  EXPECT_EQ(s.losses[0], (std::pair<std::uint16_t, std::uint16_t>{5, 6}));
  EXPECT_EQ(s.pin, "1234");
  ASSERT_EQ(s.controllers.size(), 1U);
  EXPECT_EQ(s.controllers[0].buttons, 2U);
}

TEST_F(SessionEngineOrder, PassesMediaAndPromptsOnInOrder) {
  EXPECT_EQ(rig().media.infos, 1);
  EXPECT_EQ(rig().media.videos, 2);
  ASSERT_GE(rig().media.headers.size(), 2U);
  EXPECT_EQ(rig().media.headers[0].size(), 3U);
  EXPECT_TRUE(rig().media.headers[1].empty());
  EXPECT_EQ(rig().events.count("login_pin_needed"), 1U);
  EXPECT_EQ(rig().events.count("keyboard_opened"), 1U);
  EXPECT_EQ(rig().events.names(),
            (std::vector<std::string>{"state", "state", "keyboard_opened",
                                      "login_pin_needed", "state", "ended"}));
}

TEST_F(SessionEngineOrder, EndsLeftWithTheRestSent) {
  EXPECT_EQ(outcome().ending(), Ending::left);
  EXPECT_TRUE(ended_as(outcome(), Ending::left));
  EXPECT_TRUE(outcome().was_left());
  EXPECT_EQ(outcome().rest(), play::RestOutcome::sent);
  EXPECT_EQ(outcome().reason(), nullptr);
  EXPECT_EQ(outcome().cleanup(), nullptr);
  EXPECT_FALSE(outcome().media_fault());
}

TEST_F(SessionEngineOrder, LogsTheNegotiatedStreamAndTheStop) {
  EXPECT_EQ(rig().logged("stream negotiated: 1 video profiles (1280x720); "
                         "audio 2 ch 48000 Hz"),
            1U);
  EXPECT_EQ(rig().logged("session stop succeeded"), 1U);
}

TEST_F(SessionEngineOrder, RunsOnlyOnce) {
  EXPECT_FALSE(rig().runner->run_on_this_thread());
  EXPECT_FALSE(rig().runner->begin());
  ASSERT_TRUE(rig().runner->outcome());
  EXPECT_TRUE(ended_as(*rig().runner->outcome(), Ending::left));
  EXPECT_EQ(rig().events.count("ended"), 1U);
}

class SessionEnginePump : public ::testing::Test {
 protected:
  static void fail_transiently(ScriptedSession& s) {
    s.pump_result = libchiaking::unexpected(StreamError::invalid_state);
  }
};

TEST_F(SessionEnginePump, ToleratesSixtyFourTransientErrorsInARow) {
  SessionEngineRig h;
  h.streaming();
  for (int index = 0; index < 64; ++index) h.then(fail_transiently);
  h.then([](ScriptedSession&) {});
  for (int index = 0; index < 65; ++index) h.then(fail_transiently);
  h.then([](ScriptedSession&) { ADD_FAILURE(); });
  const auto outcome = h.run();
  EXPECT_EQ(h.session->pumps, 131);
  EXPECT_EQ(outcome.ending(), Ending::session_failed);
  EXPECT_TRUE(ended_as(outcome, Ending::session_failed, Cause::invalid_state));
  EXPECT_EQ(outcome.cause(), Cause::invalid_state);
  EXPECT_EQ(h.runner->statistics().loop_errors, 129U);
  EXPECT_EQ(h.logged("pump transient result=invalid_state count=64"), 2U);
}

TEST_F(SessionEnginePump, EndsAtOnceOnAFatalError) {
  SessionEngineRig fatal;
  fatal.streaming();
  fatal.then([](ScriptedSession& s) {
    s.pump_result = libchiaking::unexpected(StreamError::invalid_argument);
  });
  const auto outcome = fatal.run();
  EXPECT_EQ(fatal.session->pumps, 2);
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::invalid_argument));
}

TEST_F(SessionEnginePump, EndsWhenTheLinkHasFailed) {
  SessionEngineRig ended;
  ended.streaming();
  ended.then([](ScriptedSession& s) {
    s.pump_result = libchiaking::unexpected(StreamError::pipeline_failed);
    s.fail_silently();
  });
  const auto outcome = ended.run();
  EXPECT_EQ(ended.session->pumps, 2);
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::pipeline_failed));
}

TEST_F(SessionEnginePump, EndsOnTheFirstErrorWithoutTolerance) {
  play::SessionSettings strict;
  strict.pump_error_tolerance = 0;
  SessionEngineRig fast{play::Venue::home(), strict};
  fast.streaming();
  fast.then(fail_transiently);
  const auto outcome = fast.run();
  EXPECT_EQ(fast.session->pumps, 2);
  EXPECT_TRUE(ended_as(outcome, Ending::session_failed, Cause::invalid_state));
}

TEST(SessionEngineStatistics, FollowStateChangesAndATimerNotEveryPass) {
  SessionEngineRig h;
  h.session->link.available = true;
  h.streaming();
  for (int pass = 0; pass < 40; ++pass)
    h.then([](ScriptedSession& s) { ++s.link.video_units; });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.pumps, 42);
    EXPECT_EQ(s.diagnostics_reads, 1);
    EXPECT_EQ(h.runner->statistics().link.video_units, 0U);
    h.clock.advance(249ms);
  });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.diagnostics_reads, 1);
    h.clock.advance(1ms);
  });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.diagnostics_reads, 2);
    EXPECT_EQ(h.runner->statistics().link.video_units, 40U);
    ++s.link.video_units;
  });
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(h.session->diagnostics_reads, 4);
  EXPECT_EQ(h.runner->statistics().link.video_units, 41U);
}

}  // namespace
}  // namespace play_test
