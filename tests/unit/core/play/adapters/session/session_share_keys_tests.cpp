#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::SessionShare;

class PlaySessionReceiveKeys : public ShareCase {
 protected:
  static auto zero(std::byte value) -> bool { return value == std::byte{0}; }

  static auto keys() -> play::ReceiveKeys {
    libchiaking::SecretArray<play::ReceiveKeys::size> key;
    libchiaking::SecretArray<play::ReceiveKeys::size> iv;
    key.fill(std::byte{0x5a});
    iv.fill(std::byte{0xa5});
    return play::ReceiveKeys{std::move(key), std::move(iv)};
  }
};

TEST_F(PlaySessionReceiveKeys, WipeWhatTheyAreMadeFrom) {
  libchiaking::SecretArray<play::ReceiveKeys::size> key;
  libchiaking::SecretArray<play::ReceiveKeys::size> iv;
  key.fill(std::byte{0x5a});
  iv.fill(std::byte{0xa5});
  const play::ReceiveKeys keys{std::move(key), std::move(iv)};
  EXPECT_TRUE(std::ranges::all_of(key, zero));
  EXPECT_TRUE(std::ranges::all_of(iv, zero));
}

TEST_F(PlaySessionReceiveKeys, AreWipedWhenTheyLeave) {
  auto moved_from = keys();
  const auto copy = moved_from.clone();
  const play::ReceiveKeys taken{std::move(moved_from)};
  EXPECT_TRUE(std::ranges::all_of(moved_from.key(), zero));
  EXPECT_TRUE(std::ranges::all_of(moved_from.iv(), zero));
  EXPECT_TRUE(std::ranges::equal(taken.key(), copy.key()));
  EXPECT_TRUE(std::ranges::equal(taken.iv(), copy.iv()));
  EXPECT_TRUE(std::ranges::all_of(
      taken.key(), [](std::byte value) { return value == std::byte{0x5a}; }));
}

TEST_F(PlaySessionReceiveKeys, AreWipedWhenDestroyed) {
  const auto taken = keys();
  alignas(play::ReceiveKeys) std::array<std::byte, sizeof(play::ReceiveKeys)>
      storage{};
  auto* placed = new (storage.data()) play::ReceiveKeys{taken.clone()};
  EXPECT_FALSE(std::ranges::all_of(storage, zero));
  std::destroy_at(placed);
  EXPECT_TRUE(std::ranges::all_of(storage, zero));
}

TEST_F(PlaySessionReceiveKeys, AreCopiedFromTheWire) {
  const auto wired = SessionShare::Core::copied(receive_keys(3));
  EXPECT_TRUE(std::ranges::equal(wired.key(), receive_keys(3).key()));
  EXPECT_TRUE(std::ranges::equal(wired.iv(), receive_keys(3).iv()));
}

}  // namespace
}  // namespace play_test
