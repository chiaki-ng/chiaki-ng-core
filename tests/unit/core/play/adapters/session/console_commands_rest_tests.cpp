#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using play::InputRefusal;
using play::RestRequest;

TEST(ConsoleRestMode, FlushesForFourHundredMillisecondsByDefault) {
  EXPECT_EQ(RestRequest::default_flush, 400ms);
}

TEST(ConsoleRestMode, HoldsOtherCommandsBackOnceRequested) {
  ConsoleRig console;
  auto& rest = console.commands().rest();
  EXPECT_TRUE(rest.request(RestRequest::Reason::leaving));
  EXPECT_EQ(rest.outcome(), RestRequest::Outcome::refused);
  EXPECT_TRUE(console.commands().pending(false));
  EXPECT_TRUE(ConsoleRig::refused(console.commands().go_home(),
                                  InputRefusal::not_ready));
  EXPECT_TRUE(ConsoleRig::refused(console.commands().microphone().connect(),
                                  InputRefusal::not_ready));
}

TEST(ConsoleRestMode, IsSentOnceThenFlushedBeforeItFinishes) {
  ConsoleRig console;
  auto& rest = console.commands().rest();
  EXPECT_TRUE(rest.request(RestRequest::Reason::leaving));

  auto step = console.negotiating();
  EXPECT_EQ(console.session().count("goto_bed"), 1U);
  EXPECT_FALSE(step.rest_finished);
  EXPECT_EQ(step.wait, 8ms);
  EXPECT_FALSE(step.for_sleep);
  EXPECT_EQ(rest.outcome(), RestRequest::Outcome::sent);
  EXPECT_TRUE(rest.flushing());

  console.clock().advance(395ms);
  step = console.streaming();
  EXPECT_FALSE(step.rest_finished);
  EXPECT_EQ(step.wait, 5ms);
  EXPECT_TRUE(rest.request(RestRequest::Reason::host_sleep));
  console.clock().advance(5ms);
  step = console.streaming();
  EXPECT_TRUE(step.rest_finished);
  EXPECT_TRUE(step.for_sleep);
  EXPECT_EQ(console.session().count("goto_bed"), 1U);
  EXPECT_EQ(rest.outcome(), RestRequest::Outcome::sent);
}

TEST(ConsoleRestMode, FinishesAtOnceWhenTheConsoleRefusesIt) {
  ConsoleRig refusing;
  refusing.session().refused_command = ConsoleRig::index_of<
      libchiaking::net::protocol::remote_play::command::GotoBed>();
  EXPECT_TRUE(refusing.commands().rest().request(RestRequest::Reason::leaving));
  const auto step = refusing.streaming();
  EXPECT_TRUE(step.rest_finished);
  EXPECT_EQ(refusing.commands().rest().outcome(),
            RestRequest::Outcome::refused);
  EXPECT_EQ(
      refusing.logged("Console rest mode request: result=invalid_argument"),
      1U);
}

TEST(ConsoleRestMode, IsRefusedWhileTheSessionSleeps) {
  ConsoleRig sleeping;
  EXPECT_TRUE(sleeping.lifecycle().sleep());
  EXPECT_FALSE(
      sleeping.commands().rest().request(RestRequest::Reason::host_sleep));
  EXPECT_EQ(sleeping.commands().rest().outcome(),
            RestRequest::Outcome::refused);
}

}  // namespace
}  // namespace play_test
