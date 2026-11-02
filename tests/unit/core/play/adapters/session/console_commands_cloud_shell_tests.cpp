#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using play::InputRefusal;
using Ui = play::Session::SystemUi;
using Press = play::Session::CreatePress;
using Capture = play::CloudMessaging::CapturePress;
using Accessibility = play::AccessibilityRequest;
using Links = std::vector<std::string>;

TEST(ConsoleSystemUi, SendsTheOverlayDeepLinksOnceOnTheCloud) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().set_system_ui(Ui::overlaid));
  EXPECT_TRUE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().deep_links,
            (Links{"psaction:systemservicestatus?isSystemUiOverlaid=1",
                   "psaction:systemservicestatus?isInBackgroundExecution=0"}));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_system_ui(Ui::overlaid));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_system_ui(Ui::game));
  console.streaming();
  ASSERT_EQ(console.session().deep_links.size(), 4U);
  EXPECT_EQ(console.session().deep_links[2],
            "psaction:systemservicestatus?isInBackgroundExecution=0");
  EXPECT_EQ(console.session().deep_links[3],
            "psaction:systemservicestatus?isSystemUiOverlaid=0");
}

TEST(ConsoleSystemUi, AsksThePsNowShellForItsPsPlusModeWhenTheStreamIsReady) {
  ConsoleRig ps_now{ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  ps_now.streaming();
  EXPECT_TRUE(ps_now.session().deep_links.empty());
  ps_now.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(ps_now.session().deep_links,
            (Links{"psaction:systemservicestatus?spartacus=1"}));
  EXPECT_EQ(ps_now.logged("system service status: result=ok"), 1U);
  for (const auto route : {ConsoleRig::cloud(), ConsoleRig::Venue::home(),
                           ConsoleRig::Venue::internet()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig other{route};
    other.streaming();
    other.commands().observe(remote_play::event::StreamInfoReady{});
    EXPECT_TRUE(other.session().deep_links.empty());
  }
}

TEST(ConsoleSystemUi, IsRefusedAtHomeAndOverTheInternet) {
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig local{route};
    EXPECT_TRUE(
        ConsoleRig::refused(local.commands().set_system_ui(Ui::overlaid),
                            InputRefusal::wrong_route));
    EXPECT_TRUE(
        ConsoleRig::refused(local.commands().set_system_ui(Ui::background),
                            InputRefusal::wrong_route));
    local.streaming();
    EXPECT_TRUE(local.session().deep_links.empty());
  }
}

TEST(ConsoleSystemUi, SendsTheScreenOffDeepLinksInTheBackground) {
  ConsoleRig console{ConsoleRig::cloud()};
  const auto& links = console.session().deep_links;
  EXPECT_TRUE(console.commands().set_system_ui(Ui::background));
  console.streaming();
  EXPECT_EQ(links,
            (Links{"psaction:systemservicestatus?isSystemUiOverlaid=0",
                   "psaction:systemservicestatus?isInBackgroundExecution=1"}));
  EXPECT_TRUE(console.commands().set_system_ui(Ui::overlaid));
  console.streaming();
  ASSERT_GE(links.size(), 2U);
  EXPECT_EQ((Links{links.begin() + 2, links.end()}),
            (Links{"psaction:systemservicestatus?isSystemUiOverlaid=1",
                   "psaction:systemservicestatus?isInBackgroundExecution=0"}));
  EXPECT_TRUE(console.commands().set_system_ui(Ui::background));
  EXPECT_TRUE(console.commands().set_system_ui(Ui::game));
  console.streaming();
  ASSERT_GE(links.size(), 4U);
  EXPECT_EQ((Links{links.begin() + 4, links.end()}),
            (Links{"psaction:systemservicestatus?isInBackgroundExecution=0",
                   "psaction:systemservicestatus?isSystemUiOverlaid=0"}));
}

TEST(ConsoleCreatePresses, ReachTheCloudLinkAsTheLatestPress) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().press_create(Press::single_tap));
  EXPECT_TRUE(console.commands().press_create(Press::long_press));
  EXPECT_TRUE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().creates,
            (std::vector<Capture>{Capture::long_press}));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().press_create(Press::double_tap));
  console.streaming();
  EXPECT_EQ(console.session().creates,
            (std::vector<Capture>{Capture::long_press, Capture::double_tap}));
}

TEST(ConsoleCreatePresses, AreRefusedBeforeTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::cloud(), false};
  EXPECT_TRUE(
      ConsoleRig::refused(idle.commands().press_create(Press::single_tap),
                          InputRefusal::not_running));
}

TEST(ConsoleCreatePresses, AreRefusedAtHomeAndOverTheInternet) {
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig local{route};
    EXPECT_TRUE(
        ConsoleRig::refused(local.commands().press_create(Press::single_tap),
                            InputRefusal::wrong_route));
  }
}

TEST(ConsoleAccessibility, ReachesTheCloudAsOneDeepLink) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{true, 5, 20, 2, true, false}));
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{true, 6, 20, 1, true, true}));
  EXPECT_TRUE(console.commands().pending(true));
  console.negotiating();
  EXPECT_TRUE(console.session().deep_links.empty());
  console.streaming();
  EXPECT_EQ(console.session().deep_links,
            (Links{"psaction:accessibility?tts=1&ttsspeed=6&ttsvolume=20&"
                   "textsize=1&boldtext=1&contrast=1"}));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{true, 6, 20, 1, true, true}));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{false, 3, 12, -1, false, false}));
  console.streaming();
  ASSERT_EQ(console.session().deep_links.size(), 2U);
  EXPECT_EQ(console.session().deep_links[1],
            "psaction:accessibility?tts=0&ttsspeed=3&ttsvolume=12&textsize=0&"
            "boldtext=0&contrast=0");
}

