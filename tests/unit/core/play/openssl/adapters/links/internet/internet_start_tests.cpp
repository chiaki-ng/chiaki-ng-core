#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/adapters/links/internet/remote_pairing_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
using libchiaking::net::protocol::remote_play::RegistrationTarget;
using libchiaking::play::InternetStart;
using Rig = RemotePairingRig;

TEST(InternetStart, TurnsARegisteredConsoleIntoAPairedConsole) {
  const auto paired = InternetStart::paired_console(Rig::registered_console(),
                                                    RegistrationTarget::ps5_1);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired->hardware_id.text(), "7a1b2c3d4e5f");
  EXPECT_EQ(paired->firmware.generation(), consoles::Generation::ps5);
  ASSERT_TRUE(paired->name);
  EXPECT_EQ(paired->name->text(), "PS4-Living");
  EXPECT_EQ(paired->name->source(), consoles::ConsoleName::Source::pairing);
  EXPECT_EQ(paired->keys.registration_key()[0], std::byte{1});
  EXPECT_EQ(paired->keys.registration_key()[15], std::byte{16});
  EXPECT_EQ(paired->keys.remote_play_key()[0], std::byte{0xa0});
  EXPECT_EQ(paired->keys.remote_play_key()[15], std::byte{0xaf});
  EXPECT_EQ(paired->keys.key_type(), 2U);
}

TEST(InternetStart, PairsAPs4RegistrationAsAPs4) {
  auto older = Rig::registered_console();
  older.target = RegistrationTarget::ps4_10;
  const auto ps4 =
      InternetStart::paired_console(older, RegistrationTarget::ps4_10);
  ASSERT_TRUE(ps4);
  EXPECT_EQ(ps4->firmware.generation(), consoles::Generation::ps4);
}

TEST(InternetStart, RefusesARegistrationWithoutAnAddressOrKeys) {
  auto nameless = Rig::registered_console();
  nameless.mac = {};
  EXPECT_FALSE(
      InternetStart::paired_console(nameless, RegistrationTarget::ps5_1));
  auto keyless = Rig::registered_console();
  keyless.registration_key = {};
  keyless.key = {};
  EXPECT_FALSE(
      InternetStart::paired_console(keyless, RegistrationTarget::ps5_1));
}

}  // namespace
}  // namespace play_test
