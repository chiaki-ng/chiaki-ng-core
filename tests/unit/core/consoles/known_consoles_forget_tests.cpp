#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleStore;

class KnownConsolesForget : public KnownConsolesTest {
 protected:
  void SetUp() override {
    console = known.paired(*seven, den).value();
    ASSERT_EQ(known.paired(*eight, den, 3).value(), console);
    ASSERT_TRUE(known.paired(*seven, attic));
    EXPECT_EQ(known.store.console_count(), 2U);
    EXPECT_EQ(known.store.pairing_count(), 3U);
  }

  std::shared_ptr<libchiaking::users::PsUser> seven =
      known.signed_in.as("npsso-seven", 7);
  std::shared_ptr<libchiaking::users::PsUser> eight =
      known.signed_in.as("npsso-eight", 8);
  std::shared_ptr<Console> console;
};

TEST_F(KnownConsolesForget, KeepsEverythingWhenTheStoreRefuses) {
  known.store.refuse_removals = true;
  EXPECT_EQ(known.consoles.forget(*console).error(),
            ConsoleStore::Error::unavailable);
  EXPECT_EQ(known.consoles.everything().size(), 2U);
  EXPECT_EQ(known.consoles.find(hardware_id(den)), console);
  EXPECT_TRUE(console->is_saved());
  EXPECT_FALSE(console->is_forgotten());
  EXPECT_TRUE(console->is_paired_with(*seven));
  EXPECT_TRUE(console->is_paired_with(*eight));
  EXPECT_EQ(known.store.console_count(), 2U);
  EXPECT_EQ(known.store.pairing_count(), 3U);
}

TEST_F(KnownConsolesForget, RemovesTheConsoleAndEveryPairing) {
  ASSERT_TRUE(known.consoles.forget(*console));
  EXPECT_EQ(known.consoles.everything().size(), 1U);
  EXPECT_FALSE(known.consoles.find(hardware_id(den)));
  EXPECT_TRUE(console->is_forgotten());
  EXPECT_FALSE(console->is_saved());
  EXPECT_FALSE(console->is_paired());
  EXPECT_FALSE(console->is_paired_with(*seven));
  EXPECT_FALSE(known.facts.keys_of(*console, *seven));
  EXPECT_FALSE(known.store.console(hardware_id(den)));
  EXPECT_FALSE(known.store.pairing(AccountId{7}, hardware_id(den)));
  EXPECT_FALSE(known.store.pairing(AccountId{8}, hardware_id(den)));
  EXPECT_EQ(known.store.console_count(), 1U);
  EXPECT_EQ(known.store.pairing_count(), 1U);
}

TEST_F(KnownConsolesForget, LeavesAForgottenConsoleUnchangeable) {
  ASSERT_TRUE(known.consoles.forget(*console));
  const auto removals = known.store.removals;
  EXPECT_TRUE(known.consoles.forget(*console));
  EXPECT_EQ(known.store.removals, removals);
  EXPECT_EQ(console->rename("Gone").error(), Console::Unchanged::forgotten);
  EXPECT_EQ(console->moved_to(host("10.0.0.9")).error(),
            Console::Unchanged::forgotten);
}

TEST_F(KnownConsolesForget, DropsAnUnsavedConsoleWithoutTheStore) {
  ASSERT_TRUE(known.consoles.forget(*console));
  const auto removals = known.store.removals;
  const auto nearby =
      known.sighted("333333333333", "PS5-Nearby", "192.168.1.60");
  EXPECT_TRUE(known.consoles.forget(*nearby));
  EXPECT_EQ(known.store.removals, removals);
  EXPECT_FALSE(known.consoles.find(hardware_id("333333333333")));
}

TEST_F(KnownConsolesForget, LeavesOnlyTheOtherConsoleToRemember) {
  ASSERT_TRUE(known.consoles.forget(*console));
  const auto back = known.consoles.remember_saved();
  ASSERT_TRUE(back);
  ASSERT_EQ(back.value().consoles.size(), 1U);
  EXPECT_EQ(back.value().rejected, 0U);
  EXPECT_EQ(back.value().consoles.front()->hardware_id(), hardware_id(attic));
}

class KnownConsolesUnpair : public KnownConsolesTest {
 protected:
  void SetUp() override {
    console = known.paired(*seven, den).value();
    ASSERT_EQ(known.paired(*eight, den, 3).value(), console);
    EXPECT_EQ(known.store.console_count(), 1U);
    EXPECT_EQ(known.store.pairing_count(), 2U);
  }

