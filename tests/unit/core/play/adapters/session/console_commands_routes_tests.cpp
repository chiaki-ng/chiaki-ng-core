#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using libchiaking::controllers::ControllerKind;
using play::InputRefusal;
using play::RestRequest;

TEST(ConsoleCommandRoutes, RefuseEverythingBeforeTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::Venue::home(), false};
  EXPECT_EQ(idle.commands().rest().outcome(), RestRequest::Outcome::none);
  EXPECT_TRUE(ConsoleRig::refused(idle.commands().go_home(),
                                  InputRefusal::not_running));
  EXPECT_FALSE(idle.commands().rest().request(RestRequest::Reason::leaving));
  EXPECT_EQ(idle.commands().rest().outcome(), RestRequest::Outcome::refused);
}

TEST(ConsoleCommandRoutes, KeepTheHomeScreenAndKeyboardOffTheCloud) {
  ConsoleRig cloud{ConsoleRig::cloud()};
  EXPECT_TRUE(ConsoleRig::refused(cloud.commands().go_home(),
                                  InputRefusal::wrong_route));
  EXPECT_TRUE(ConsoleRig::refused(cloud.commands().keyboard().accept(),
                                  InputRefusal::wrong_route));
}

TEST(ConsoleCommandRoutes, OpenTheMicrophoneOnEitherCloudService) {
  ConsoleRig cloud{ConsoleRig::cloud()};
  EXPECT_TRUE(cloud.commands().microphone().open());
  EXPECT_TRUE(cloud.commands().microphone().connect());
  ConsoleRig old_service{
      ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  EXPECT_TRUE(old_service.commands().microphone().open());
  EXPECT_TRUE(old_service.commands().microphone().connect());
}

TEST(ConsoleCommandRoutes, RefuseRestModeOnTheCloudButTakeAPinAndAPlayer) {
  ConsoleRig cloud{ConsoleRig::cloud()};
  EXPECT_FALSE(
      cloud.commands().rest().request(RestRequest::Reason::host_sleep));
  EXPECT_TRUE(cloud.commands().pin().submit(ConsoleRig::code("1234")));
  EXPECT_TRUE(cloud.seats().add_player(
      ConsoleRig::seat(1), ControllerKind::dualsense, std::nullopt));
}

TEST(ConsoleCommandRoutes, SendTheHomeScreenRequestOnceStreaming) {
  ConsoleRig remote{ConsoleRig::Venue::internet()};
  EXPECT_TRUE(remote.commands().go_home());
  EXPECT_TRUE(remote.commands().pending(true));
  EXPECT_FALSE(remote.commands().pending(false));
  EXPECT_TRUE(remote.negotiating().refusals.empty());
  EXPECT_EQ(remote.session().count("go_home"), 0U);
  remote.streaming();
  EXPECT_EQ(remote.session().count("go_home"), 1U);
  EXPECT_FALSE(remote.commands().pending(true));
  EXPECT_EQ(remote.logged("Console home screen request: result=ok"), 1U);
}

}  // namespace
}  // namespace play_test
