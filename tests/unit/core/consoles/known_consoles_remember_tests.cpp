#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::ConsoleStore;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::KnownConsoles;
using libchiaking::consoles::Presence;
using libchiaking::consoles::SavedPairing;

static_assert(!std::is_default_constructible_v<Console>);
static_assert(!std::is_copy_constructible_v<Console> &&
              !std::is_move_constructible_v<Console>);
static_assert(
    !std::is_constructible_v<Console, libchiaking::consoles::SavedConsole>);
static_assert(!std::is_copy_constructible_v<KnownConsoles>);

class KnownConsolesRemembered : public KnownConsolesTest {
 protected:
  void SetUp() override {
    auto row = saved(den, "Den", Firmware::Family::ps4_9,
                     ConsoleName::Source::renamed);
    row.last_address = host("192.168.1.30", Generation::ps4);
    ASSERT_TRUE(known.store.save(row));
    ASSERT_TRUE(known.store.save(saved(attic, "Attic")));
    ASSERT_TRUE(known.store.save(SavedPairing{AccountId{7}, hardware_id(den),
                                              keys(1), DeviceUid::main_ps4()}));
    ASSERT_TRUE(known.store.save(
        SavedPairing{AccountId{8}, hardware_id(den), keys(3), std::nullopt}));
    const auto remembered = known.consoles.remember_saved();
    ASSERT_TRUE(remembered);
    EXPECT_EQ(remembered.value().consoles.size(), 2U);
    EXPECT_EQ(remembered.value().rejected, 0U);
  }
};

TEST_F(KnownConsolesRemembered, AreEverySavedConsole) {
  EXPECT_EQ(known.consoles.everything().size(), 2U);
  EXPECT_FALSE(known.consoles.find(hardware_id("111111111111")));
}

TEST_F(KnownConsolesRemembered, KeepEverySavedFact) {
  const auto console = known.consoles.find(hardware_id(den));
  ASSERT_TRUE(console);
  EXPECT_EQ(console->hardware_id(), hardware_id(den));
  EXPECT_EQ(console->generation(), Generation::ps4);
  EXPECT_EQ(console->firmware().family(), Firmware::Family::ps4_9);
  EXPECT_EQ(console->name(), "Den");
  EXPECT_EQ(console->name_source(), ConsoleName::Source::renamed);
  EXPECT_EQ(console->last_address(), host("192.168.1.30", Generation::ps4));
  EXPECT_EQ(console->presence(), Presence::unknown);
  EXPECT_FALSE(console->running_title());
  EXPECT_TRUE(console->is_saved());
  EXPECT_TRUE(console->is_paired());
  EXPECT_FALSE(console->is_forgotten());
  EXPECT_EQ(console->paired_accounts().size(), 2U);
}

TEST_F(KnownConsolesRemembered, KeepEachAccountsPairing) {
  const auto console = known.consoles.find(hardware_id(den));
  ASSERT_TRUE(console);
  const auto seven = known.signed_in.as("npsso-seven", 7);
  const auto nine = known.signed_in.as("npsso-nine", 9);
  EXPECT_TRUE(console->is_paired_with(*seven));
  EXPECT_FALSE(console->is_paired_with(*nine));
  const auto held = known.facts.keys_of(*console, *seven);
  ASSERT_TRUE(held);
  EXPECT_TRUE(
      std::ranges::equal(held->registration_key(), keys(1).registration_key()));
  EXPECT_TRUE(
      std::ranges::equal(held->remote_play_key(), keys(1).remote_play_key()));
  EXPECT_EQ(held->key_type(), 2U);
  EXPECT_EQ(known.facts.device_uid_of(*console, *seven), DeviceUid::main_ps4());
  EXPECT_FALSE(known.facts.keys_of(*console, *nine));
}

TEST_F(KnownConsolesRemembered, KeepAnUnpairedConsoleWithoutAnAddress) {
  const auto other = known.consoles.find(hardware_id(attic));
  ASSERT_TRUE(other);
  EXPECT_FALSE(other->is_paired());
  EXPECT_EQ(other->generation(), Generation::ps5);
  EXPECT_FALSE(other->last_address());
}

