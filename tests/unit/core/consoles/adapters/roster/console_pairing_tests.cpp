#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::ConsoleStore;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::PairedConsole;

class ConsolePairingFirstSave : public KnownConsolesTest {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> user =
      known.signed_in.as("npsso", 7);
};

class ConsolePairingSave : public KnownConsolesTest {
 protected:
  void pair_the_nearby_console() {
    ASSERT_EQ(
        known.facts
            .paired(
                *user,
                PairedConsole{
                    hardware_id(attic), Firmware{Firmware::Family::ps5_1},
                    name("Nickname", ConsoleName::Source::pairing), keys(1)},
                host("192.168.1.99"), DeviceUid::main_ps4())
            .value(),
        nearby);
  }

  std::shared_ptr<libchiaking::users::PsUser> user =
      known.signed_in.as("npsso", 7);
  std::shared_ptr<Console> nearby =
      known.sighted(attic, "PS5-Attic", "192.168.1.41");
};

TEST_F(ConsolePairingFirstSave, KeepsNothingWhenTheConsoleCannotBeSaved) {
  known.store.refuse_console_saves = true;
  EXPECT_EQ(known.paired(*user, den).error(), ConsoleStore::Error::unavailable);
  EXPECT_TRUE(known.consoles.everything().empty());
  EXPECT_EQ(known.store.console_count(), 0U);
  EXPECT_EQ(known.store.pairing_count(), 0U);
}

TEST_F(ConsolePairingFirstSave, KeepsNothingWhenThePairingCannotBeSaved) {
  known.store.refuse_pairing_saves = true;
  EXPECT_EQ(known.paired(*user, den).error(), ConsoleStore::Error::unavailable);
  EXPECT_TRUE(known.consoles.everything().empty());
  EXPECT_EQ(known.store.console_count(), 0U);
  EXPECT_EQ(known.store.pairing_count(), 0U);
}

TEST_F(ConsolePairingSave, LeavesASightedConsoleUnsavedWhenThePairingFails) {
  known.store.refuse_pairing_saves = true;
  EXPECT_EQ(known.paired(*user, attic).error(),
            ConsoleStore::Error::unavailable);
  EXPECT_EQ(known.consoles.find(hardware_id(attic)), nearby);
  EXPECT_FALSE(nearby->is_saved());
  EXPECT_FALSE(nearby->is_paired());
  EXPECT_EQ(nearby->name(), "PS5-Attic");
  EXPECT_EQ(known.store.console_count(), 0U);
  EXPECT_EQ(known.store.pairing_count(), 0U);
}

TEST_F(ConsolePairingSave, SavesTheConsoleThenThePairing) {
  ASSERT_NO_FATAL_FAILURE(pair_the_nearby_console());
  EXPECT_TRUE(nearby->is_saved());
  EXPECT_TRUE(nearby->is_paired_with(*user));
  EXPECT_EQ(nearby->name(), "PS5-Attic");
  EXPECT_EQ(nearby->last_address(), host("192.168.1.99"));
  EXPECT_EQ(known.store.console(hardware_id(attic))->last_address,
            host("192.168.1.99"));
  const auto row = known.store.pairing(AccountId{7}, hardware_id(attic));
  ASSERT_TRUE(row);
  EXPECT_EQ(row->device_uid, DeviceUid::main_ps4());
  EXPECT_TRUE(std::ranges::equal(row->keys.remote_play_key(),
                                 keys(1).remote_play_key()));
}

TEST_F(ConsolePairingSave, KeepsTheOldKeysWhenARepairCannotBeSaved) {
  ASSERT_NO_FATAL_FAILURE(pair_the_nearby_console());
  known.store.refuse_pairing_saves = true;
  EXPECT_EQ(known.paired(*user, attic, 9).error(),
            ConsoleStore::Error::unavailable);
  known.store.refuse_pairing_saves = false;
  const auto held = known.facts.keys_of(*nearby, *user);
  ASSERT_TRUE(held);
  EXPECT_TRUE(
      std::ranges::equal(held->registration_key(), keys(1).registration_key()));
  EXPECT_EQ(known.store.console(hardware_id(attic))->name,
            name("PS5-Attic", ConsoleName::Source::discovered));
}

