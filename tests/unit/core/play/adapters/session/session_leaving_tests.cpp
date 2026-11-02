#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <gtest/gtest.h>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
using libchiaking::catalog::ServiceGeneration;
using play::EndReason;
using play::InputRefusal;
using play::Leave;
using play::Outcome;
using play::Where;

using PlaySessionLeaving = SessionCase;

TEST_F(PlaySessionLeaving, CanPutTheConsoleToRest) {
  SessionRig rig;
  rig.start_playing();
  EXPECT_TRUE(rig.session->go_home());
  SessionRig::eventually([&] { return rig.link.count("go_home") == 1; });
  EXPECT_FALSE(rig.session->audio_3d());
  EXPECT_TRUE(rig.session->set_audio_3d(true));
  EXPECT_TRUE(rig.session->audio_3d());
  EXPECT_TRUE(rig.session->leave(Leave::rest_console));
  const auto& outcome = rig.session->wait_until_ended();
  EXPECT_EQ(outcome.rest(), play::RestOutcome::sent);
  EXPECT_TRUE(outcome.was_left());
  EXPECT_TRUE(ended_as(outcome, Outcome::Ending::left));
  EXPECT_EQ(rig.link.count("rest"), 1U);
  EXPECT_EQ(rig.link.count("stop"), 1U);
  EXPECT_EQ(rig.link.count("stop_for_sleep"), 0U);
  EXPECT_EQ(rig.session->go_home().error(), InputRefusal::not_running);
}

TEST_F(PlaySessionLeaving, CannotRestACloudConsole) {
  SessionRig cloud{Where::cloud, ServiceGeneration::ps5_cloud};
  cloud.start_playing();
  EXPECT_EQ(cloud.session->go_home().error(), InputRefusal::wrong_route);
  EXPECT_TRUE(cloud.session->set_audio_3d(true));
  EXPECT_TRUE(cloud.session->audio_3d());
  EXPECT_FALSE(cloud.session->leave(Leave::rest_console));
  const auto& left = cloud.session->wait_until_ended();
  EXPECT_EQ(left.rest(), play::RestOutcome::refused);
  EXPECT_TRUE(ended_as(left, Outcome::Ending::left));
  EXPECT_EQ(cloud.link.count("rest"), 0U);
}

TEST_F(PlaySessionLeaving, StopsLocallyForHostSleep) {
  SessionRig sleeping;
  sleeping.start_playing();
  sleeping.session->leave_for_host_sleep(false);
  const auto& slept = sleeping.session->wait_until_ended();
  EXPECT_TRUE(ended_as(slept, Outcome::Ending::left));
  EXPECT_EQ(sleeping.link.count("stop_for_sleep"), 1U);
  EXPECT_EQ(sleeping.link.count("stop"), 0U);
}

TEST_F(PlaySessionLeaving, CanRestTheConsoleForHostSleep) {
  SessionRig resting;
  resting.start_playing();
  resting.session->leave_for_host_sleep(true);
  const auto& rested = resting.session->wait_until_ended();
  EXPECT_EQ(rested.rest(), play::RestOutcome::sent);
  EXPECT_EQ(resting.link.count("rest"), 1U);
  EXPECT_EQ(resting.link.count("stop_for_sleep"), 1U);
}

using PlaySessionEnding = SessionCase;

TEST_F(PlaySessionEnding, EndsWithTheReasonAConsoleQuit) {
  SessionRig rig;
  rig.start_playing();
  rig.link.quit(
      play::ConsoleQuit{play::ConsoleQuit::Reason::console_shut_down, "bye"});
  const auto& outcome = rig.session->wait_until_ended();
  EXPECT_EQ(outcome.ending(), Outcome::Ending::console_quit);
  EXPECT_TRUE(ended_as(outcome, play::ConsoleQuit::Reason::console_shut_down));
  ASSERT_TRUE(outcome.quit());
  EXPECT_EQ(outcome.quit()->reason(),
            play::ConsoleQuit::Reason::console_shut_down);
  EXPECT_EQ(outcome.quit()->text(), "bye");
}

TEST_F(PlaySessionEnding, EndsWithTheReasonTheLinkFailed) {
  SessionRig failing;
  failing.start_playing();
  failing.link.fail(EndReason{EndReason::Cause::control_timeout,
                              {.step = EndReason::Step::control_receive,
                               .console_reason = 0x80108b10,
                               .detail = "lost"}});
  const auto& failed = failing.session->wait_until_ended();
  EXPECT_EQ(failed.ending(), Outcome::Ending::session_failed);
  EXPECT_TRUE(ended_as(failed, Outcome::Ending::session_failed,
                       EndReason::Cause::control_timeout));
  ASSERT_NE(failed.reason(), nullptr);
  EXPECT_EQ(failed.reason()->step(), EndReason::Step::control_receive);
  EXPECT_EQ(failed.reason()->console_reason(), 0x80108b10);
  EXPECT_EQ(failed.reason()->detail(), "lost");
  ASSERT_TRUE(failed.quit());
  EXPECT_EQ(failed.quit()->reason(),
            play::ConsoleQuit::Reason::control_unknown);
  EXPECT_FALSE(failed.trace().empty());
}

TEST_F(PlaySessionEnding, EndsAsAFailedShutdownWhenCleanupFails) {
  SessionRig cleanup;
  cleanup.start_playing();
  cleanup.link.fail_cleanup(EndReason{EndReason::Cause::shutdown_failed,
                                      {.step = EndReason::Step::shutdown}});
  cleanup.link.finish();
  const auto& untidy = cleanup.session->wait_until_ended();
  EXPECT_EQ(untidy.ending(), Outcome::Ending::shutdown_failed);
  EXPECT_TRUE(ended_as(untidy, Outcome::Ending::shutdown_failed,
                       EndReason::Cause::shutdown_failed));
  ASSERT_NE(untidy.cleanup(), nullptr);
  EXPECT_EQ(untidy.cleanup()->step(), EndReason::Step::shutdown);
}

}  // namespace
}  // namespace play_test
