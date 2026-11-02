#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <libchiaking/detail/consoles/known_consoles.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace consoles_test {

class Values {
 public:
  static auto hardware_id(std::string_view text)
      -> libchiaking::consoles::HardwareId {
    return libchiaking::consoles::HardwareId::parse(text).value();
  }

  static auto name(std::string_view text,
                   libchiaking::consoles::ConsoleName::Source source)
      -> libchiaking::consoles::ConsoleName {
    return libchiaking::consoles::ConsoleName::create(std::string{text}, source)
        .value();
  }

  static auto host(std::string_view text,
                   libchiaking::consoles::Generation generation =
                       libchiaking::consoles::Generation::ps5)
      -> libchiaking::consoles::Host {
    return libchiaking::consoles::Host::create(text, generation).value();
  }

  static auto keys(std::uint8_t seed) -> libchiaking::consoles::PairingKeys {
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::registration_key_size>
        registration{};
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::remote_play_key_size>
        remote_play{};
    registration.fill(static_cast<std::byte>(seed));
    remote_play.fill(static_cast<std::byte>(seed + 1U));
    return libchiaking::consoles::PairingKeys::create(registration, remote_play,
                                                      2)
        .value();
  }

  static auto saved(std::string_view id, std::string_view named,
                    libchiaking::consoles::Firmware::Family family =
                        libchiaking::consoles::Firmware::Family::ps5_1,
                    libchiaking::consoles::ConsoleName::Source source =
                        libchiaking::consoles::ConsoleName::Source::discovered)
      -> libchiaking::consoles::SavedConsole {
    return libchiaking::consoles::SavedConsole{
        hardware_id(id), libchiaking::consoles::Firmware{family},
        name(named, source), std::nullopt};
  }
};

class Known {
 public:
  Known()
      : consoles(libchiaking::testing::ServiceBuilder::build<
                 libchiaking::consoles::KnownConsoles>(
            network, signed_in.offline, crypto, random, store, threading,
            steady)),
        facts(consoles) {}

  auto sighted(std::string_view id, std::string_view named,
               std::string_view address, bool in_rest_mode = false)
      -> std::shared_ptr<libchiaking::consoles::Console> {
    return facts.sighted(
        {Values::hardware_id(id),
         libchiaking::consoles::Firmware{
             libchiaking::consoles::Firmware::Family::ps5_1},
         Values::name(named,
                      libchiaking::consoles::ConsoleName::Source::discovered),
         Values::host(address), in_rest_mode, std::nullopt});
  }

  auto paired(const libchiaking::users::PsUser& as, std::string_view id,
              std::uint8_t seed = 1)
      -> libchiaking::Expected<std::shared_ptr<libchiaking::consoles::Console>,
                               libchiaking::consoles::ConsoleStore::Error> {
    return facts.paired(
        as,
        libchiaking::consoles::PairedConsole{
            Values::hardware_id(id),
            libchiaking::consoles::Firmware{
                libchiaking::consoles::Firmware::Family::ps5_1},
            Values::name("Nickname",
                         libchiaking::consoles::ConsoleName::Source::pairing),
            Values::keys(seed)});
  }

  libchiaking::platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  libchiaking::platform::SteadyClock steady;
  test_support::users::SignedIn signed_in;
  libchiaking::platform::DefaultNetworkProvider network{threading, steady};
  test_support::NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryConsoleStore store{threading};
  libchiaking::consoles::KnownConsoles consoles;
  libchiaking::consoles::ConsoleFacts facts;
};

class KnownConsolesTest : public ::testing::Test, public Values {
 protected:
  static constexpr std::string_view den = "a1b2c3d4e5f6";
  static constexpr std::string_view attic = "0a0b0c0d0e0f";

  Known known;
};

}  // namespace consoles_test
