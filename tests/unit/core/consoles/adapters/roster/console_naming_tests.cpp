#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::PairedConsole;
using Source = ConsoleName::Source;

class ConsoleNaming : public KnownConsolesTest {
 protected:
  void SetUp() override {
    console =
        known.facts
            .paired(*user, PairedConsole{hardware_id(den),
                                         Firmware{Firmware::Family::ps5_1},
                                         std::nullopt, keys(1)})
            .value();
  }

  void reach_the_discovered_name() {
    known.facts.named(*console, name("PS's name", Source::account));
    ASSERT_EQ(known.paired(*user, den).value(), console);
    ASSERT_EQ(known.sighted(den, "PS5-Den", "192.168.1.40"), console);
  }

  void rename_to_study() {
    ASSERT_NO_FATAL_FAILURE(reach_the_discovered_name());
    ASSERT_TRUE(console->rename("Den"));
    ASSERT_TRUE(console->rename("Study"));
  }

  std::shared_ptr<libchiaking::users::PsUser> user =
      known.signed_in.as("npsso", 7);
  std::shared_ptr<Console> console;
};

TEST_F(ConsoleNaming, StartsFromTheGeneration) {
  EXPECT_EQ(console->name(), "PS5");
  EXPECT_EQ(console->name_source(), Source::generation);
}

TEST_F(ConsoleNaming, TakesTheAccountNameAndSavesIt) {
  known.facts.named(*console, name("PS's name", Source::account));
  EXPECT_EQ(console->name(), "PS's name");
  EXPECT_EQ(known.store.console(hardware_id(den))->name.text(), "PS's name");
}

TEST_F(ConsoleNaming, PrefersThePairingNameOverTheAccount) {
  known.facts.named(*console, name("PS's name", Source::account));
  ASSERT_EQ(known.paired(*user, den).value(), console);
  EXPECT_EQ(console->name(), "Nickname");
  EXPECT_EQ(console->name_source(), Source::pairing);
  known.facts.named(*console, name("PS's newer name", Source::account));
  EXPECT_EQ(console->name(), "Nickname");
}

TEST_F(ConsoleNaming, PrefersTheDiscoveredNameOverThePairing) {
  ASSERT_NO_FATAL_FAILURE(reach_the_discovered_name());
  EXPECT_EQ(console->name(), "PS5-Den");
  EXPECT_EQ(console->name_source(), Source::discovered);
  EXPECT_EQ(known.store.console(hardware_id(den))->name,
            name("PS5-Den", Source::discovered));
  ASSERT_EQ(known.paired(*user, den).value(), console);
  EXPECT_EQ(console->name(), "PS5-Den");
  ASSERT_EQ(known.sighted(den, "PS5-Den-2", "192.168.1.40"), console);
  EXPECT_EQ(console->name(), "PS5-Den-2");
}

TEST_F(ConsoleNaming, KeepsARenameOverEveryOtherSource) {
  ASSERT_NO_FATAL_FAILURE(reach_the_discovered_name());
  ASSERT_TRUE(console->rename("Den"));
  EXPECT_EQ(console->name(), "Den");
  EXPECT_EQ(console->name_source(), Source::renamed);
  ASSERT_EQ(known.sighted(den, "PS5-Den-3", "192.168.1.40"), console);
  known.facts.named(*console, name("PS's newest name", Source::account));
  ASSERT_EQ(known.paired(*user, den).value(), console);
  EXPECT_EQ(console->name(), "Den");
  EXPECT_EQ(known.store.console(hardware_id(den))->name,
            name("Den", Source::renamed));
}

TEST_F(ConsoleNaming, RefusesAnInvalidRename) {
  EXPECT_EQ(console->rename("").error(), Console::Unchanged::invalid_name);
  EXPECT_EQ(console->rename("bad\nname").error(),
            Console::Unchanged::invalid_name);
  EXPECT_TRUE(console->rename("Study"));
  EXPECT_EQ(console->name(), "Study");
}

TEST_F(ConsoleNaming, KeepsTheOldNameWhenTheRenameCannotBeSaved) {
  ASSERT_NO_FATAL_FAILURE(rename_to_study());
  known.store.refuse_console_saves = true;
  EXPECT_EQ(console->rename("Unsaved").error(), Console::Unchanged::not_saved);
  EXPECT_EQ(console->name(), "Study");
  EXPECT_EQ(known.store.console(hardware_id(den))->name.text(), "Study");
}

TEST_F(ConsoleNaming, KeepsARenameAcrossARestart) {
  ASSERT_NO_FATAL_FAILURE(rename_to_study());
  Known later;
  ASSERT_TRUE(later.store.save(*known.store.console(hardware_id(den))));
  const auto returned =
      later.consoles.remember_saved().value().consoles.front();
  EXPECT_EQ(returned->name(), "Study");
  EXPECT_EQ(returned->name_source(), Source::renamed);
  ASSERT_EQ(later.sighted(den, "PS5-Den-4", "192.168.1.40"), returned);
  EXPECT_EQ(returned->name(), "Study");
}

TEST_F(ConsoleNaming, SavesTheRenameOfAnUnsavedConsoleOnceItIsPaired) {
  const auto nearby = known.sighted(attic, "PS5-Attic", "192.168.1.41");
  const auto saves = known.store.console_saves;
  ASSERT_TRUE(nearby->rename("Attic"));
  EXPECT_EQ(nearby->name(), "Attic");
  EXPECT_EQ(known.store.console_saves, saves);
  EXPECT_FALSE(known.store.console(hardware_id(attic)));
  ASSERT_EQ(known.paired(*user, attic).value(), nearby);
  EXPECT_EQ(known.store.console(hardware_id(attic))->name,
            name("Attic", Source::renamed));
}

}  // namespace
}  // namespace consoles_test
