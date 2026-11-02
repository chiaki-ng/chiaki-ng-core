#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::Firmware;

class ConsoleAddress : public KnownConsolesTest {
 protected:
  void SetUp() override {
    ASSERT_TRUE(known.store.save(saved(den, "Den", Firmware::Family::ps4_10)));
    console = known.consoles.remember_saved().value().consoles.front();
    EXPECT_FALSE(console->last_address());
  }

  std::shared_ptr<libchiaking::users::PsUser> user =
      known.signed_in.as("npsso", 7);
  std::shared_ptr<Console> console;
};

TEST_F(ConsoleAddress, IsKeptAndSaved) {
  ASSERT_TRUE(console->moved_to(host("192.168.1.30")));
  EXPECT_EQ(console->last_address(), host("192.168.1.30"));
  EXPECT_EQ(known.store.console(hardware_id(den))->last_address,
            host("192.168.1.30"));
}

TEST_F(ConsoleAddress, RefusesIpv6ForAPs4) {
  ASSERT_TRUE(console->moved_to(host("192.168.1.30")));
  EXPECT_EQ(console->moved_to(host("fe80::9")).error(),
            Console::Unchanged::address_not_for_generation);
  EXPECT_EQ(console->last_address(), host("192.168.1.30"));
}

TEST_F(ConsoleAddress, StaysWhenTheMoveCannotBeSaved) {
  ASSERT_TRUE(console->moved_to(host("192.168.1.30")));
  known.store.refuse_console_saves = true;
  EXPECT_EQ(console->moved_to(host("192.168.1.31")).error(),
            Console::Unchanged::not_saved);
  EXPECT_EQ(console->last_address(), host("192.168.1.30"));
}

TEST_F(ConsoleAddress, TakesIpv6ForAPs5) {
  const auto ps5 = known.paired(*user, attic).value();
  ASSERT_TRUE(ps5->moved_to(host("fe80::9")));
  EXPECT_TRUE(known.store.console(hardware_id(attic))->last_address->is_ipv6());
}

TEST_F(ConsoleAddress, FollowsASighting) {
  const auto ps5 = known.paired(*user, attic).value();
  ASSERT_TRUE(ps5->moved_to(host("fe80::9")));
  ASSERT_EQ(known.sighted(attic, "PS5-Attic", "192.168.1.77"), ps5);
  EXPECT_EQ(ps5->last_address(), host("192.168.1.77"));
  EXPECT_EQ(known.store.console(hardware_id(attic))->last_address,
            host("192.168.1.77"));
}

}  // namespace
}  // namespace consoles_test