  std::shared_ptr<libchiaking::users::PsUser> seven =
      known.signed_in.as("npsso-seven", 7);
  std::shared_ptr<libchiaking::users::PsUser> eight =
      known.signed_in.as("npsso-eight", 8);
  std::shared_ptr<Console> console;
};

TEST_F(KnownConsolesUnpair, KeepsThePairingWhenTheStoreRefuses) {
  known.store.refuse_removals = true;
  EXPECT_EQ(known.consoles.unpair(AccountId{7}, *console).error(),
            ConsoleStore::Error::unavailable);
  EXPECT_TRUE(console->is_paired_with(*seven));
  EXPECT_TRUE(known.facts.keys_of(*console, *seven));
  EXPECT_TRUE(known.store.pairing(AccountId{7}, hardware_id(den)));
}

TEST_F(KnownConsolesUnpair, RemovesOneAccountAndKeepsTheConsole) {
  const auto removals = known.store.removals;
  ASSERT_TRUE(known.consoles.unpair(AccountId{7}, *console));
  EXPECT_EQ(known.store.removals, removals + 1);
  EXPECT_FALSE(console->is_paired_with(*seven));
  EXPECT_FALSE(known.facts.keys_of(*console, *seven));
  EXPECT_TRUE(console->is_paired_with(*eight));
  EXPECT_TRUE(console->is_saved());
  EXPECT_FALSE(console->is_forgotten());
  EXPECT_EQ(console->paired_accounts(), std::vector<AccountId>{AccountId{8}});
  EXPECT_EQ(known.consoles.find(hardware_id(den)), console);
  EXPECT_TRUE(known.store.console(hardware_id(den)));
  EXPECT_FALSE(known.store.pairing(AccountId{7}, hardware_id(den)));
  EXPECT_TRUE(known.store.pairing(AccountId{8}, hardware_id(den)));
  EXPECT_TRUE(known.consoles.unpair(AccountId{7}, *console));
  EXPECT_EQ(known.store.removals, removals + 1);
}

TEST_F(KnownConsolesUnpair, KeepsTheConsoleSavedWhenTheLastAccountLeaves) {
  ASSERT_TRUE(known.consoles.unpair(AccountId{7}, *console));
  ASSERT_TRUE(known.consoles.unpair(AccountId{8}, *console));
  EXPECT_FALSE(console->is_paired());
  EXPECT_TRUE(console->is_saved());
  EXPECT_TRUE(known.store.console(hardware_id(den)));
  EXPECT_EQ(known.store.pairing_count(), 0U);
  const auto back = known.consoles.remember_saved();
  ASSERT_TRUE(back);
  EXPECT_EQ(back.value().consoles.size(), 1U);
  EXPECT_FALSE(console->is_paired());
}

TEST_F(KnownConsolesUnpair, LeavesAnUnsavedConsoleAlone) {
  const auto nearby =
      known.sighted("333333333333", "PS5-Nearby", "192.168.1.60");
  const auto removals = known.store.removals;
  EXPECT_TRUE(known.consoles.unpair(AccountId{7}, *nearby));
  EXPECT_EQ(known.store.removals, removals);
}

TEST_F(KnownConsolesUnpair, LeavesAnotherRostersConsoleAlone) {
  Known elsewhere;
  EXPECT_TRUE(elsewhere.consoles.unpair(AccountId{7}, *console));
  EXPECT_TRUE(console->is_paired_with(*seven));
}

TEST_F(KnownConsolesUnpair, LeavesAForgottenConsoleAlone) {
  ASSERT_TRUE(known.consoles.forget(*console));
  const auto forgotten = known.store.removals;
  EXPECT_TRUE(known.consoles.unpair(AccountId{7}, *console));
  EXPECT_EQ(known.store.removals, forgotten);
}

TEST(KnownConsolesOwner, LeavesItsConsolesForgottenWhenItGoes) {
  std::shared_ptr<Console> console;
  {
    Known known;
    const auto user = known.signed_in.as("npsso", 7);
    console = known.paired(*user, "a1b2c3d4e5f6").value();
  }
  EXPECT_TRUE(console->is_forgotten());
  EXPECT_FALSE(console->is_paired());
  EXPECT_EQ(console->rename("Den").error(), Console::Unchanged::forgotten);
}

}  // namespace
}  // namespace consoles_test
