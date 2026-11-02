#include "tests/support/consoles/paired_consoles.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::RegisteredConsole;
using remote_play::RegistrationTarget;

static_assert(!std::is_copy_constructible_v<RegisteredConsole>);
static_assert(!std::is_copy_assignable_v<RegisteredConsole>);
static_assert(std::is_nothrow_move_constructible_v<RegisteredConsole>);

class RegisteredConsoleWipe : public ::testing::Test {
 protected:
  class RawConsole {
   public:
    RawConsole()
        : bytes_(static_cast<unsigned char*>(
              ::operator new(sizeof(RegisteredConsole),
                             std::align_val_t{alignof(RegisteredConsole)}))) {
      std::fill_n(bytes_, sizeof(RegisteredConsole),
                  static_cast<unsigned char>(0xee));
    }
    ~RawConsole() {
      ::operator delete(bytes_, std::align_val_t{alignof(RegisteredConsole)});
    }
    RawConsole(const RawConsole&) = delete;
    auto operator=(const RawConsole&) -> RawConsole& = delete;

    auto storage() noexcept -> void* { return bytes_; }

    auto wiped() const noexcept -> bool { return secrets_wiped(bytes_); }

    auto non_zero() const noexcept -> std::size_t {
      const volatile unsigned char* observed = bytes_;
      std::size_t total = 0;
      for (std::size_t index = 0; index < sizeof(RegisteredConsole); ++index)
        total += observed[index] != 0 ? 1U : 0U;
      return total;
    }

   private:
    unsigned char* bytes_;
  };

  static auto secrets_wiped(const unsigned char* console) noexcept -> bool {
    const std::array<std::pair<std::size_t, std::size_t>, 3> secrets{{
        {offsetof(RegisteredConsole, ap_key),
         sizeof(RegisteredConsole::ap_key)},
        {offsetof(RegisteredConsole, registration_key),
         sizeof(RegisteredConsole::registration_key)},
        {offsetof(RegisteredConsole, key), sizeof(RegisteredConsole::key)},
    }};
    const volatile unsigned char* observed = console;
    for (const auto& [offset, size] : secrets)
      for (std::size_t index = offset; index < offset + size; ++index)
        if (observed[index] != 0) return false;
    return true;
  }

  static void fill(RegisteredConsole& console) {
    console.target = RegistrationTarget::ps4_10;
    console.ap_ssid.fill('s');
    console.ap_bssid.fill('b');
    console.ap_name.fill('n');
    const std::string_view nickname = "PS4-Living";
    std::ranges::copy(nickname, console.nickname.begin());
    console.ap_key.fill('k');
    console.mac = {std::byte{0x7a}, std::byte{0x1b}, std::byte{0x2c},
                   std::byte{0x3d}, std::byte{0x4e}, std::byte{0x5f}};
    console.registration_key.fill(std::byte{0x11});
    console.key.fill(std::byte{0xa0});
    console.key_type = 2;
    console.console_pin = 24680;
  }

  static auto keys_are_zero(const RegisteredConsole& console) -> bool {
    return std::ranges::all_of(
               console.registration_key,
               [](std::byte value) { return value == std::byte{0}; }) &&
           std::ranges::all_of(
               console.key,
               [](std::byte value) { return value == std::byte{0}; }) &&
           std::ranges::all_of(console.ap_key,
                               [](char value) { return value == '\0'; });
  }
};

TEST_F(RegisteredConsoleWipe, WipesEverySecretOnDestruction) {
  RawConsole raw;
  auto* console = new (raw.storage()) RegisteredConsole;
  fill(*console);
  EXPECT_EQ(console->console_pin, 24680);
  EXPECT_EQ(console->nickname[0], 'P');
  EXPECT_GT(raw.non_zero(), sizeof(RegisteredConsole) / 2);
  console->~RegisteredConsole();
  EXPECT_TRUE(raw.wiped());
}