class KnownConsolesFromTheStore : public KnownConsolesTest {};

TEST_F(KnownConsolesFromTheStore, ReportAnUnavailableStore) {
  known.store.unavailable = true;
  EXPECT_EQ(known.consoles.remember_saved().error(),
            ConsoleStore::Error::unavailable);
  EXPECT_TRUE(known.consoles.everything().empty());
}

TEST_F(KnownConsolesFromTheStore, HoldOneConsolePerHardwareId) {
  ASSERT_TRUE(known.store.save(saved(den, "Den")));
  const auto first = known.consoles.remember_saved();
  const auto again = known.consoles.remember_saved();
  ASSERT_TRUE(first);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().rejected, 0U);
  EXPECT_EQ(known.consoles.everything().size(), 1U);
  ASSERT_FALSE(first.value().consoles.empty());
  ASSERT_FALSE(again.value().consoles.empty());
  EXPECT_EQ(first.value().consoles.front(), again.value().consoles.front());

  const auto sighted =
      known.sighted("A1:B2:C3:D4:E5:F6", "PS5-Den", "192.168.1.40");
  EXPECT_EQ(sighted, first.value().consoles.front());
  const auto user = known.signed_in.as("npsso", 7);
  const auto paired = known.paired(*user, den);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired.value(), sighted);
  EXPECT_EQ(known.consoles.everything().size(), 1U);
}

TEST_F(KnownConsolesFromTheStore, AddANewConsoleOnceWhenSighted) {
  ASSERT_TRUE(known.store.save(saved(den, "Den")));
  ASSERT_TRUE(known.consoles.remember_saved());
  const auto sighted =
      known.sighted("A1:B2:C3:D4:E5:F6", "PS5-Den", "192.168.1.40");
  const auto fresh = known.sighted(attic, "PS5-Attic", "192.168.1.41");
  EXPECT_NE(fresh, sighted);
  EXPECT_EQ(known.sighted(attic, "PS5-Attic", "192.168.1.41"), fresh);
  EXPECT_EQ(known.consoles.everything().size(), 2U);
  EXPECT_FALSE(fresh->is_saved());
  EXPECT_EQ(known.store.console_count(), 1U);
}

TEST_F(KnownConsolesFromTheStore, CountRejectedRows) {
  known.store.seed(saved(den, "Den"));
  known.store.seed(saved(den, "Den again"));
  auto unreachable = saved(attic, "Attic", Firmware::Family::ps4_10);
  unreachable.last_address = host("fe80::2");
  known.store.seed(std::move(unreachable));
  known.store.seed(
      SavedPairing{AccountId{7}, hardware_id(den), keys(1), std::nullopt});
  known.store.seed(
      SavedPairing{AccountId{7}, hardware_id(den), keys(5), std::nullopt});
  known.store.seed(
      SavedPairing{AccountId{7}, hardware_id(attic), keys(1), std::nullopt});
  known.store.seed(SavedPairing{AccountId{7}, hardware_id("222222222222"),
                                keys(1), std::nullopt});
  known.store.seed(
      SavedPairing{AccountId{0}, hardware_id(den), keys(1), std::nullopt});

  const auto remembered = known.consoles.remember_saved();
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().consoles.size(), 1U);
  EXPECT_EQ(remembered.value().rejected, 6U);
  const auto console = known.consoles.find(hardware_id(den));
  ASSERT_TRUE(console);
  EXPECT_EQ(console->name(), "Den");
  EXPECT_EQ(console->paired_accounts().size(), 1U);
  const auto user = known.signed_in.as("npsso", 7);
  const auto held = known.facts.keys_of(*console, *user);
  ASSERT_TRUE(held);
  EXPECT_TRUE(
      std::ranges::equal(held->registration_key(), keys(1).registration_key()));
  EXPECT_FALSE(known.consoles.find(hardware_id(attic)));
}

}  // namespace
}  // namespace consoles_test
