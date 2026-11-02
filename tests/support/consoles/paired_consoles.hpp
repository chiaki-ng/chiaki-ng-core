#pragma once

#include "libchiaking/consoles.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "libchiaking/users.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace test_support::consoles {

class PairedConsoles {
 public:
  static constexpr std::byte registration_byte{0x31};
  static constexpr std::byte remote_play_byte{0xa0};

  PairedConsoles()
      : known(libchiaking::testing::ServiceBuilder::build<
              libchiaking::consoles::KnownConsoles>(
            network, http, crypto, random, store, threading, monotonic)) {}

  static auto keys() -> libchiaking::consoles::PairingKeys {
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::registration_key_size>
        registration{};
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::remote_play_key_size>
        remote_play{};
    registration.fill(registration_byte);
    remote_play.fill(remote_play_byte);
    return Setup::value(
        libchiaking::consoles::PairingKeys::create(registration, remote_play, 2),
        "the pairing keys are made");
  }

  auto paired(const libchiaking::users::PsUser& as,
              std::string_view hardware_id,
              libchiaking::consoles::Generation generation,
              std::string_view address = {},
              std::optional<libchiaking::consoles::DeviceUid> device_uid = {})
      -> std::shared_ptr<libchiaking::consoles::Console> {
    const auto id = Setup::value(
        libchiaking::consoles::HardwareId::parse(hardware_id),
        "the hardware id parses");
    libchiaking::consoles::SavedConsole row{
        id, libchiaking::consoles::Firmware::newest(generation),
        libchiaking::consoles::ConsoleName::of(generation), std::nullopt};
    if (!address.empty()) {
      row.last_address =
          Setup::value(libchiaking::consoles::Host::create(address, generation),
                       "the address is a host");
    }
    Setup::require(static_cast<bool>(store.save(row)), "the console is saved");
    Setup::require(static_cast<bool>(store.save(libchiaking::consoles::SavedPairing{
                       as.account_id(), id, keys(), device_uid})),
                   "the pairing is saved");
    Setup::require(static_cast<bool>(known.remember_saved()),
                   "the saved consoles are remembered");
    auto console = known.find(id);
    Setup::require(console != nullptr, "the paired console is known");
    return console;
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock monotonic;
  libchiaking::platform::DefaultNetworkProvider network{threading, monotonic};
  api::OfflineHttp http;
  NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryConsoleStore store{threading};
  libchiaking::consoles::KnownConsoles known;
};

}  // namespace test_support::consoles
