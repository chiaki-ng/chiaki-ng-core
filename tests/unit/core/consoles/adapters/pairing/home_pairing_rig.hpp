#pragma once

#include "src/core/consoles/adapters/pairing/home_pairing.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/consoles/adapters/pairing/home_console.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace consoles_test {

class PairingTime {
 public:
  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto now() -> std::chrono::microseconds {
    return std::chrono::microseconds{steady().now_microseconds()};
  }

  static void pause(std::chrono::nanoseconds duration) {
    libchiaking::platform::Sleeper{threading(), steady()}.sleep_for(duration);
  }

  static auto account(std::uint8_t first)
      -> std::optional<libchiaking::consoles::HomePairing::AccountBytes> {
    std::optional<libchiaking::consoles::HomePairing::AccountBytes> bytes;
    bytes.emplace();
    for (std::size_t index = 0; index < bytes->size(); ++index)
      (*bytes)[index] = static_cast<std::byte>(first + index);
    return bytes;
  }

  static auto request(
      libchiaking::net::protocol::remote_play::RegistrationTarget target,
      std::optional<libchiaking::consoles::HomePairing::AccountBytes> account,
      std::optional<std::string> online_id, std::uint32_t pin,
      std::uint32_t console_pin)
      -> libchiaking::consoles::HomePairing::Request {
    return libchiaking::consoles::HomePairing::Request::with(
        target, std::move(account), std::move(online_id),
        libchiaking::consoles::PairingPin::create(pin).value(), console_pin);
  }
};

}  // namespace consoles_test
