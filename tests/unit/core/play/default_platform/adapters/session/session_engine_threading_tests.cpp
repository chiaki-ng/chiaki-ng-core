#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/session_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/unit/core/play/outcome_checks.hpp"
#include "tests/unit/core/play/real_session_rig.hpp"
#include "tests/unit/core/play/recording_media.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;
using api_test::LoopbackListener;
using api_test::RealStream;
using libchiaking::Expected;
using play::InputRefusal;
using remote_play::StreamError;
using remote_play::StreamState;
using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::SignedInPlayer;
using namespace std::chrono_literals;

class SessionEngineThreading : public ::testing::Test {
 protected:
  static auto accepted_input(const Expected<void, StreamError>& result)
      -> testing::AssertionResult {
    if (result || result.error() == StreamError::invalid_state ||
        result.error() == StreamError::invalid_argument ||
        result.error() == StreamError::pin_rejected ||
        result.error() == StreamError::controller_rejected)
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "refused with " << testing::PrintToString(result.error());
  }

  template <class Value>
  static auto answered(const Expected<Value, InputRefusal>& result)
      -> testing::AssertionResult {
    if (result || result.error() <= InputRefusal::not_ready)
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "refused with " << testing::PrintToString(result.error());
  }

  static void until_establishing(play::Session::Core& runner,
                                 std::chrono::steady_clock::time_point began) {
    while (runner.link_state() != StreamState::establishing_route &&
           LiveProviders::now() - began < 10s)
      LiveProviders::nap(1ms);
  }

  LoopbackListener listener_;
};

TEST_F(SessionEngineThreading, PollsStateAndDiagnosticsFromAnotherThread) {
  RealStream opened{RealStream::local(listener_.port())};
  std::atomic<bool> done{false};
  std::atomic<unsigned> polls{0};
  auto poller = LiveProviders::spawn("poller", [&] {
    while (!done.load()) {
      const auto state = opened->state();
      EXPECT_GE(state, StreamState::idle);
      EXPECT_LE(state, StreamState::failed);
      const auto diagnostics = opened->diagnostics();
      EXPECT_FALSE(diagnostics.available);
      EXPECT_EQ(diagnostics.video_units, 0U);
      polls.fetch_add(1);
    }
  });
  while (polls.load() == 0) LiveProviders::spin_pause();
  EXPECT_TRUE(opened->start());
  opened.pump_for(100ms);
  EXPECT_EQ(opened->state(), StreamState::establishing_route);
  EXPECT_TRUE(opened->stop());
  EXPECT_EQ(opened->state(), StreamState::stopped);
  done = true;
  static_cast<void>(poller.join());
}

TEST_F(SessionEngineThreading, TakesHostInputFromAnotherThread) {
  auto configuration = RealStream::local(listener_.port());
  configuration.enable_keyboard = true;
  RealStream opened{std::move(configuration)};
  std::atomic<bool> done{false};
  std::atomic<unsigned> rounds{0};
  auto input = LiveProviders::spawn("input", [&] {
    remote_play::ControllerState sample;
    const std::array<std::byte, play::LinkDriver::microphone_frame_size>
        frame{};
    const std::array pin{std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                         std::byte{'4'}};
    const auto flags = remote_play::audio_state_flags(3, 0xff, 1);
    std::uint32_t buttons = 0;
    while (!done.load()) {
      sample.buttons = buttons++;
      EXPECT_TRUE(accepted_input(opened->send_controller_state(sample)));
      EXPECT_TRUE(accepted_input(opened->submit_pin(pin)));
      EXPECT_TRUE(accepted_input(opened->send_microphone(frame)));
      EXPECT_TRUE(accepted_input(opened->set_microphone_muted(true)));
      EXPECT_TRUE(accepted_input(opened->connect_microphone()));
      EXPECT_TRUE(accepted_input(opened->request_idr()));
      EXPECT_TRUE(accepted_input(opened->report_video_loss(1, 2)));
      EXPECT_TRUE(accepted_input(opened->execute(
          command::KeyboardSetText{libchiaking::SecretString{"hi"}})));
      EXPECT_TRUE(accepted_input(opened->execute(command::KeyboardAccept{})));
      EXPECT_TRUE(accepted_input(opened->execute(command::KeyboardReject{})));
      EXPECT_TRUE(accepted_input(opened->execute(command::GotoBed{})));
      EXPECT_TRUE(accepted_input(opened->execute(command::GoHome{})));
      EXPECT_TRUE(accepted_input(opened->execute(command::SetPadCount{2})));
      EXPECT_TRUE(accepted_input(opened->execute(command::CouchUserJoin{1})));
      EXPECT_TRUE(
          accepted_input(opened->execute(command::CouchPresence{1, true})));
      EXPECT_TRUE(
          accepted_input(opened->execute(command::CouchAnnouncePads{})));
      EXPECT_TRUE(accepted_input(opened->execute(command::CouchClosePad{1})));
      EXPECT_TRUE(accepted_input(
          opened->send_audio_state(remote_play::AudioStateKind::flags, flags)));
      rounds.fetch_add(1);
    }
  });
  while (rounds.load() < 4) LiveProviders::spin_pause();
  EXPECT_TRUE(opened->start());
  const auto started = rounds.load();
  while (rounds.load() < started + 4) opened.pump_for(5ms);
  opened.pump_for(50ms);
  EXPECT_EQ(opened->state(), StreamState::establishing_route);
  EXPECT_TRUE(opened->stop());
  const auto stopped = rounds.load();
  while (rounds.load() < stopped + 4) LiveProviders::spin_pause();
  done = true;
  static_cast<void>(input.join());
  EXPECT_EQ(opened->state(), StreamState::stopped);
}

