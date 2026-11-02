#include "tests/unit/api/consoles/titles_rig.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/consoles/start_states.hpp"

#include <gtest/gtest.h>

namespace api_consoles_test {
namespace {

using libchiaking::consoles::Firmware;
using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitlesUnavailable;
using libchiaking::users::Unanswered;

class ConsoleStart : public ConsoleTitlesTest {};

TEST_F(ConsoleStart, RefusesWhatCannotBeStarted) {
  const auto user = rig.signed_in(7);
  const auto home = rig.home(40);
  const auto console = rig.paired(living_room, Firmware::Family::ps5_1, 40);
  const auto old = rig.paired(old_console, Firmware::Family::ps4_10, 7);
  const auto title = TitlesRig::title("PPSA01234_00", "PPSA01234");
  ASSERT_TRUE(title);

  test_support::consoles::StartStates watched;
  const auto signed_out = console->start(*title, *home, &watched, {});
  ASSERT_FALSE(signed_out);
  EXPECT_EQ(signed_out.error().reason, StartFailure::Reason::unanswered);
  ASSERT_TRUE(signed_out.error().unanswered);
  EXPECT_EQ(signed_out.error().unanswered->reason,
            Unanswered::Reason::signed_out);
  EXPECT_EQ(signed_out.error().reached, StartState::pending);

  const auto not_paired = console->start(*title, *user, &watched, {});
  ASSERT_FALSE(not_paired);
  EXPECT_EQ(not_paired.error().reason, StartFailure::Reason::not_paired);
  EXPECT_EQ(not_paired.error().reached, StartState::pending);
  const auto no_titles = console->titles(*user, {});
  ASSERT_FALSE(no_titles);
  EXPECT_EQ(no_titles.error().reason, TitlesUnavailable::Reason::not_paired);

  const auto ps4 = old->start(*title, *user, &watched, {});
  ASSERT_FALSE(ps4);
  EXPECT_EQ(ps4.error().reason, StartFailure::Reason::ps5_only);
  EXPECT_EQ(ps4.error().reached, StartState::pending);
  const auto ps4_titles = old->titles(*user, {});
  ASSERT_FALSE(ps4_titles);
  EXPECT_EQ(ps4_titles.error().reason, TitlesUnavailable::Reason::ps5_only);

  EXPECT_TRUE(watched.states.empty());
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ConsoleStart, CancelledCallsReportCancelledWithoutTheToken) {
  const auto user = rig.signed_in(7);
  const auto console = rig.paired(living_room, Firmware::Family::ps5_1, 7);

  const auto titles = console->titles(*user, test_support::Stopped::token());
  ASSERT_FALSE(titles);
  EXPECT_EQ(titles.error().reason, TitlesUnavailable::Reason::unanswered);
  ASSERT_TRUE(titles.error().unanswered);
  EXPECT_EQ(titles.error().unanswered->reason, Unanswered::Reason::cancelled);
  const auto recent =
      rig.known.recently_played(*user, test_support::Stopped::token());
  ASSERT_FALSE(recent);
  EXPECT_EQ(recent.error().reason, Unanswered::Reason::cancelled);

  const auto title = TitlesRig::title("PPSA01234_00", "PPSA01234");
  ASSERT_TRUE(title);
  test_support::consoles::StartStates watched;
  const auto started =
      console->start(*title, *user, &watched, test_support::Stopped::token());
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().reason, StartFailure::Reason::cancelled);
  EXPECT_EQ(started.error().reached, StartState::pending);
  EXPECT_TRUE(watched.states.empty());

  EXPECT_TRUE(rig.never_logged_the_token());
  EXPECT_TRUE(rig.http.seen.empty());
}

}  // namespace
}  // namespace api_consoles_test