TEST_F(RegisteredConsoleWipe, LeavesNoKeyInAMovedFromConsole) {
  RawConsole source_raw;
  RawConsole target_raw;
  auto* source = new (source_raw.storage()) RegisteredConsole;
  fill(*source);
  auto* target =
      new (target_raw.storage()) RegisteredConsole{std::move(*source)};
  EXPECT_TRUE(keys_are_zero(*source));
  EXPECT_EQ(target->console_pin, 24680);
  EXPECT_EQ(target->registration_key[15], std::byte{0x11});
  EXPECT_EQ(target->key[0], std::byte{0xa0});
  EXPECT_EQ(target->ap_key[79], 'k');
  EXPECT_EQ(target->mac[0], std::byte{0x7a});
  EXPECT_EQ(target->mac[5], std::byte{0x5f});
  source->~RegisteredConsole();
  EXPECT_TRUE(source_raw.wiped());

  RawConsole assigned_raw;
  auto* assigned = new (assigned_raw.storage()) RegisteredConsole;
  assigned->registration_key.fill(std::byte{0x77});
  *assigned = std::move(*target);
  EXPECT_TRUE(keys_are_zero(*target));
  EXPECT_EQ(assigned->registration_key[0], std::byte{0x11});
  EXPECT_EQ(assigned->console_pin, 24680);
  target->~RegisteredConsole();
  EXPECT_TRUE(target_raw.wiped());
  assigned->~RegisteredConsole();
  EXPECT_TRUE(assigned_raw.wiped());
}

TEST_F(RegisteredConsoleWipe, WipesTheConsoleOfAReleasedRegisteredEvent) {
  using Event = remote_play::StreamEvent;
  auto* raw = static_cast<unsigned char*>(
      ::operator new(sizeof(Event), std::align_val_t{alignof(Event)}));
  remote_play::event::Registered registered;
  fill(registered.console);
  auto* event = new (raw) Event{std::move(registered)};
  const auto* held = std::get_if<remote_play::event::Registered>(event);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->console.console_pin, 24680);
  const auto* console = reinterpret_cast<const unsigned char*>(&held->console);
  EXPECT_GE(static_cast<const void*>(console), static_cast<const void*>(raw));
  EXPECT_LE(static_cast<const void*>(console + sizeof(RegisteredConsole)),
            static_cast<const void*>(raw + sizeof(Event)));
  const auto offset = static_cast<std::size_t>(console - raw);
  event->~Event();
  EXPECT_TRUE(secrets_wiped(raw + offset));
  ::operator delete(raw, std::align_val_t{alignof(Event)});
}

TEST_F(RegisteredConsoleWipe, WipesTheKeysOfAReleasedHomeConfiguration) {
  using Configuration = libchiaking::play::HomeLinkConfiguration;
  test_support::consoles::PairedConsoles consoles;
  const auto player = SignedInPlayer::fresh();
  const auto console =
      consoles.paired(*player, "aabbccddee01",
                      libchiaking::consoles::Generation::ps5, "192.168.1.10");
  auto built = Configuration::create(*console, *player, {}, {});
  ASSERT_TRUE(built);
  auto* raw = static_cast<unsigned char*>(::operator new(
      sizeof(Configuration), std::align_val_t{alignof(Configuration)}));
  auto* held = new (raw) Configuration{std::move(built.value())};
  EXPECT_TRUE(keys_are_zero(built.value().console));
  EXPECT_EQ(held->console.registration_key[0],
            test_support::consoles::PairedConsoles::registration_byte);
  EXPECT_EQ(held->console.key[15],
            test_support::consoles::PairedConsoles::remote_play_byte);
  EXPECT_EQ(held->console.target, RegistrationTarget::ps5_1);
  const auto* kept = reinterpret_cast<const unsigned char*>(&held->console);
  EXPECT_GE(static_cast<const void*>(kept), static_cast<const void*>(raw));
  EXPECT_LE(static_cast<const void*>(kept + sizeof(RegisteredConsole)),
            static_cast<const void*>(raw + sizeof(Configuration)));
  const auto offset = static_cast<std::size_t>(kept - raw);
  held->~Configuration();
  EXPECT_TRUE(secrets_wiped(raw + offset));
  ::operator delete(raw, std::align_val_t{alignof(Configuration)});
}

TEST_F(RegisteredConsoleWipe, RefusesAHomeConfigurationForAnotherAccount) {
  using Configuration = libchiaking::play::HomeLinkConfiguration;
  test_support::consoles::PairedConsoles consoles;
  const auto player = SignedInPlayer::fresh();
  const auto console =
      consoles.paired(*player, "aabbccddee01",
                      libchiaking::consoles::Generation::ps5, "192.168.1.10");
  const auto stranger = SignedInPlayer::fresh(
      SignedInPlayer::saved(SignedInPlayer::npsso, SignedInPlayer::device,
                            libchiaking::AccountId{99}));
  const auto refused = Configuration::create(*console, *stranger, {}, {});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), libchiaking::play::ConfigurationError::not_paired);
}

}  // namespace
}  // namespace play_test
