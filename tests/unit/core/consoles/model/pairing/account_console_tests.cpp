#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::AccountConsole;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Generation;

static_assert(!std::is_default_constructible_v<AccountConsole>);

class AccountConsoleListing : public ::testing::Test {
 protected:
  const DeviceUid uid =
      DeviceUid::parse(std::string(DeviceUid::text_length, 'd')).value();
};

TEST_F(AccountConsoleListing, KeepsWhatTheAccountLists) {
  const auto listed =
      AccountConsole::create(uid, "Living room", Generation::ps5, true);
  ASSERT_TRUE(listed);
  EXPECT_EQ(listed->name(), "Living room");
  EXPECT_EQ(listed->generation(), Generation::ps5);
  EXPECT_TRUE(listed->remote_play_enabled());
  EXPECT_FALSE(listed->is_main_ps4());
  EXPECT_EQ(listed->device_uid(), uid);
}

TEST_F(AccountConsoleListing, AcceptsAnEmptyName) {
  EXPECT_TRUE(AccountConsole::create(uid, "", Generation::ps4, false));
}

TEST_F(AccountConsoleListing, RefusesAnOverlongOrUnprintableName) {
  EXPECT_FALSE(AccountConsole::create(
      uid, std::string(AccountConsole::name_limit + 1, 'n'), Generation::ps5,
      true));
  EXPECT_FALSE(AccountConsole::create(uid, "bad\nname", Generation::ps5, true));
}

TEST_F(AccountConsoleListing, RefusesTheMainPs4IdentityForAPs5) {
  EXPECT_FALSE(AccountConsole::create(DeviceUid::main_ps4(), "Impossible",
                                      Generation::ps5, true));
}

TEST(AccountConsoleMainPs4, IsAPs4WithRemotePlayAndItsPlaceholder) {
  const auto main = AccountConsole::main_ps4();
  EXPECT_TRUE(main.is_main_ps4());
  EXPECT_EQ(main.generation(), Generation::ps4);
  EXPECT_TRUE(main.remote_play_enabled());
  EXPECT_EQ(main.name(), AccountConsole::main_ps4_name);
  EXPECT_EQ(main.device_uid(), DeviceUid::main_ps4());
}

}  // namespace
}  // namespace consoles_test