TEST(ConsoleAccessibility, SendsDefaultsForOutOfRangeSettings) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{true, 9, 25, 3, false, true}));
  console.streaming();
  EXPECT_TRUE(console.commands().set_accessibility(
      Accessibility{true, -1, -1, -2, true, false}));
  console.streaming();
  EXPECT_EQ(console.session().deep_links,
            (Links{"psaction:accessibility?tts=1&ttsspeed=3&ttsvolume=12&"
                   "textsize=0&boldtext=0&contrast=1",
                   "psaction:accessibility?tts=1&ttsspeed=3&ttsvolume=12&"
                   "textsize=0&boldtext=1&contrast=0"}));
}

TEST(ConsoleAccessibility, IsSentAgainAfterTheCloudDroppedIt) {
  ConsoleRig console{ConsoleRig::cloud()};
  console.session().deep_link_results.push_back(
      libchiaking::unexpected(remote_play::StreamError::ctrl_failed));
  const Accessibility wanted{true, 6, 20, 1, true, true};
  EXPECT_TRUE(console.commands().set_accessibility(wanted));
  console.streaming();
  ASSERT_EQ(console.session().deep_links.size(), 1U);
  EXPECT_TRUE(console.commands().set_accessibility(wanted));
  console.streaming();
  EXPECT_EQ(console.session().deep_links.size(), 2U);
}

TEST(ConsoleAccessibility, IsHeldUntilTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::cloud(), false};
  EXPECT_TRUE(idle.commands().set_accessibility(Accessibility{}));
  EXPECT_TRUE(idle.commands().pending(true));
}

TEST(ConsoleAccessibility, IsRefusedAtHomeAndOverTheInternet) {
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig local{route};
    EXPECT_TRUE(
        ConsoleRig::refused(local.commands().set_accessibility(Accessibility{}),
                            InputRefusal::wrong_route));
  }
}

TEST(ConsoleInvitations, ReachTheCloudShellAsAnInvitation) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().join_invitation("inv 1", "session-1"));
  console.streaming();
  EXPECT_EQ(console.session().invitations,
            (std::vector<std::pair<std::string, std::string>>{
                {"inv 1", "session-1"}}));
  EXPECT_TRUE(console.session().deep_links.empty());
}

TEST(ConsoleInvitations, ReachTheOldServiceShell) {
  ConsoleRig old_service{
      ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  EXPECT_TRUE(old_service.commands().join_invitation("inv-2", "session-2"));
  old_service.streaming();
  EXPECT_EQ(old_service.session().invitations,
            (std::vector<std::pair<std::string, std::string>>{
                {"inv-2", "session-2"}}));
}

TEST(ConsoleInvitations, AreRefusedByAConsole) {
  ConsoleRig home{ConsoleRig::Venue::home()};
  EXPECT_TRUE(
      ConsoleRig::refused(home.commands().join_invitation("inv-3", "session-3"),
                          InputRefusal::wrong_route));
}

class ConsolePsButton : public testing::Test {
 protected:
  static auto pressed(bool down) -> libchiaking::controllers::ControllerState {
    libchiaking::controllers::ControllerState state;
    if (down)
      state.buttons = static_cast<std::uint32_t>(
          libchiaking::controllers::ControllerButton::ps);
    return state;
  }
};

TEST_F(ConsolePsButton, TogglesThePressingPadsSystemMenuOnTheOldService) {
  ConsoleRig old_service{
      ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  for (const bool down : {true, true, false, true}) {
    SCOPED_TRACE(down);
    EXPECT_TRUE(old_service.queue().send(ConsoleRig::seat(0), pressed(down)));
    EXPECT_TRUE(old_service.queue().flush());
  }
  EXPECT_EQ(old_service.session().system_menu_toggles, 2);
  EXPECT_TRUE(old_service.queue().send(ConsoleRig::seat(1), pressed(true)));
  EXPECT_TRUE(old_service.queue().flush());
  EXPECT_EQ(old_service.session().system_menu_toggles, 3);
  EXPECT_EQ(old_service.session().system_menu_pads,
            (std::vector<std::uint32_t>{0, 0, 1}));
}

TEST_F(ConsolePsButton, LeavesTheSystemMenuAloneOnPs5Cloud) {
  ConsoleRig ps5_cloud{ConsoleRig::cloud()};
  EXPECT_TRUE(ps5_cloud.queue().send(ConsoleRig::seat(0), pressed(true)));
  EXPECT_TRUE(ps5_cloud.queue().flush());
  EXPECT_EQ(ps5_cloud.session().system_menu_toggles, 0);
}

TEST(ConsoleSavedData, IsRequestedOnceOnTheOldService) {
  ConsoleRig old_service{
      ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  EXPECT_TRUE(old_service.commands().open_saved_data());
  EXPECT_TRUE(old_service.commands().open_saved_data());
  old_service.streaming();
  EXPECT_EQ(old_service.session().saved_data_requests, 1);
}

TEST(ConsoleSavedData, IsRefusedOnPs5CloudAndAtHome) {
  ConsoleRig ps5_cloud{ConsoleRig::cloud()};
  EXPECT_TRUE(ConsoleRig::refused(ps5_cloud.commands().open_saved_data(),
                                  InputRefusal::wrong_route));
  ConsoleRig home{ConsoleRig::Venue::home()};
  EXPECT_TRUE(ConsoleRig::refused(home.commands().open_saved_data(),
                                  InputRefusal::wrong_route));
}

}  // namespace
}  // namespace play_test
