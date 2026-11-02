#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Presence;
using libchiaking::consoles::RunningTitle;

class ConsolePresence : public KnownConsolesTest {
 protected:
  std::shared_ptr<Console> console =
      known.sighted(den, "PS5-Den", "192.168.1.40");
};

TEST_F(ConsolePresence, IsReadyWhenSighted) {
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_FALSE(console->running_title());
}

TEST_F(ConsolePresence, CarriesTheRunningTitle) {
  ASSERT_EQ(
      known.facts.sighted({hardware_id(den), Firmware{Firmware::Family::ps5_1},
                           std::nullopt, host("192.168.1.40"), false,
                           RunningTitle{"PPSA01234_00", "A Game"}}),
      console);
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_EQ(console->running_title(), (RunningTitle{"PPSA01234_00", "A Game"}));
  EXPECT_EQ(console->name(), "PS5-Den");
}

TEST_F(ConsolePresence, IsRestModeWithoutATitle) {
  ASSERT_EQ(
      known.facts.sighted({hardware_id(den), Firmware{Firmware::Family::ps5_1},
                           std::nullopt, host("192.168.1.40"), false,
                           RunningTitle{"PPSA01234_00", "A Game"}}),
      console);
  ASSERT_EQ(known.sighted(den, "PS5-Den", "192.168.1.40", true), console);
  EXPECT_EQ(console->presence(), Presence::rest_mode);
  EXPECT_FALSE(console->running_title());
}

TEST_F(ConsolePresence, FollowsTheConsoleOutOfSightAndBack) {
  known.facts.out_of_sight(*console, true);
  EXPECT_EQ(console->presence(), Presence::unknown);
  known.facts.out_of_sight(*console, false);
  EXPECT_EQ(console->presence(), Presence::not_nearby);
  ASSERT_EQ(known.sighted(den, "PS5-Den", "192.168.1.40"), console);
  EXPECT_EQ(console->presence(), Presence::ready);
}

TEST_F(ConsolePresence, IsUnknownOnceForgotten) {
  ASSERT_TRUE(known.consoles.forget(*console));
  EXPECT_EQ(console->presence(), Presence::unknown);
}

}  // namespace
}  // namespace consoles_test