TEST_F(ConsolePairingSave, ReplacesTheKeysAndKeepsTheDeviceUidOnARepair) {
  ASSERT_NO_FATAL_FAILURE(pair_the_nearby_console());
  ASSERT_EQ(known.paired(*user, attic, 9).value(), nearby);
  const auto held = known.facts.keys_of(*nearby, *user);
  ASSERT_TRUE(held);
  EXPECT_TRUE(
      std::ranges::equal(held->registration_key(), keys(9).registration_key()));
  EXPECT_EQ(known.facts.device_uid_of(*nearby, *user), DeviceUid::main_ps4());
  EXPECT_EQ(known.store.pairing(AccountId{7}, hardware_id(attic))->device_uid,
            DeviceUid::main_ps4());
  EXPECT_EQ(known.store.pairing_count(), 1U);
}

class ConsoleDeviceUid : public KnownConsolesTest {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> user =
      known.signed_in.as("npsso", 7);
  std::shared_ptr<libchiaking::users::PsUser> stranger =
      known.signed_in.as("npsso-other", 9);
  std::shared_ptr<Console> console = known.paired(*user, den).value();
  DeviceUid uid =
      DeviceUid::parse(std::string(DeviceUid::text_length, 'e')).value();
};

TEST_F(ConsoleDeviceUid, IsUnknownUntilLearned) {
  EXPECT_FALSE(known.facts.device_uid_of(*console, *user));
}

TEST_F(ConsoleDeviceUid, IsNotLearnedForAnUnpairedUser) {
  EXPECT_EQ(known.facts.linked(*console, *stranger, uid).error(),
            Console::Unchanged::not_paired);
}

TEST_F(ConsoleDeviceUid, IsNotLearnedWhenItCannotBeSaved) {
  known.store.refuse_pairing_saves = true;
  EXPECT_EQ(known.facts.linked(*console, *user, uid).error(),
            Console::Unchanged::not_saved);
  EXPECT_FALSE(known.facts.device_uid_of(*console, *user));
}

TEST_F(ConsoleDeviceUid, IsSavedWithThePairing) {
  ASSERT_TRUE(known.facts.linked(*console, *user, uid));
  EXPECT_EQ(known.facts.device_uid_of(*console, *user), uid);
  EXPECT_EQ(known.store.pairing(AccountId{7}, hardware_id(den))->device_uid,
            uid);
  const auto saves = known.store.pairing_saves;
  EXPECT_TRUE(known.facts.linked(*console, *user, uid));
  EXPECT_EQ(known.store.pairing_saves, saves);
}

class ConsoleOffline : public KnownConsolesTest {};

TEST_F(ConsoleOffline, CanBePairedByAUserWhoNeverReachesPsn) {
  const auto user = known.signed_in.as("npsso", 4242);
  const auto console = known.paired(*user, den).value();
  EXPECT_TRUE(console->is_paired_with(*user));
  ASSERT_FALSE(console->paired_accounts().empty());
  EXPECT_EQ(console->paired_accounts().front(), AccountId{4242});
  EXPECT_TRUE(known.facts.keys_of(*console, *user));
  EXPECT_TRUE(console->rename("Den"));
  EXPECT_TRUE(console->moved_to(host("192.168.1.30")));
  EXPECT_EQ(known.sighted(den, "PS5-Den", "192.168.1.31"), console);
  EXPECT_TRUE(known.consoles.forget(*console));
  EXPECT_EQ(known.signed_in.offline.requests, 0);
}

}  // namespace
}  // namespace consoles_test