TEST_F(SessionEngineThreading, TakesInputDuringAWaitWithoutBlocking) {
  RealStream opened{RealStream::local(listener_.port())};
  ASSERT_TRUE(opened->start());
  std::atomic<bool> sent{false};
  auto input = LiveProviders::spawn("input", [&] {
    LiveProviders::nap(50ms);
    EXPECT_TRUE(accepted_input(
        opened->send_controller_state(remote_play::ControllerState{})));
    sent = true;
  });
  const auto started = LiveProviders::now();
  EXPECT_TRUE(opened->wait(3000ms));
  const auto waited = LiveProviders::now() - started;
  static_cast<void>(input.join());
  EXPECT_TRUE(sent.load());
  EXPECT_LT(waited, 2000ms);
  EXPECT_TRUE(opened->pump_once());
  EXPECT_TRUE(opened->stop());
}

class RealRunner {
 public:
  explicit RealRunner(std::uint16_t ctrl_port) {
    auto configuration = RealStream::local(ctrl_port);
    configuration.enable_keyboard = true;
    auto checked = std::move(configuration).validated();
    EXPECT_TRUE(checked);
    checked.value().stop = stop.token();
    auto session = play::HomeLinkConfiguration::link(std::move(checked.value()),
                                                     platform.factory());
    EXPECT_TRUE(session);
    EXPECT_TRUE(session.value());
    journal->follow(platform.log(), &platform.api_capture());
    auto created = play::Session::Core::open(
        {.player = SignedInPlayer::fresh(),
         .console = nullptr,
         .where = play::Where::home,
         .service = std::nullopt,
         .link = std::move(session.value()),
         .stop = stop,
         .media = media,
         .events = events,
         .clock = platform.clock(),
         .threading = platform.factory().threading(),
         .journal = journal,
         .first_controller =
             libchiaking::controllers::ControllerKind::dualsense,
         .settings = {},
         .share = nullptr});
    EXPECT_TRUE(created);
    runner = std::move(created.value());
  }

  auto run() -> play::Outcome {
    EXPECT_TRUE(runner->run_on_this_thread());
    return runner->outcome().value();
  }

  libchiaking::platform::StopSource stop;
  libchiaking::platform::DefaultPlatform platform;
  libchiaking::testing::RecordingMedia media;
  RecordedEvents events;
  std::shared_ptr<libchiaking::diagnostics::SessionJournal> journal{
      std::make_shared<libchiaking::diagnostics::SessionJournal>(
          LiveProviders::threading(), LiveProviders::steady(), 1)};
  std::unique_ptr<play::Session::Core> runner;
};

