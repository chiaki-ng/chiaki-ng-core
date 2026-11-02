#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "src/core/consoles/adapters/pairing/home_pairing.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/consoles/adapters/pairing/home_console.hpp"

#include <libchiaking/detail/consoles/known_consoles.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_network.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

template <class Clock = libchiaking::platform::SteadyClock>
class Home {
 public:
  static constexpr std::uint32_t digits = 12345678;

  template <class... ClockArguments>
  explicit Home(ClockArguments&&... arguments)
      : clock(std::forward<ClockArguments>(arguments)...),
        known(libchiaking::testing::ServiceBuilder::build<
              libchiaking::consoles::KnownConsoles>(
            network, signed_in.offline, crypto, random, store,
            test_support::process_threading(), clock)),
        facts(known) {}

  static auto pin() -> libchiaking::consoles::PairingPin {
    return libchiaking::consoles::PairingPin::create(digits).value();
  }

  static auto host(std::string_view text,
                   libchiaking::consoles::Generation generation =
                       libchiaking::consoles::Generation::ps5)
      -> libchiaking::consoles::Host {
    return libchiaking::consoles::Host::create(text, generation).value();
  }

  static auto den() -> libchiaking::consoles::HardwareId {
    return libchiaking::consoles::HardwareId::parse("7a1b2c3d4e5f").value();
  }

  auto welcome() -> std::vector<std::byte> {
    return HomeConsole::welcome(
        crypto,
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1,
        digits, HomeConsole::ps5_facts);
  }

  auto nothing_is_live() -> testing::AssertionResult {
    if (!known.everything().empty())
      return testing::AssertionFailure()
             << known.everything().size() << " consoles are live";
    if (store.console_count() != 0)
      return testing::AssertionFailure()
             << store.console_count() << " consoles are saved";
    if (store.pairing_count() != 0)
      return testing::AssertionFailure()
             << store.pairing_count() << " pairings are saved";
    return testing::AssertionSuccess();
  }

  test_support::users::SignedIn signed_in;
  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryNetwork network{
      test_support::process_threading(), steady};
  XorCrypto crypto;
  test_support::CountingRandom random;
  libchiaking::testing::InMemoryConsoleStore store{
      test_support::process_threading()};
  Clock clock;
  test_support::users::SystemWall wall;
  libchiaking::consoles::KnownConsoles known;
  libchiaking::consoles::ConsoleFacts facts;
  libchiaking::platform::StopToken never;
};

}  // namespace consoles_test
