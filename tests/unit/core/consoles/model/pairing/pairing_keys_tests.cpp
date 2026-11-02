#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::HardwareId;
using libchiaking::consoles::PairingKeys;
using libchiaking::consoles::SavedPairing;

static_assert(!std::is_default_constructible_v<PairingKeys>);
static_assert(!std::is_copy_constructible_v<PairingKeys> &&
              std::is_move_constructible_v<PairingKeys>);
static_assert(!std::is_copy_constructible_v<SavedPairing>);
static_assert(
    !std::is_copy_constructible_v<libchiaking::consoles::PairedConsole>);

class PairingKeysHeld : public ::testing::Test {
 protected:
  PairingKeysHeld() {
    registration.fill(std::byte{0x31});
    remote_play.fill(std::byte{0x7e});
  }

  static auto wiped(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(
        bytes, [](std::byte value) { return value == std::byte{0}; });
  }

  std::array<std::byte, PairingKeys::registration_key_size> registration{};
  std::array<std::byte, PairingKeys::remote_play_key_size> remote_play{};
};

TEST(PairingKeys, RefuseAKeyOfZeroBytes) {
  std::array<std::byte, PairingKeys::registration_key_size> registration{};
  std::array<std::byte, PairingKeys::remote_play_key_size> remote_play{};
  EXPECT_FALSE(PairingKeys::create(registration, remote_play, 2));
  registration.fill(std::byte{0x31});
  EXPECT_FALSE(PairingKeys::create(registration, remote_play, 2));
}

TEST_F(PairingKeysHeld, KeepBothKeysAndTheType) {
  const auto keys = PairingKeys::create(registration, remote_play, 2);
  ASSERT_TRUE(keys);
  EXPECT_TRUE(std::ranges::equal(keys->registration_key(), registration));
  EXPECT_TRUE(std::ranges::equal(keys->remote_play_key(), remote_play));
  EXPECT_EQ(keys->key_type(), 2U);
}

TEST_F(PairingKeysHeld, AreWipedWhenMoved) {
  auto keys = PairingKeys::create(registration, remote_play, 2);
  ASSERT_TRUE(keys);
  PairingKeys moved{std::move(*keys)};
  EXPECT_TRUE(std::ranges::equal(moved.registration_key(), registration));
  EXPECT_TRUE(std::ranges::equal(moved.remote_play_key(), remote_play));
  EXPECT_TRUE(wiped(keys->registration_key()));
  EXPECT_TRUE(wiped(keys->remote_play_key()));
}

TEST_F(PairingKeysHeld, KeepTheirCloneWhenMoved) {
  auto keys = PairingKeys::create(registration, remote_play, 2);
  ASSERT_TRUE(keys);
  const auto copy = keys->clone();
  PairingKeys moved{std::move(*keys)};
  EXPECT_TRUE(std::ranges::equal(copy.registration_key(), registration));
  EXPECT_TRUE(std::ranges::equal(copy.remote_play_key(), remote_play));
  EXPECT_EQ(copy.key_type(), 2U);
}

TEST_F(PairingKeysHeld, AreClonedWithTheirSavedPairing) {
  const auto keys = PairingKeys::create(registration, remote_play, 2);
  const auto console = HardwareId::parse("a1b2c3d4e5f6");
  ASSERT_TRUE(keys);
  ASSERT_TRUE(console);
  const SavedPairing pairing{libchiaking::AccountId{7}, *console, keys->clone(),
                             DeviceUid::main_ps4()};
  const auto cloned = pairing.clone();
  EXPECT_EQ(cloned.account, pairing.account);
  EXPECT_EQ(cloned.console, pairing.console);
  EXPECT_EQ(cloned.device_uid, pairing.device_uid);
  EXPECT_TRUE(std::ranges::equal(cloned.keys.remote_play_key(), remote_play));
}

}  // namespace
}  // namespace consoles_test