TEST_F(SessionEngineThreading, TakesRunnerInputFromAnotherThread) {
  RealRunner rig{listener_.port()};
  auto& runner = *rig.runner;
  std::atomic<bool> done{false};
  std::atomic<unsigned> rounds{0};
  auto input = LiveProviders::spawn("input", [&] {
    libchiaking::controllers::ControllerState sample;
    const std::array<std::byte, play::Microphone::frame_size> frame{};
    const auto guest = libchiaking::controllers::SeatNumber::masked(1);
    const auto first = libchiaking::controllers::SeatNumber::first();
    std::uint32_t buttons = 0;
    while (!done.load()) {
      sample.buttons = buttons++;
      EXPECT_TRUE(answered(runner.controller_rules().send(first, sample)));
      EXPECT_TRUE(answered(runner.controller_rules().forward(first, sample)));
      auto code = play::AccessCode::parse("1234");
      EXPECT_TRUE(code);
      const auto ticket =
          runner.commands().pin().submit(std::move(code).value());
      EXPECT_TRUE(answered(ticket));
      if (ticket)
        EXPECT_LE(runner.commands().pin().progress(ticket.value()),
                  play::LoginPin::Progress::rejected);
      EXPECT_TRUE(answered(runner.commands().microphone().push(frame)));
      EXPECT_TRUE(answered(runner.commands().microphone().set_muted(true)));
      EXPECT_TRUE(answered(runner.commands().microphone().connect()));
      static_cast<void>(runner.request_keyframe());
      static_cast<void>(runner.video_presented());
      EXPECT_TRUE(answered(runner.commands().keyboard().set_text("hi")));
      EXPECT_TRUE(answered(runner.commands().keyboard().accept()));
      EXPECT_TRUE(answered(runner.commands().keyboard().reject()));
      EXPECT_TRUE(answered(runner.commands().go_home()));
      EXPECT_TRUE(answered(runner.seat_rules().add_player(
          guest, libchiaking::controllers::ControllerKind::console_default,
          std::nullopt)));
      auto passcode = play::AccessCode::parse("4321");
      EXPECT_TRUE(passcode);
      EXPECT_TRUE(answered(runner.seat_rules().enter_passcode(
          guest, std::move(passcode).value())));
      EXPECT_TRUE(answered(runner.seat_rules().remove_player(guest)));
      static_cast<void>(runner.seat_rules().seat_guest(
          guest, play::GuestTag{9},
          libchiaking::controllers::ControllerKind::console_default,
          std::nullopt));
      runner.seat_rules().release_guest(guest);
      static_cast<void>(runner.commands().set_audio_3d((buttons & 1U) != 0));
      static_cast<void>(runner.statistics());
      static_cast<void>(runner.link_state());
      runner.wake();
      rounds.fetch_add(1);
    }
  });
  while (rounds.load() < 4) LiveProviders::spin_pause();
  auto canceller = LiveProviders::spawn("canceller", [&] {
    until_establishing(runner, LiveProviders::now());
    const auto running = rounds.load();
    while (rounds.load() < running + 8) LiveProviders::nap(1ms);
    runner.cancel();
  });
  const auto outcome = rig.run();
  static_cast<void>(canceller.join());
  EXPECT_TRUE(outcome.was_left());
  EXPECT_FALSE(outcome.succeeded());
  const auto ended = rounds.load();
  while (rounds.load() < ended + 4) LiveProviders::spin_pause();
  done = true;
  static_cast<void>(input.join());
  EXPECT_TRUE(runner.link_state() == StreamState::failed ||
              runner.link_state() == StreamState::stopped)
      << testing::PrintToString(runner.link_state());
  EXPECT_TRUE(rig.stop.stop_requested());
}

TEST_F(SessionEngineThreading, EndsTheRunOnARestRequestFromAnotherThread) {
  RealRunner rig{listener_.port()};
  auto& runner = *rig.runner;
  std::atomic<bool> asked{false};
  auto rester = LiveProviders::spawn("rester", [&] {
    until_establishing(runner, LiveProviders::now());
    static_cast<void>(
        runner.stop_with_rest(play::RestRequest::Reason::leaving));
    asked = true;
  });
  const auto began = LiveProviders::now();
  const auto outcome = rig.run();
  static_cast<void>(rester.join());
  EXPECT_TRUE(asked.load());
  EXPECT_LT(LiveProviders::now() - began, 10s);
  EXPECT_TRUE(!outcome.succeeded() ||
              outcome.rest() != play::RestOutcome::none);
  EXPECT_NE(runner.link_state(), StreamState::establishing_route);
}

TEST_F(SessionEngineThreading, EndsTheRunOnDecoderRejectionsFromAnotherThread) {
  RealRunner rig{listener_.port()};
  auto& runner = *rig.runner;
  std::atomic<bool> ended{false};
  auto decoder = LiveProviders::spawn("decoder", [&] {
    const auto began = LiveProviders::now();
    until_establishing(runner, began);
    std::uint16_t frame = 0;
    while (!ended.load() && LiveProviders::now() - began < 10s) {
      runner.video_rejected(frame++, 7);
      static_cast<void>(runner.video_presented());
      LiveProviders::nap(1ms);
    }
  });
  const auto began = LiveProviders::now();
  const auto outcome = rig.run();
  ended = true;
  static_cast<void>(decoder.join());
  EXPECT_LT(LiveProviders::now() - began, 10s);
  EXPECT_EQ(outcome.ending(), play::Outcome::Ending::media_failed);
  EXPECT_TRUE(
      ended_as(outcome, libchiaking::media::MediaFailure::recovery_request));
  EXPECT_FALSE(outcome.was_left());
  ASSERT_TRUE(outcome.media_fault());
  EXPECT_EQ(outcome.media_fault()->kind,
            libchiaking::media::MediaFailure::recovery_request);
  EXPECT_EQ(runner.link_state(), StreamState::stopped);
}

TEST_F(SessionEngineThreading, EndsTheRunOnHostSleepFromAnotherThread) {
  RealRunner rig{listener_.port()};
  auto& runner = *rig.runner;
  auto sleeper = LiveProviders::spawn("sleeper", [&] {
    until_establishing(runner, LiveProviders::now());
    runner.stop_for_host_sleep();
  });
  const auto began = LiveProviders::now();
  const auto outcome = rig.run();
  static_cast<void>(sleeper.join());
  EXPECT_LT(LiveProviders::now() - began, 10s);
  EXPECT_FALSE(outcome.succeeded());
  EXPECT_TRUE(runner.link_state() == StreamState::stopped ||
              runner.link_state() == StreamState::failed)
      << testing::PrintToString(runner.link_state());
}

}  // namespace
}  // namespace play_test
