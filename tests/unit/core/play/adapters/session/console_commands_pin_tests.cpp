#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using libchiaking::unexpected;
using libchiaking::net::protocol::remote_play::StreamError;
using play::InputRefusal;
using play::LoginPin;

class ConsoleLoginPin : public testing::Test {
 protected:
  auto pin() -> LoginPin::Core& { return console_.commands().pin(); }

  void reject_a_pin() {
    EXPECT_TRUE(pin().submit(ConsoleRig::code("4567")));
    console().negotiating();
    console().session().pin_result = unexpected(StreamError::pin_rejected);
    EXPECT_TRUE(pin().submit(ConsoleRig::code("2468")));
    console().negotiating();
  }

  auto console() -> ConsoleRig& { return console_; }

 private:
  ConsoleRig console_;
};

TEST_F(ConsoleLoginPin, WaitsTenSecondsForAVerdictByDefault) {
  EXPECT_EQ(LoginPin::default_await, 10s);
}

TEST_F(ConsoleLoginPin, IsRefusedBeforeTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::Venue::home(), false};
  EXPECT_TRUE(ConsoleRig::refused(
      idle.commands().pin().submit(ConsoleRig::code("1111")),
      InputRefusal::not_running));
  EXPECT_EQ(idle.commands().pin().await(), LoginPin::Verdict::accepted);
}

TEST_F(ConsoleLoginPin, SendsOnlyTheNewestPinAndOnlyOnce) {
  const auto superseded = pin().submit(ConsoleRig::code("1111"));
  const auto current = pin().submit(ConsoleRig::code("4567"));
  ASSERT_TRUE(superseded);
  ASSERT_TRUE(current);
  EXPECT_FALSE(superseded.value() == current.value());
  EXPECT_EQ(pin().progress(superseded.value()), LoginPin::Progress::rejected);
  EXPECT_EQ(pin().progress(current.value()), LoginPin::Progress::pending);
  EXPECT_TRUE(pin().queued());
  EXPECT_EQ(console().session().wakes, 2);
  console().negotiating();
  EXPECT_EQ(console().session().count("pin"), 1U);
  EXPECT_EQ(console().session().pin, "4567");
  EXPECT_FALSE(pin().queued());
  EXPECT_EQ(pin().progress(current.value()), LoginPin::Progress::accepted);
  EXPECT_EQ(pin().await(), LoginPin::Verdict::accepted);
  EXPECT_EQ(pin().await(), LoginPin::Verdict::accepted);
  console().negotiating();
  EXPECT_EQ(console().session().count("pin"), 1U);
}

TEST_F(ConsoleLoginPin, ReportsAPinTheConsoleRejected) {
  const auto current = pin().submit(ConsoleRig::code("4567"));
  ASSERT_TRUE(current);
  console().negotiating();

  console().session().pin_result = unexpected(StreamError::pin_rejected);
  const auto wrong = pin().submit(ConsoleRig::code("2468"));
  ASSERT_TRUE(wrong);
  EXPECT_EQ(pin().progress(wrong.value()), LoginPin::Progress::pending);
  EXPECT_EQ(pin().progress(current.value()), LoginPin::Progress::rejected);
  console().negotiating();
  EXPECT_EQ(pin().progress(wrong.value()), LoginPin::Progress::rejected);
  EXPECT_EQ(console().session().count("pin"), 2U);
  EXPECT_EQ(pin().await(), LoginPin::Verdict::rejected);
  EXPECT_EQ(
      console().logged("login code submission failed: result=pin_rejected"),
      1U);
}

TEST_F(ConsoleLoginPin, SendsNothingOnceTheWaitForAVerdictGaveUp) {
  reject_a_pin();
  EXPECT_TRUE(pin().submit(ConsoleRig::code("1357")));
  EXPECT_EQ(pin().await(0ms), LoginPin::Verdict::rejected);
  console().negotiating();
  EXPECT_EQ(console().session().count("pin"), 2U);
}

TEST_F(ConsoleLoginPin, RejectsAQueuedPinWhenTheSessionFinishes) {
  reject_a_pin();
  EXPECT_TRUE(pin().submit(ConsoleRig::code("1357")));
  EXPECT_EQ(pin().await(0ms), LoginPin::Verdict::rejected);
  console().negotiating();

  const auto late = pin().submit(ConsoleRig::code("9753"));
  ASSERT_TRUE(late);
  EXPECT_EQ(pin().progress(late.value()), LoginPin::Progress::pending);
  console().lifecycle().finish();
  EXPECT_EQ(pin().progress(late.value()), LoginPin::Progress::rejected);
  EXPECT_EQ(pin().await(), LoginPin::Verdict::rejected);
  console().negotiating();
  EXPECT_EQ(console().session().count("pin"), 2U);
}

}  // namespace
}  // namespace play_test
