#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using libchiaking::unexpected;
using libchiaking::net::protocol::remote_play::StreamError;
using play::InputRefusal;
using play::Microphone;
using play::RestRequest;

class ConsoleMicrophone : public testing::Test {
 protected:
  auto console() -> ConsoleRig& { return console_; }
  auto microphone() -> Microphone::Core& {
    return console_.commands().microphone();
  }

  auto connect_and_unmute() {
    EXPECT_TRUE(microphone().open());
    auto ticket = microphone().connect(77);
    EXPECT_TRUE(ticket);
    EXPECT_TRUE(microphone().set_muted(false));
    console_.streaming();
    return ticket;
  }

 private:
  ConsoleRig console_;
};

TEST_F(ConsoleMicrophone, CarriesFortyByteFramesSixteenDeep) {
  EXPECT_EQ(Microphone::frame_size, 40U);
  EXPECT_EQ(Microphone::default_depth, 16U);
  EXPECT_EQ(Microphone::frame_size, play::LinkDriver::microphone_frame_size);
}

TEST_F(ConsoleMicrophone, RefusesFramesBeforeItIsOpen) {
  EXPECT_TRUE(
      ConsoleRig::refused(microphone().push(ConsoleRig::microphone_frame(1)),
                          InputRefusal::not_ready));
}

TEST_F(ConsoleMicrophone, TagsEachConnectionAndConnectsOnlyWhileStreaming) {
  EXPECT_TRUE(microphone().open());
  EXPECT_FALSE(microphone().latest());
  const auto untagged = microphone().connect();
  ASSERT_TRUE(untagged);
  EXPECT_EQ(untagged.value().tag(), 0U);
  const auto ticket = microphone().connect(77);
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket.value().tag(), 77U);
  EXPECT_FALSE(ticket.value() == untagged.value());
  EXPECT_EQ(microphone().latest(), ticket.value());
  ASSERT_TRUE(microphone().latest());
  EXPECT_EQ(microphone().latest()->tag(), 77U);
  EXPECT_EQ(microphone().state(ticket.value()),
            Microphone::ConnectState::pending);
  EXPECT_TRUE(microphone().set_muted(false));
  EXPECT_TRUE(microphone().pending());
  console().negotiating();
  EXPECT_EQ(console().session().count("microphone_connect"), 0U);
  console().streaming();
  EXPECT_EQ(
      console().session().calls_between_pumps(0),
      (std::vector<std::string>{"microphone_connect", "microphone_unmute"}));
  EXPECT_EQ(microphone().state(ticket.value()),
            Microphone::ConnectState::accepted);
}

TEST_F(ConsoleMicrophone, KeepsTheNewestSixteenFramesAndCountsFailures) {
  static_cast<void>(connect_and_unmute());
  for (int index = 0; index < 18; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(microphone().push(ConsoleRig::microphone_frame(index)));
  }
  auto statistics = microphone().statistics();
  EXPECT_EQ(statistics.frames_queued, 18U);
  EXPECT_EQ(statistics.frames_dropped, 2U);
  console().streaming();
  const auto& frames = console().session().microphone_frames;
  ASSERT_EQ(frames.size(), 16U);
  ASSERT_EQ(frames.front().size(), 40U);
  EXPECT_EQ(frames.front()[0], std::byte{2});
  ASSERT_FALSE(frames.back().empty());
  EXPECT_EQ(frames.back()[0], std::byte{17});
  EXPECT_EQ(microphone().statistics().frames_sent, 16U);
  EXPECT_FALSE(microphone().pending());

  console().session().microphone_result =
      unexpected(StreamError::invalid_state);
  EXPECT_TRUE(microphone().push(ConsoleRig::microphone_frame(1)));
  EXPECT_TRUE(microphone().push(ConsoleRig::microphone_frame(2)));
  console().streaming();
  statistics = microphone().statistics();
  EXPECT_EQ(statistics.send_failures, 2U);
  EXPECT_EQ(statistics.last_error, play::EndReason::Cause::invalid_state);
  EXPECT_EQ(statistics.frames_sent, 16U);
  EXPECT_EQ(console().logged(
                "Microphone frame failed: result=invalid_state failures="),
            2U);

  EXPECT_TRUE(microphone().push(ConsoleRig::microphone_frame(3)));
  EXPECT_TRUE(microphone().set_muted(true));
  EXPECT_EQ(microphone().statistics().frames_dropped, 3U);
  EXPECT_TRUE(
      ConsoleRig::refused(microphone().push(ConsoleRig::microphone_frame(4)),
                          InputRefusal::not_ready));
  console().streaming();
  EXPECT_EQ(console().session().count("microphone_mute"), 1U);
}

TEST_F(ConsoleMicrophone, RejectsAConnectionTheConsoleRefuses) {
  const auto ticket = connect_and_unmute();
  ASSERT_TRUE(ticket);
  console().session().microphone_connect_result =
      unexpected(StreamError::invalid_state);
  const auto second = microphone().connect();
  ASSERT_TRUE(second);
  EXPECT_FALSE(second.value() == ticket.value());
  EXPECT_EQ(microphone().state(ticket.value()),
            Microphone::ConnectState::accepted);
  console().streaming();
  EXPECT_EQ(microphone().state(second.value()),
            Microphone::ConnectState::rejected);
}

TEST_F(ConsoleMicrophone, RejectsAConnectionAndUnmutingOnceRestModeIsAsked) {
  static_cast<void>(connect_and_unmute());
  const auto third = microphone().connect();
  ASSERT_TRUE(third);
  EXPECT_TRUE(
      console().commands().rest().request(RestRequest::Reason::leaving));
  EXPECT_EQ(microphone().state(third.value()),
            Microphone::ConnectState::rejected);
  EXPECT_TRUE(ConsoleRig::refused(microphone().set_muted(false),
                                  InputRefusal::not_ready));
}

}  // namespace
}  // namespace play_test
